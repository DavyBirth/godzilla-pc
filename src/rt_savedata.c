/* libSceSaveData on host folders:
 *   <user>\savedata\<user id>\<title id>\<dir name>\          files the game writes
 *   <user>\savedata\<user id>\<title id>\<dir name>.sce_sys\  param.bin, icon0.png
 * A mounted folder appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the folder so the game's own files are untouched.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from bbport's src/runtime_savedata.c (github.com/deadinside28/bloodborne_pc) to Win32,
 * without its Bloodborne-specific save patch. Backup functions follow shadPS4 (no backups kept). */
#include "gz.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_CREATE2 32
#define SLOTS 16

typedef struct {
    char data[10];
    char pad[6];
} TitleId;
typedef struct {
    char data[32];
} DirName;
typedef struct {
    char data[16];
} MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param;
    int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct {
    int32_t user;
    int32_t pad;
    const TitleId *title;
    const DirName *dir;
    const void *fingerprint;
    uint64_t blocks;
    uint32_t mode;
    uint8_t reserved[32];
} Mount1;
typedef struct {
    MountPoint point;
    uint64_t required_blocks;
    uint32_t unused, status;
    uint8_t reserved[28];
    int32_t pad;
} MountResult;
typedef struct {
    int32_t user;
    int32_t pad;
    const TitleId *title;
    const DirName *dir;
    uint32_t unused;
    uint8_t reserved[32];
    int32_t pad2;
} Delete;
typedef struct {
    int32_t user;
    int32_t pad;
    const TitleId *title;
    const DirName *dir;
    uint32_t key, order;
    uint8_t reserved[32];
} SearchCond;
typedef struct {
    uint64_t blocks, free_blocks;
    uint8_t reserved[32];
} SearchInfo;
typedef struct {
    uint32_t hits;
    int32_t pad;
    DirName *names;
    uint32_t names_capacity, set_count;
    Param *params;
    SearchInfo *infos;
    uint8_t reserved[12];
    int32_t pad2;
} SearchResult;
typedef struct {
    const void *buffer;
    uint64_t buffer_size, data_size;
    uint8_t reserved[32];
} Icon;
typedef struct {
    int32_t user;
    int32_t pad;
    const TitleId *title;
    const DirName *dir;
    uint8_t reserved[32];
} CheckBackup;
_Static_assert(sizeof(Param) == 1328, "OrbisSaveDataParam layout");
_Static_assert(sizeof(Mount1) == 80, "OrbisSaveDataMount layout");
_Static_assert(sizeof(MountResult) == 64, "OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond) == 64 && sizeof(SearchResult) == 56, "dir name search layouts");

static SRWLOCK lock = SRWLOCK_INIT;
static int initialized;
static char title_id[16] = "CUSA02046";
static struct {
    int used;
    char host[700], meta[700];
} slots[SLOTS];

/* param.sfo lookup (bbport probe.c sfo_value): string value of key, 0 when absent. */
static int sfo_string(const char *path, const char *key, char *text, size_t text_size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    static unsigned char data[65536];
    size_t n = fread(data, 1, sizeof(data), f);
    fclose(f);
    if (n < 20 || memcmp(data, "\0PSF", 4))
        return 0;
    uint32_t keys, values, count;
    memcpy(&keys, data + 8, 4);
    memcpy(&values, data + 12, 4);
    memcpy(&count, data + 16, 4);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= n; ++i) {
        const unsigned char *e = data + 20 + i * 16;
        uint16_t key_offset;
        uint32_t length, offset;
        memcpy(&key_offset, e, 2);
        memcpy(&length, e + 4, 4);
        memcpy(&offset, e + 12, 4);
        if (keys + key_offset >= n || values + offset + length > n ||
            strcmp((const char *)data + keys + key_offset, key))
            continue;
        size_t copy = length < text_size - 1 ? length : text_size - 1;
        memcpy(text, data + values + offset, copy);
        text[copy] = 0;
        return 1;
    }
    return 0;
}
void gz_savedata_configure(const char *app0) {
    char sfo[MAX_PATH], id[16] = "";
    snprintf(sfo, sizeof(sfo), "%s\\sce_sys\\param.sfo", app0);
    if (sfo_string(sfo, "INSTALL_DIR_SAVEDATA", id, sizeof(id)) ||
        sfo_string(sfo, "TITLE_ID", id, sizeof(id)))
        if (id[0])
            snprintf(title_id, sizeof(title_id), "%s", id);
}

static void root(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out, size, "%s\\savedata\\%d\\%s", gz_user_dir(), user,
             title && *title ? title : title_id);
}
static int valid_name(const char *name, size_t max) {
    size_t n = strnlen(name, max);
    if (!n || n == max)
        return 0;
    for (size_t i = 0; i < n; ++i)
        if (name[i] == '/' || name[i] == '\\' || name[i] == ':' ||
            (name[i] == '.' && (i == 0 || name[i - 1] == '.')))
            return 0;
    return 1;
}
static int folder_exists(const char *path) {
    wchar_t w[1024];
    if (gz_widen(path, w, 1024))
        return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static FILE *open_utf8(const char *path, const wchar_t *mode) {
    wchar_t w[1024];
    return gz_widen(path, w, 1024) ? NULL : _wfopen(w, mode);
}
static void write_param(const char *meta, const Param *p) {
    char path[760];
    snprintf(path, sizeof(path), "%s\\param.bin", meta);
    FILE *f = open_utf8(path, L"wb");
    if (f) {
        Param copy = *p;
        copy.mtime = (int64_t)time(NULL);
        fwrite(&copy, sizeof(copy), 1, f);
        fclose(f);
    }
}
static int read_param(const char *meta, Param *p) {
    char path[760];
    snprintf(path, sizeof(path), "%s\\param.bin", meta);
    memset(p, 0, sizeof(*p));
    FILE *f = open_utf8(path, L"rb");
    if (!f)
        return -1;
    size_t n = fread(p, sizeof(*p), 1, f);
    fclose(f);
    return n == 1 ? 0 : -1;
}
/* Deletes a folder tree (UTF-8 path). */
static void remove_tree(const char *path) {
    char pattern[1100];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    wchar_t w[1100];
    if (gz_widen(pattern, w, 1100))
        return;
    WIN32_FIND_DATAW e;
    HANDLE find = FindFirstFileW(w, &e);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(e.cFileName, L".") || !wcscmp(e.cFileName, L".."))
                continue;
            char name[600], child[1100];
            WideCharToMultiByte(CP_UTF8, 0, e.cFileName, -1, name, sizeof(name), NULL, NULL);
            snprintf(child, sizeof(child), "%s\\%s", path, name);
            wchar_t wc[1100];
            if (gz_widen(child, wc, 1100))
                continue;
            if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(child);
            else
                DeleteFileW(wc);
        } while (FindNextFileW(find, &e));
        FindClose(find);
    }
    if (!gz_widen(path, w, 1100))
        RemoveDirectoryW(w);
}

static ABI int32_t save_initialize(const void *param) {
    (void)param;
    initialized = 1;
    return 0;
}
static ABI int32_t save_terminate(void) {
    if (!initialized)
        return ERR_NOT_INITIALIZED;
    initialized = 0;
    return 0;
}
static int32_t mount(int32_t user, const char *title, const DirName *dir, uint32_t mode,
                     MountResult *result) {
    if (!initialized)
        return ERR_NOT_INITIALIZED;
    if (user < 0)
        return ERR_INVALID_USER;
    if (!dir || !result || !valid_name(dir->data, sizeof(dir->data)))
        return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(user, title, base, sizeof(base));
    snprintf(host, sizeof(host), "%s\\%s", base, dir->data);
    snprintf(meta, sizeof(meta), "%s\\%s.sce_sys", base, dir->data);
    int exists = folder_exists(host);
    if ((mode & MODE_CREATE) && exists)
        return ERR_EXISTS;
    if (!(mode & (MODE_CREATE | MODE_CREATE2)) && !exists)
        return ERR_NOT_FOUND;
    AcquireSRWLockExclusive(&lock);
    int slot = -1;
    for (int i = 0; i < SLOTS; ++i) {
        if (slots[i].used && !strcmp(slots[i].host, host)) {
            ReleaseSRWLockExclusive(&lock);
            return ERR_BAD_MOUNTED;
        }
        if (!slots[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        ReleaseSRWLockExclusive(&lock);
        return ERR_MOUNT_FULL;
    }
    if (!exists && (gz_make_dirs(host) || gz_make_dirs(meta))) {
        ReleaseSRWLockExclusive(&lock);
        return ERR_INTERNAL;
    }
    if (!exists) {
        Param empty = {0};
        write_param(meta, &empty);
    }
    slots[slot].used = 1;
    snprintf(slots[slot].host, sizeof(slots[slot].host), "%s", host);
    snprintf(slots[slot].meta, sizeof(slots[slot].meta), "%s", meta);
    memset(result, 0, sizeof(*result));
    snprintf(result->point.data, sizeof(result->point.data), "/savedata%d", slot);
    result->status = exists ? 0 : 1; /* CREATED */
    gz_file_mount(result->point.data, host);
    ReleaseSRWLockExclusive(&lock);
    gz_log("Runtime: save data '%s' mounted at %s (%s%s)", dir->data, result->point.data,
           exists ? "existing" : "created", (mode & MODE_RDONLY) ? ", read-only" : "");
    return 0;
}
static ABI int32_t save_mount(const Mount1 *m, MountResult *result) {
    if (!m)
        return ERR_PARAMETER;
    return mount(m->user, m->title ? m->title->data : NULL, m->dir, m->mode, result);
}
static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data, "/savedata", 9))
        return -1;
    int slot = atoi(point->data + 9);
    return slot >= 0 && slot < SLOTS && slots[slot].used ? slot : -1;
}
static ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized)
        return ERR_NOT_INITIALIZED;
    AcquireSRWLockExclusive(&lock);
    int slot = slot_of(point);
    if (slot >= 0) {
        gz_file_unmount(point->data);
        slots[slot].used = 0;
    }
    ReleaseSRWLockExclusive(&lock);
    return slot >= 0 ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer,
                                  uint64_t size) {
    if (!initialized)
        return ERR_NOT_INITIALIZED;
    if (!buffer)
        return ERR_PARAMETER;
    AcquireSRWLockExclusive(&lock);
    int slot = slot_of(point);
    int32_t r = 0;
    if (slot < 0)
        r = ERR_NOT_MOUNTED;
    else {
        Param p;
        read_param(slots[slot].meta, &p);
        switch (type) {
        case 0:
            if (size < sizeof(Param))
                r = ERR_PARAMETER;
            else
                memcpy(&p, buffer, sizeof(p));
            break; /* ALL */
        case 1:
            snprintf(p.title, sizeof(p.title), "%.*s", (int)size, (const char *)buffer);
            break;
        case 2:
            snprintf(p.subtitle, sizeof(p.subtitle), "%.*s", (int)size, (const char *)buffer);
            break;
        case 3:
            snprintf(p.detail, sizeof(p.detail), "%.*s", (int)size, (const char *)buffer);
            break;
        case 4:
            if (size < 4)
                r = ERR_PARAMETER;
            else
                memcpy(&p.user_param, buffer, 4);
            break;
        default:
            r = ERR_PARAMETER;
        }
        if (!r)
            write_param(slots[slot].meta, &p);
    }
    ReleaseSRWLockExclusive(&lock);
    return r;
}
static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized)
        return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer)
        return ERR_PARAMETER;
    AcquireSRWLockExclusive(&lock);
    int slot = slot_of(point);
    int32_t r = ERR_NOT_MOUNTED;
    if (slot >= 0) {
        char path[760];
        snprintf(path, sizeof(path), "%s\\icon0.png", slots[slot].meta);
        FILE *f = open_utf8(path, L"wb");
        r = f && fwrite(icon->buffer, 1, icon->data_size, f) == icon->data_size ? 0 : ERR_INTERNAL;
        if (f)
            fclose(f);
    }
    ReleaseSRWLockExclusive(&lock);
    return r;
}
static ABI int32_t save_delete(const Delete *d) {
    if (!initialized)
        return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data, sizeof(d->dir->data)))
        return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(d->user, d->title ? d->title->data : NULL, base, sizeof(base));
    snprintf(host, sizeof(host), "%s\\%s", base, d->dir->data);
    snprintf(meta, sizeof(meta), "%s\\%s.sce_sys", base, d->dir->data);
    if (!folder_exists(host))
        return ERR_NOT_FOUND;
    remove_tree(host);
    remove_tree(meta);
    gz_log("Runtime: save data '%s' deleted", d->dir->data);
    return 0;
}
/* SQL-LIKE pattern: % any run, _ one character. */
static int like(const char *s, const char *p) {
    if (!*p)
        return !*s;
    if (*p == '%') {
        for (;; ++s) {
            if (like(s, p + 1))
                return 1;
            if (!*s)
                return 0;
        }
    }
    return *s && (*p == '_' || *p == *s) && like(s + 1, p + 1);
}
typedef struct {
    char name[32];
    Param param;
} Entry;
static uint32_t sort_key, sort_order;
static int compare(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    int c = sort_key == 1   ? (x->param.user_param > y->param.user_param) -
                                  (x->param.user_param < y->param.user_param)
            : sort_key == 3 ? (x->param.mtime > y->param.mtime) - (x->param.mtime < y->param.mtime)
                            : strcmp(x->name, y->name);
    return sort_order ? -c : c;
}
static ABI int32_t save_search(const SearchCond *cond, SearchResult *result) {
    if (!initialized)
        return ERR_NOT_INITIALIZED;
    if (!cond || !result)
        return ERR_PARAMETER;
    char base[600], pattern[620];
    root(cond->user, cond->title ? cond->title->data : NULL, base, sizeof(base));
    snprintf(pattern, sizeof(pattern), "%s\\*", base);
    Entry *entries = NULL;
    size_t count = 0, capacity = 0;
    wchar_t w[700];
    WIN32_FIND_DATAW e;
    HANDLE find = gz_widen(pattern, w, 700) ? INVALID_HANDLE_VALUE : FindFirstFileW(w, &e);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            char name[600];
            if (!(e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            WideCharToMultiByte(CP_UTF8, 0, e.cFileName, -1, name, sizeof(name), NULL, NULL);
            size_t n = strlen(name);
            if (name[0] == '.' || n >= 32 || (n > 8 && !strcmp(name + n - 8, ".sce_sys")))
                continue;
            if (cond->dir && cond->dir->data[0] && !like(name, cond->dir->data))
                continue;
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 16;
                Entry *next = realloc(entries, capacity * sizeof(*entries));
                if (!next)
                    break;
                entries = next;
            }
            snprintf(entries[count].name, 32, "%s", name);
            char meta[700];
            snprintf(meta, sizeof(meta), "%s\\%s.sce_sys", base, name);
            read_param(meta, &entries[count].param);
            ++count;
        } while (FindNextFileW(find, &e));
        FindClose(find);
    }
    sort_key = cond->key;
    sort_order = cond->order;
    if (count)
        qsort(entries, count, sizeof(*entries), compare);
    result->hits = (uint32_t)count;
    uint32_t set = count < result->names_capacity ? (uint32_t)count : result->names_capacity;
    for (uint32_t i = 0; i < set; ++i) {
        if (result->names)
            memcpy(result->names[i].data, entries[i].name, 32);
        if (result->params)
            result->params[i] = entries[i].param;
        if (result->infos) {
            memset(&result->infos[i], 0, sizeof(SearchInfo));
            result->infos[i].blocks = 32768;
            result->infos[i].free_blocks = 16384;
        }
    }
    result->set_count = set;
    free(entries);
    return 0;
}
/* Backups: none are kept (as shadPS4's defaults), so there is never one to restore. */
static ABI int32_t save_check_backup(const CheckBackup *check) {
    (void)check;
    return ERR_NOT_FOUND;
}
static ABI int32_t save_restore_backup(const void *restore) {
    (void)restore;
    return ERR_NOT_FOUND;
}
static ABI int32_t save_umount_with_backup(const MountPoint *point) {
    return save_umount(point);
}

static const GzExport exports[] = {
    {NULL, "sceSaveDataInitialize", (void *)save_initialize},
    {NULL, "sceSaveDataInitialize2", (void *)save_initialize},
    {NULL, "sceSaveDataInitialize3", (void *)save_initialize},
    {NULL, "sceSaveDataTerminate", (void *)save_terminate},
    {NULL, "sceSaveDataMount", (void *)save_mount},
    {NULL, "sceSaveDataUmount", (void *)save_umount},
    {NULL, "sceSaveDataUmountWithBackup", (void *)save_umount_with_backup},
    {NULL, "sceSaveDataSetParam", (void *)save_set_param},
    {NULL, "sceSaveDataSaveIcon", (void *)save_icon},
    {NULL, "sceSaveDataDelete", (void *)save_delete},
    {NULL, "sceSaveDataDirNameSearch", (void *)save_search},
    {NULL, "sceSaveDataCheckBackupData", (void *)save_check_backup},
    {NULL, "sceSaveDataRestoreBackupData", (void *)save_restore_backup},
};
uintptr_t gz_savedata_resolve(const char *library, const char *symbol) {
    return GZ_LOOKUP(exports, library, symbol);
}
