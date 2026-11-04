/* Guest file system: PS4 mount points mapped onto host folders.
 *   /app0, /hostapp  -> the merged game folder (read-only)
 *   /temp0, /download0, /data, and mounts added by SaveData -> the user folder
 * Guest descriptors are small integers in our own table; stdio 0-2 pass through.
 * Paths containing ".." components are rejected rather than normalized.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from bbport's src/runtime_file.c (github.com/deadinside28/bloodborne_pc) to Win32:
 * each file keeps its own position and every read/write passes an explicit offset, because a
 * Windows read at an offset moves the file pointer while a PS4 pread does not. */
#include "gz.h"
#include "snapshot_view.h"
#ifdef GZ_GPU
#include "gpu.h"
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILES 1024
#define MAX_MOUNTS 24
/* FreeBSD errno values seen by the guest. */
enum {
    E_NOENT = 2,
    E_IO = 5,
    E_BADF = 9,
    E_NOMEM = 12,
    E_ACCES = 13,
    E_FAULT = 14,
    E_EXIST = 17,
    E_NOTDIR = 20,
    E_ISDIR = 21,
    E_INVAL = 22,
    E_MFILE = 24,
    E_NOSPC = 28,
    E_ROFS = 30,
    E_NAMETOOLONG = 63,
    E_NOTEMPTY = 66
};

typedef struct {
    int64_t sec, nsec;
} GuestTimespec;
typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat) == 120, "FreeBSD stat layout");

typedef struct {
    char *names;
    size_t count, *offsets;
    unsigned char *types;
} Listing;
typedef struct {
    int used, append;
    HANDLE host;
    int64_t position;
    Listing *dir;
    char path[512];
    const unsigned char *snapshot_view;
} File;
typedef struct {
    char guest[64];
    char host[512];
} Mount;
static File files[MAX_FILES];
static Mount mounts[MAX_MOUNTS];
static size_t mount_count;
static volatile LONG64 opens, reads, writes, missing, bytes_read;
static SRWLOCK lock = SRWLOCK_INIT;
static char user_root[512];
static int trace_files;

static int from_windows(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        return E_NOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return E_ACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return E_EXIST;
    case ERROR_INVALID_HANDLE:
        return E_BADF;
    case ERROR_DIR_NOT_EMPTY:
        return E_NOTEMPTY;
    case ERROR_TOO_MANY_OPEN_FILES:
        return E_MFILE;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return E_NOSPC;
    case ERROR_DIRECTORY:
        return E_NOTDIR;
    case ERROR_FILENAME_EXCED_RANGE:
        return E_NAMETOOLONG;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return E_NOMEM;
    case ERROR_NOACCESS:
        return E_FAULT;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
        return E_INVAL;
    default:
        return E_IO;
    }
}
int gz_widen(const char *utf8, wchar_t *out, int count) {
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, count) ? 0 : -1;
}
static int is_directory(const wchar_t *path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
/* Creates a folder and its parents (UTF-8 path); 0 if it exists afterwards. */
int gz_make_dirs(const char *path) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char *p = buffer; *p; ++p) {
        if ((*p == '\\' || *p == '/') && p > buffer && p[-1] != ':') {
            char c = *p;
            *p = 0;
            wchar_t w[1024];
            if (!gz_widen(buffer, w, 1024))
                CreateDirectoryW(w, NULL);
            *p = c;
        }
    }
    wchar_t w[1024];
    if (gz_widen(buffer, w, 1024))
        return -1;
    CreateDirectoryW(w, NULL);
    return is_directory(w) ? 0 : -1;
}

int gz_file_mount(const char *guest, const char *host) {
    AcquireSRWLockExclusive(&lock);
    int r = 0;
    size_t i = 0;
    while (i < mount_count && strcmp(mounts[i].guest, guest))
        ++i;
    if (i == mount_count) {
        if (mount_count == MAX_MOUNTS || strlen(guest) >= 64 || strlen(host) >= 512)
            r = -1;
        else
            snprintf(mounts[mount_count++].guest, 64, "%s", guest);
    }
    if (!r)
        snprintf(mounts[i].host, 512, "%s", host);
    ReleaseSRWLockExclusive(&lock);
    return r;
}
void gz_file_unmount(const char *guest) {
    AcquireSRWLockExclusive(&lock);
    for (size_t i = 0; i < mount_count; ++i)
        if (!strcmp(mounts[i].guest, guest)) {
            mounts[i] = mounts[--mount_count];
            break;
        }
    ReleaseSRWLockExclusive(&lock);
}
const char *gz_user_dir(void) {
    return user_root;
}
void gz_file_configure(const char *app0, const char *user) {
    const char *trace = getenv("GZ_TRACE_FILES");
    trace_files = trace && !strcmp(trace, "1");
    snprintf(user_root, sizeof(user_root), "%s", user);
    gz_file_mount("/app0", app0);
    gz_file_mount("/hostapp", app0);
    /* Local, version-checked data fixes live with user data, never in /app0. */
    const char *menu_fix = getenv("GZ_MENU_FIX");
    if (!menu_fix || strcmp(menu_fix, "0")) {
        const char *archives[] = {"mn_option.pac", "game_bp.pac", "game_en.pac",
                                  "game_fr.pac",   "game_ns.pac", "mn_main_bp.pac"};
        for (size_t i = 0; i < sizeof(archives) / sizeof(archives[0]); ++i) {
            char override[1024], guest[128];
            wchar_t wide[1024];
            snprintf(override, sizeof(override), "%s\\patches\\%s", user, archives[i]);
            if (!gz_widen(override, wide, 1024) &&
                GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES) {
                snprintf(guest, sizeof(guest), "/app0/%s", archives[i]);
                if (gz_file_mount(guest, override))
                    gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: cannot mount local menu correction");
                snprintf(guest, sizeof(guest), "/hostapp/%s", archives[i]);
                if (gz_file_mount(guest, override))
                    gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: cannot mount local menu correction");
                gz_log("Runtime: local menu correction mounted: %s (GZ_MENU_FIX=0 disables)",
                       archives[i]);
            }
        }
    }
    const char *system_font = getenv("GZ_SYSTEM_FONT");
    if (system_font && *system_font) {
        wchar_t font_path[1024];
        if (gz_widen(system_font, font_path, 1024) ||
            GetFileAttributesW(font_path) == INVALID_FILE_ATTRIBUTES)
            gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: configured system font not found: %s",
                    system_font);
        if (gz_file_mount("/sys/common/font/SST-Roman.otf", system_font))
            gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: cannot mount configured system font");
        gz_log("Runtime: system font SST-Roman explicitly supplied by GZ_SYSTEM_FONT: %s",
               system_font);
    }
    const char *system_font_jp = getenv("GZ_SYSTEM_FONT_JP");
    if (system_font_jp && *system_font_jp) {
        wchar_t font_path[1024];
        if (gz_widen(system_font_jp, font_path, 1024) ||
            GetFileAttributesW(font_path) == INVALID_FILE_ATTRIBUTES)
            gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: configured Japanese system font not found: %s",
                    system_font_jp);
        if (gz_file_mount("/sys/common/font/SSTJpPro-Regular.otf", system_font_jp))
            gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: cannot mount configured Japanese system font");
        gz_log("Runtime: system font SSTJpPro explicitly supplied by GZ_SYSTEM_FONT_JP: %s",
               system_font_jp);
    }
    static const char *writable[] = {"temp0", "download0", "data"};
    for (int i = 0; i < 3; ++i) {
        char host[600], guest[32];
        snprintf(host, sizeof(host), "%s\\%s", user, writable[i]);
        if (gz_make_dirs(host))
            gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: cannot create the folder %s", host);
        snprintf(guest, sizeof(guest), "/%s", writable[i]);
        gz_file_mount(guest, host);
    }
}
/* Guest path -> host path (UTF-8, backslashes); 0 or a FreeBSD errno. */
int gz_file_translate(const char *guest, char *out, size_t size) {
    if (!guest || !*guest)
        return E_NOENT;
    char buffer[1024];
    if (guest[0] != '/')
        snprintf(buffer, sizeof(buffer), "/app0/%s", guest);
    else
        snprintf(buffer, sizeof(buffer), "%s", guest);
    for (const char *p = buffer; (p = strstr(p, "..")); p += 2)
        if ((p == buffer || p[-1] == '/') && (p[2] == 0 || p[2] == '/'))
            return E_ACCES;
    AcquireSRWLockShared(&lock);
    size_t best = 0;
    const Mount *m = NULL;
    for (size_t i = 0; i < mount_count; ++i) {
        size_t n = strlen(mounts[i].guest);
        if (!strncmp(buffer, mounts[i].guest, n) && (buffer[n] == '/' || !buffer[n]) && n > best) {
            best = n;
            m = &mounts[i];
        }
    }
    int result = 0;
    if (!m)
        result = E_NOENT;
    else if ((size_t)snprintf(out, size, "%s%s", m->host, buffer + best) >= size)
        result = E_NAMETOOLONG;
    ReleaseSRWLockShared(&lock);
    if (result == E_NOENT)
        gz_log("Runtime: no mount for guest path %s", guest);
    if (!result)
        for (char *p = out; *p; ++p)
            if (*p == '/')
                *p = '\\';
    return result;
}
static int game_path(const char *p) {
    if (!p || !*p)
        return 0;
    if (*p != '/')
        return 1;
    return (!strncmp(p, "/app0", 5) && (!p[5] || p[5] == '/')) ||
           (!strncmp(p, "/hostapp", 8) && (!p[8] || p[8] == '/')) ||
           (!strncmp(p, "/sys", 4) && (!p[4] || p[4] == '/'));
}
static void free_listing(Listing *l) {
    if (l) {
        free(l->names);
        free(l->offsets);
        free(l->types);
        free(l);
    }
}
static Listing *list_directory(const wchar_t *path) {
    wchar_t pattern[1100];
    _snwprintf(pattern, 1100, L"%ls\\*", path);
    WIN32_FIND_DATAW e;
    HANDLE find = FindFirstFileW(pattern, &e);
    if (find == INVALID_HANDLE_VALUE)
        return NULL;
    Listing *l = calloc(1, sizeof(*l));
    size_t capacity = 0, bytes = 0, name_capacity = 0;
    do {
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, e.cFileName, -1, name, sizeof(name), NULL, NULL))
            continue;
        size_t n = strlen(name) + 1;
        if (l->count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            l->offsets = realloc(l->offsets, capacity * sizeof(size_t));
            l->types = realloc(l->types, capacity);
        }
        if (bytes + n > name_capacity) {
            name_capacity = (bytes + n) * 2;
            l->names = realloc(l->names, name_capacity);
        }
        if (!l->offsets || !l->types || !l->names)
            gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: out of memory listing a folder");
        memcpy(l->names + bytes, name, n);
        l->offsets[l->count] = bytes;
        l->types[l->count] =
            (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8; /* DT_DIR / DT_REG */
        ++l->count;
        bytes += n;
    } while (FindNextFileW(find, &e));
    FindClose(find);
    return l;
}
static GuestTimespec from_filetime(FILETIME t) {
    uint64_t v = ((uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime) -
                 UINT64_C(116444736000000000); /* since 1970 */
    return (GuestTimespec){(int64_t)(v / 10000000), (int64_t)(v % 10000000) * 100};
}
static void convert_stat(DWORD attributes, uint64_t size, FILETIME access, FILETIME write,
                         FILETIME create, GuestStat *g) {
    memset(g, 0, sizeof(*g));
    int dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    int ro = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    g->mode = (uint16_t)(dir ? 0040000 | 0755 : 0100000 | (ro ? 0444 : 0644));
    g->nlink = 1;
    g->size = dir ? 0 : (int64_t)size;
    g->blksize = 16384;
    g->blocks = (g->size + 511) / 512;
    g->atime = from_filetime(access);
    g->mtime = from_filetime(write);
    g->ctime = g->mtime;
    g->birthtime = from_filetime(create);
}
static File *get(int fd) {
    return fd >= 3 && fd < MAX_FILES && files[fd].used ? &files[fd] : NULL;
}

/* All operations return >= 0 or -(FreeBSD errno); wrappers adapt the convention. */
static int64_t do_open(const char *guest, int flags, int mode) {
    (void)mode;
    if (game_path(guest) && (flags & (3 | 0x8 | 0x200 | 0x400 | 0x800)))
        return -E_ROFS;
    char path[1024];
    wchar_t wide[1024];
    int e = gz_file_translate(guest, path, sizeof(path));
    if (e)
        return -e;
    if (gz_widen(path, wide, 1024))
        return -E_NAMETOOLONG;
    DWORD access = (flags & 3) == 0   ? GENERIC_READ
                   : (flags & 3) == 1 ? GENERIC_WRITE
                                      : GENERIC_READ | GENERIC_WRITE;
    DWORD disposition = (flags & 0x200) ? ((flags & 0x800)   ? CREATE_NEW
                                           : (flags & 0x400) ? CREATE_ALWAYS
                                                             : OPEN_ALWAYS)
                                        : ((flags & 0x400) ? TRUNCATE_EXISTING : OPEN_EXISTING);
    int dir = is_directory(wide);
    if ((flags & 0x20000) && !dir)
        return -E_NOTDIR; /* O_DIRECTORY */
    HANDLE host = CreateFileW(wide, dir ? GENERIC_READ : access,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              dir ? OPEN_EXISTING : disposition,
                              dir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, NULL);
    if (host == INVALID_HANDLE_VALUE) {
        e = from_windows(GetLastError());
        if (e == E_NOENT) {
            InterlockedIncrement64(&missing);
            gz_log("Runtime: open(%s) -> not found", guest);
        }
        return -e;
    }
    Listing *listing = dir ? list_directory(wide) : NULL;
    const unsigned char *snapshot_view =
        !dir && access == GENERIC_READ ? gz_snapshot_view(host, path, user_root) : NULL;
    AcquireSRWLockExclusive(&lock);
    int fd = -1;
    for (int i = 3; i < MAX_FILES; ++i)
        if (!files[i].used) {
            fd = i;
            break;
        }
    if (fd < 0) {
        ReleaseSRWLockExclusive(&lock);
        CloseHandle(host);
        free_listing(listing);
        return -E_MFILE;
    }
    files[fd] = (File){.used = 1, .append = (flags & 0x8) != 0, .host = host, .dir = listing};
    files[fd].snapshot_view = snapshot_view;
    snprintf(files[fd].path, sizeof(files[fd].path), "%s", guest);
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement64(&opens);
    if (trace_files)
        gz_log("TEST FILE: tick=%llu fd=%d flags=0x%x path=%s",
               (unsigned long long)GetTickCount64(), fd, flags, guest);
    return fd;
}
static int64_t do_close(int fd) {
    if (fd >= 0 && fd < 3)
        return 0;
    AcquireSRWLockExclusive(&lock);
    File *f = get(fd);
    if (!f) {
        ReleaseSRWLockExclusive(&lock);
        return -E_BADF;
    }
    CloseHandle(f->host);
    free_listing(f->dir);
    *f = (File){0};
    ReleaseSRWLockExclusive(&lock);
    return 0;
}
/* Reads up to size bytes at offset (in chunks: ReadFile takes 32-bit sizes). */
static int64_t read_at(HANDLE h, void *buffer, uint64_t size, int64_t offset) {
    uint64_t done = 0;
    while (done < size) {
        DWORD chunk = size - done > 0x40000000 ? 0x40000000 : (DWORD)(size - done), n = 0;
        OVERLAPPED o = {0};
        o.Offset = (DWORD)(offset + done);
        o.OffsetHigh = (DWORD)((uint64_t)(offset + done) >> 32);
#ifdef GZ_GPU
        gz_gpu_prepare_write((char *)buffer + done, chunk);
#endif
        if (!ReadFile(h, (char *)buffer + done, chunk, &n, &o)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF)
                break;
            return done ? (int64_t)done : -from_windows(e);
        }
        done += n;
        if (n < chunk)
            break;
    }
    InterlockedIncrement64(&reads);
    InterlockedAdd64(&bytes_read, (LONG64)done);
    return (int64_t)done;
}
static int64_t write_at(HANDLE h, const void *buffer, uint64_t size, int64_t offset) {
#ifdef GZ_GPU
    gz_gpu_prepare_read(buffer, size);
#endif
    uint64_t done = 0;
    while (done < size) {
        DWORD chunk = size - done > 0x40000000 ? 0x40000000 : (DWORD)(size - done), n = 0;
        OVERLAPPED o = {0};
        o.Offset = (DWORD)(offset + done);
        o.OffsetHigh = (DWORD)((uint64_t)(offset + done) >> 32);
        if (!WriteFile(h, (const char *)buffer + done, chunk, &n, &o))
            return done ? (int64_t)done : -from_windows(GetLastError());
        done += n;
        if (n < chunk)
            break;
    }
    InterlockedIncrement64(&writes);
    return (int64_t)done;
}
static int64_t file_size(HANDLE h) {
    LARGE_INTEGER s;
    return GetFileSizeEx(h, &s) ? s.QuadPart : -1;
}
static int64_t read_file(File *f, void *buffer, uint64_t size, int64_t offset) {
    if (!f->snapshot_view)
        return read_at(f->host, buffer, size, offset);
    const uint64_t image_size = 480 * 270 * 4;
    if (offset < 0)
        return -E_INVAL;
    if ((uint64_t)offset >= image_size)
        return 0;
    if (size > image_size - (uint64_t)offset)
        size = image_size - (uint64_t)offset;
#ifdef GZ_GPU
    gz_gpu_prepare_write(buffer, size);
#endif
    memcpy(buffer, f->snapshot_view + offset, (size_t)size);
    InterlockedIncrement64(&reads);
    InterlockedAdd64(&bytes_read, (LONG64)size);
    return (int64_t)size;
}
static int64_t do_read(int fd, void *buffer, uint64_t size) {
    if (fd >= 0 && fd < 3)
        return 0; /* no console input */
    File *f = get(fd);
    if (!f)
        return -E_BADF;
    if (f->dir)
        return -E_ISDIR;
    int64_t n = read_file(f, buffer, size, f->position);
    if (n > 0)
        f->position += n;
    return n;
}
static int64_t do_pread(int fd, void *buffer, uint64_t size, int64_t offset) {
    File *f = get(fd);
    if (!f)
        return -E_BADF;
    if (offset < 0)
        return -E_INVAL;
    return read_file(f, buffer, size, offset);
}
static int64_t do_write(int fd, const void *buffer, uint64_t size) {
    if (fd == 1 || fd == 2) { /* the game's console output goes to our log */
        char text[1024];
        size_t n = size < sizeof(text) - 1 ? (size_t)size : sizeof(text) - 1;
        memcpy(text, buffer, n);
        text[n] = 0;
        while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
            text[--n] = 0;
        gz_log("Guest: %s", text);
        return (int64_t)size;
    }
    File *f = get(fd);
    if (!f)
        return -E_BADF;
    if (f->append)
        f->position = file_size(f->host);
    int64_t n = write_at(f->host, buffer, size, f->position);
    if (n > 0 && getenv("GZ_CAMERA_TRACE") && strstr(f->path, "\\area") &&
        strstr(f->path, ".dat")) {
        uint64_t nonzero = 0;
        const unsigned char *pixels = buffer;
        for (int64_t i = 0; i < n; ++i)
            nonzero += pixels[i] != 0;
        const char *name = strrchr(f->path, '\\');
        gz_log("Camera save: %s offset=%lld bytes=%lld nonzero=%llu", name ? name + 1 : f->path,
               (long long)f->position, (long long)n, (unsigned long long)nonzero);
    }
    if (n > 0)
        f->position += n;
    return n;
}
static int64_t do_pwrite(int fd, const void *buffer, uint64_t size, int64_t offset) {
    File *f = get(fd);
    if (!f)
        return -E_BADF;
    return offset < 0 ? -E_INVAL : write_at(f->host, buffer, size, offset);
}
static int64_t do_lseek(int fd, int64_t offset, int whence) {
    File *f = get(fd);
    if (!f)
        return -E_BADF;
    if (whence < 0 || whence > 2)
        return -E_INVAL;
    if (f->dir) { /* directory offsets are entry indices for getdirentries */
        if (whence == 0 && offset >= 0) {
            f->position = offset;
            return offset;
        }
        return -E_INVAL;
    }
    int64_t base = whence == 0 ? 0 : whence == 1 ? f->position : file_size(f->host);
    if (base < 0 || base + offset < 0)
        return -E_INVAL;
    f->position = base + offset;
    return f->position;
}
static int64_t do_fstat(int fd, GuestStat *out) {
    File *f = get(fd);
    if (!f)
        return -E_BADF;
    if (!out)
        return -E_FAULT;
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle(f->host, &i))
        return -from_windows(GetLastError());
    convert_stat(i.dwFileAttributes, (uint64_t)i.nFileSizeHigh << 32 | i.nFileSizeLow,
                 i.ftLastAccessTime, i.ftLastWriteTime, i.ftCreationTime, out);
    out->ino = i.nFileIndexLow;
    return 0;
}
static int64_t do_stat(const char *guest, GuestStat *out) {
    char path[1024];
    wchar_t wide[1024];
    int e = gz_file_translate(guest, path, sizeof(path));
    if (e)
        return -e;
    if (!out)
        return -E_FAULT;
    if (gz_widen(path, wide, 1024))
        return -E_NAMETOOLONG;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &a))
        return -from_windows(GetLastError());
    convert_stat(a.dwFileAttributes, (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow,
                 a.ftLastAccessTime, a.ftLastWriteTime, a.ftCreationTime, out);
    return 0;
}
static int64_t do_getdents(int fd, char *buffer, uint64_t size, int64_t *basep) {
    AcquireSRWLockExclusive(&lock);
    File *f = get(fd);
    int64_t result = 0;
    if (!f)
        result = -E_BADF;
    else if (!f->dir)
        result = -E_INVAL;
    else if (!buffer)
        result = -E_FAULT;
    else if (size < 512)
        result = -E_INVAL;
    else {
        if (basep)
            *basep = f->position;
        uint64_t written = 0;
        while ((size_t)f->position < f->dir->count) {
            const char *name = f->dir->names + f->dir->offsets[f->position];
            size_t n = strlen(name);
            if (n > 255)
                n = 255;
            uint16_t reclen = (uint16_t)((8 + n + 1 + 7) & ~(size_t)7);
            if (written + reclen > size)
                break;
            char *p = buffer + written;
            memset(p, 0, reclen);
            uint32_t ino = (uint32_t)f->position + 1;
            memcpy(p, &ino, 4);
            memcpy(p + 4, &reclen, 2);
            p[6] = (char)f->dir->types[f->position];
            p[7] = (char)n;
            memcpy(p + 8, name, n);
            written += reclen;
            ++f->position;
        }
        result = (int64_t)written;
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}
/* op 0 mkdir, 1 rmdir, 2 unlink */
static int64_t path_op(const char *guest, int op) {
    if (game_path(guest))
        return -E_ROFS;
    char path[1024];
    wchar_t wide[1024];
    int e = gz_file_translate(guest, path, sizeof(path));
    if (e)
        return -e;
    if (gz_widen(path, wide, 1024))
        return -E_NAMETOOLONG;
    BOOL ok = op == 0   ? CreateDirectoryW(wide, NULL)
              : op == 1 ? RemoveDirectoryW(wide)
                        : DeleteFileW(wide);
    return ok ? 0 : -from_windows(GetLastError());
}
static int64_t do_rename(const char *from, const char *to) {
    if (game_path(from) || game_path(to))
        return -E_ROFS;
    char a[1024], b[1024];
    wchar_t wa[1024], wb[1024];
    int e = gz_file_translate(from, a, sizeof(a));
    if (!e)
        e = gz_file_translate(to, b, sizeof(b));
    if (e)
        return -e;
    if (gz_widen(a, wa, 1024) || gz_widen(b, wb, 1024))
        return -E_NAMETOOLONG;
    return MoveFileExW(wa, wb, MOVEFILE_REPLACE_EXISTING) ? 0 : -from_windows(GetLastError());
}
static int64_t do_ftruncate(int fd, int64_t length) {
    File *f = get(fd);
    if (!f)
        return -E_BADF;
    FILE_END_OF_FILE_INFO info = {.EndOfFile.QuadPart = length};
    return SetFileInformationByHandle(f->host, FileEndOfFileInfo, &info, sizeof(info))
               ? 0
               : -from_windows(GetLastError());
}
static int64_t do_truncate(const char *guest, int64_t length) {
    int64_t fd = do_open(guest, 1, 0);
    if (fd < 0)
        return fd;
    int64_t r = do_ftruncate((int)fd, length);
    do_close((int)fd);
    return r;
}
static int64_t do_fsync(int fd) {
    File *f = get(fd);
    if (!f)
        return -E_BADF;
    return FlushFileBuffers(f->host) ? 0 : -from_windows(GetLastError());
}
static int64_t do_access(const char *guest, int mode) {
    (void)mode;
    GuestStat s;
    return do_stat(guest, &s);
}

/* Conventions: sceKernel* -> 0x8002xxxx error codes; POSIX -> -1 and errno. */
static int64_t sce(int64_t r) {
    return r < 0 ? GZ_ERR(-r) : r;
}
static int64_t posix(int64_t r) {
    if (r < 0) {
        *gz_errno() = (int32_t)-r;
        return -1;
    }
    return r;
}
#define PAIR(name, params, args)                                                                   \
    static ABI int64_t sce_##name params {                                                         \
        return sce(do_##name args);                                                                \
    }                                                                                              \
    static ABI int64_t posix_##name params {                                                       \
        return posix(do_##name args);                                                              \
    }
PAIR(open, (const char *p, int f, int m), (p, f, m))
PAIR(close, (int fd), (fd))
PAIR(read, (int fd, void *b, uint64_t n), (fd, b, n))
PAIR(pread, (int fd, void *b, uint64_t n, int64_t o), (fd, b, n, o))
PAIR(write, (int fd, const void *b, uint64_t n), (fd, b, n))
PAIR(pwrite, (int fd, const void *b, uint64_t n, int64_t o), (fd, b, n, o))
PAIR(lseek, (int fd, int64_t o, int w), (fd, o, w))
PAIR(fstat, (int fd, GuestStat *s), (fd, s))
PAIR(stat, (const char *p, GuestStat *s), (p, s))
PAIR(getdents, (int fd, char *b, uint64_t n, int64_t *base), (fd, b, n, base))
PAIR(rename, (const char *a, const char *b), (a, b))
PAIR(ftruncate, (int fd, int64_t l), (fd, l))
PAIR(truncate, (const char *p, int64_t l), (p, l))
PAIR(fsync, (int fd), (fd))
static ABI int64_t posix_access(const char *p, int m) {
    return posix(do_access(p, m));
}
static ABI int64_t sce_mkdir(const char *p, int m) {
    (void)m;
    return sce(path_op(p, 0));
}
static ABI int64_t posix_mkdir(const char *p, int m) {
    (void)m;
    return posix(path_op(p, 0));
}
static ABI int64_t sce_rmdir(const char *p) {
    return sce(path_op(p, 1));
}
static ABI int64_t posix_rmdir(const char *p) {
    return posix(path_op(p, 1));
}
static ABI int64_t sce_unlink(const char *p) {
    return sce(path_op(p, 2));
}
static ABI int64_t posix_unlink(const char *p) {
    return posix(path_op(p, 2));
}
static ABI int32_t sce_check_reachability(const char *p) {
    GuestStat s;
    return (int32_t)sce(do_stat(p, &s));
}

static const GzExport exports[] = {
    {NULL, "sceKernelOpen", (void *)sce_open},
    {NULL, "open", (void *)posix_open},
    {NULL, "_open", (void *)posix_open},
    {NULL, "sceKernelClose", (void *)sce_close},
    {NULL, "close", (void *)posix_close},
    {NULL, "_close", (void *)posix_close},
    {NULL, "sceKernelRead", (void *)sce_read},
    {NULL, "read", (void *)posix_read},
    {NULL, "_read", (void *)posix_read},
    {NULL, "sceKernelPread", (void *)sce_pread},
    {NULL, "pread", (void *)posix_pread},
    {NULL, "sceKernelWrite", (void *)sce_write},
    {NULL, "write", (void *)posix_write},
    {NULL, "_write", (void *)posix_write},
    {NULL, "sceKernelPwrite", (void *)sce_pwrite},
    {NULL, "pwrite", (void *)posix_pwrite},
    {NULL, "sceKernelLseek", (void *)sce_lseek},
    {NULL, "lseek", (void *)posix_lseek},
    {NULL, "sceKernelFstat", (void *)sce_fstat},
    {NULL, "fstat", (void *)posix_fstat},
    {NULL, "_fstat", (void *)posix_fstat},
    {NULL, "sceKernelStat", (void *)sce_stat},
    {NULL, "stat", (void *)posix_stat},
    {NULL, "sceKernelGetdirentries", (void *)sce_getdents},
    {NULL, "getdirentries", (void *)posix_getdents},
    {NULL, "sceKernelRename", (void *)sce_rename},
    {NULL, "rename", (void *)posix_rename},
    {NULL, "sceKernelFtruncate", (void *)sce_ftruncate},
    {NULL, "ftruncate", (void *)posix_ftruncate},
    {NULL, "sceKernelTruncate", (void *)sce_truncate},
    {NULL, "truncate", (void *)posix_truncate},
    {NULL, "sceKernelFsync", (void *)sce_fsync},
    {NULL, "fsync", (void *)posix_fsync},
    {NULL, "access", (void *)posix_access},
    {NULL, "sceKernelCheckReachability", (void *)sce_check_reachability},
    {NULL, "sceKernelMkdir", (void *)sce_mkdir},
    {NULL, "mkdir", (void *)posix_mkdir},
    {NULL, "sceKernelRmdir", (void *)sce_rmdir},
    {NULL, "rmdir", (void *)posix_rmdir},
    {NULL, "sceKernelUnlink", (void *)sce_unlink},
    {NULL, "unlink", (void *)posix_unlink},
};
uintptr_t gz_file_resolve(const char *library, const char *symbol) {
    return GZ_LOOKUP(exports, library, symbol);
}
void gz_file_report(void) {
    gz_log("Runtime: files opened=%lld, reads=%lld (%lld bytes), writes=%lld, not found=%lld",
           (long long)opens, (long long)reads, (long long)bytes_read, (long long)writes,
           (long long)missing);
}
