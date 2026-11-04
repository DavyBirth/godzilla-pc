/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets unavailable;
 *   - message dialogs offer native choices; other dialogs still finish immediately;
 *   - the whole package is installed (PlayGo reports every chunk local);
 *   - system modules are reference-counted (the host provides their functions).
 * SPDX-License-Identifier: GPL-2.0-or-later
 * From bbport's src/runtime_services.c and runtime_content.c
 * (github.com/deadinside28/bloodborne_pc); functions bbport lacks follow shadPS4. */
#include "gz.h"
#include "dialog_ui.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define USER_ID 1
#define USER_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_NO_EVENT ((int32_t)0x80960007)
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define TROPHY_INVALID ((int32_t)0x80551604)
#define MODULE_INVALID_ID ((int32_t)0x805a1000)
#define MODULE_NOT_LOADED ((int32_t)0x805a1001)
#define CONTENT_PARAMETER ((int32_t)0x80D90002)
#define CONTENT_BUSY ((int32_t)0x80D90003)
#define CONTENT_NO_ENTITLEMENT ((int32_t)0x80D90007)

static volatile LONG next_id;
static int32_t new_id(void) {
    return InterlockedIncrement(&next_id);
}
static ABI int32_t ok_void(void) {
    return 0;
}

/* ---- Sysmodule ---- */
static const struct {
    uint32_t id;
    const char *name;
} module_names[] = {
#include "sysmodule_names.inc"
};
static const char *module_name(uint16_t id) {
    for (size_t i = 0; i < sizeof(module_names) / sizeof(*module_names); ++i)
        if (module_names[i].id == id)
            return module_names[i].name;
    return "unknown";
}
static volatile LONG module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id)
        return MODULE_INVALID_ID;
    LONG refs = InterlockedIncrement(&module_refs[id]);
    gz_log("Runtime: system module 0x%x (%s) loaded, references=%ld", id, module_name(id), refs);
    return 0;
}
static ABI int32_t module_is_loaded(uint16_t id) {
    if (!id)
        return MODULE_INVALID_ID;
    return module_refs[id] ? 0 : MODULE_NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t r = module_is_loaded(id);
    if (!r)
        InterlockedDecrement(&module_refs[id]);
    return r;
}

/* ---- AppContent: base game, no add-ons ---- */
static int content_initialized;
static ABI int32_t content_init(const unsigned char *init, unsigned char *boot) {
    if (!init || !boot)
        return CONTENT_PARAMETER;
    if (content_initialized)
        return CONTENT_BUSY;
    memset(boot, 0, 40);
    content_initialized = 1;
    gz_log("Runtime: AppContent initialized; base game, no add-ons mounted");
    return 0;
}
static ABI int32_t content_addcont_info(uint32_t service, const void *label, void *info) {
    (void)service;
    return !label || !info ? CONTENT_PARAMETER : CONTENT_NO_ENTITLEMENT;
}
static ABI int32_t content_entitlement_key(uint32_t service, const void *label, void *key) {
    (void)service;
    return !label || !key ? CONTENT_PARAMETER : CONTENT_NO_ENTITLEMENT;
}

/* ---- UserService ---- */
static volatile LONG login_event_pending = 1;
static ABI int32_t user_initialize(const void *params) {
    (void)params;
    gz_log("Runtime: UserService initialized (user 1 logged in)");
    return 0;
}
static ABI int32_t user_initial(int32_t *id) {
    if (!id)
        return USER_INVALID_ARGUMENT;
    *id = USER_ID;
    return 0;
}
static ABI int32_t user_name(int32_t id, char *name, uint64_t size) {
    if (id != USER_ID || !name)
        return USER_INVALID_ARGUMENT;
    const char *value = getenv("GZ_USER_NAME") ? getenv("GZ_USER_NAME") : "Player";
    if (strlen(value) + 1 > size)
        return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(name, value);
    return 0;
}
static ABI int32_t user_event(int32_t *event) {
    if (!event)
        return USER_INVALID_ARGUMENT;
    if (!InterlockedExchange(&login_event_pending, 0))
        return USER_NO_EVENT;
    event[0] = 0;
    event[1] = USER_ID; /* LOGIN */
    return 0;
}
static ABI int32_t user_color(int32_t id, int32_t *color) {
    if (id != USER_ID || !color)
        return USER_INVALID_ARGUMENT;
    *color = 0; /* blue: the first user's light bar colour */
    return 0;
}

/* ---- SystemService ---- */
/* PS4 language ids: 1 English (US), 2 French, 17 Portuguese (Brazil). GZ_LANGUAGE overrides. */
static int language(void) {
    const char *v = getenv("GZ_LANGUAGE");
    return v ? atoi(v) : 1;
}
static ABI int32_t system_param(int32_t id, int32_t *value) {
    if (!value)
        return SYSTEM_PARAMETER;
    switch (id) {
    case 1:
        *value = language();
        break;
    case 2:
        *value = 1;
        break; /* date format DD/MM/YYYY */
    case 3:
        *value = 1;
        break; /* 24-hour clock */
    case 4: {  /* time zone: minutes east of UTC */
        TIME_ZONE_INFORMATION tz;
        DWORD mode = GetTimeZoneInformation(&tz);
        *value = -(int32_t)(tz.Bias + (mode == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : 0));
        break;
    }
    case 5:
        *value = 0;
        break; /* summer time */
    case 7:
        *value = 0;
        break; /* parental level off */
    case 1000:
        *value = 1;
        break; /* enter button = cross */
    default:
        gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: unsupported system parameter %d", id);
    }
    return 0;
}
static ABI int32_t system_status(unsigned char *status) {
    if (!status)
        return SYSTEM_PARAMETER;
    memset(status, 0, 12); /* no events, no overlay, foreground, normal CPU mode */
    return 0;
}
static ABI int32_t system_event(void *event) {
    (void)event;
    return SYSTEM_NO_EVENT;
}
static ABI int32_t hide_splash(void) {
    gz_log("Runtime: SystemService: splash screen hidden");
    return 0;
}

/* ---- NetCtl / Net / Http / Ssl / Rudp: no network ---- */
static ABI int32_t net_pool_create(const char *name, int size, int flags) {
    (void)name;
    (void)size;
    (void)flags;
    return new_id();
}
static ABI int32_t net_unreachable(void) {
    return NET_ENETUNREACH;
}
static ABI uint16_t net_swap16(uint16_t v) {
    return (uint16_t)(v >> 8 | v << 8);
}
static ABI int32_t net_pton(int af, const char *src, uint8_t *dst) {
    if (af != 2)
        return NET_EINVAL;
    unsigned a, b, c, d;
    char tail;
    if (!src || !dst || sscanf(src, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 ||
        b > 255 || c > 255 || d > 255)
        return 0;
    dst[0] = (uint8_t)a;
    dst[1] = (uint8_t)b;
    dst[2] = (uint8_t)c;
    dst[3] = (uint8_t)d;
    return 1;
}
static ABI const char *net_ntop(int af, const uint8_t *src, char *dst, uint32_t size) {
    if (af != 2 || !src || !dst)
        return NULL;
    if (snprintf(dst, size, "%u.%u.%u.%u", src[0], src[1], src[2], src[3]) >= (int)size)
        return NULL;
    return dst;
}
static ABI int32_t net_zero_info(int s, void *info, int n, int flags) {
    (void)s;
    (void)n;
    (void)flags;
    (void)info;
    return NET_ENETUNREACH;
}
static ABI int32_t netctl_state(int32_t *state) {
    if (!state)
        return NET_CTL_INVALID_ADDR;
    *state = 0;
    return 0;
}
static ABI int32_t netctl_info(int code, void *info) {
    (void)code;
    (void)info;
    return NET_CTL_NOT_CONNECTED;
}
static ABI int32_t netctl_register(void *cb, void *arg, int32_t *cid) {
    (void)cb;
    (void)arg;
    if (!cid)
        return NET_CTL_INVALID_ADDR;
    *cid = new_id();
    return 0;
}
static ABI int32_t netctl_result(int32_t event, int32_t *error) {
    (void)event;
    if (!error)
        return NET_CTL_INVALID_ADDR;
    *error = NET_CTL_NOT_CONNECTED;
    return 0;
}
static ABI int32_t lib_init_id(void) {
    return new_id();
}
static ABI int32_t rudp_zero_count(void) {
    return 0;
}

/* ---- NP (PSN): signed out ---- */
static ABI int32_t np_state(int32_t user, int32_t *state) {
    (void)user;
    if (!state)
        return NP_INVALID_ARGUMENT;
    *state = 1;
    return 0;
} /* SIGNED_OUT */
static ABI int32_t np_signed_out(void) {
    return NP_SIGNED_OUT;
}
static ABI int32_t np_register(void *cb, void *arg) {
    (void)cb;
    (void)arg;
    return new_id();
}
static ABI int32_t np_request(void) {
    return new_id();
}
static ABI int32_t np_compare(const void *a, const void *b) {
    if (!a || !b)
        return NP_INVALID_ARGUMENT;
    return memcmp(a, b, 16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* ---- Trophies: accepted locally, recorded in the log ---- */
static ABI int32_t trophy_context(int32_t *ctx, int32_t user, uint32_t label, uint64_t options) {
    (void)user;
    (void)label;
    (void)options;
    if (!ctx)
        return TROPHY_INVALID;
    *ctx = new_id();
    return 0;
}
static ABI int32_t trophy_handle(int32_t *handle) {
    if (!handle)
        return TROPHY_INVALID;
    *handle = new_id();
    return 0;
}
static ABI int32_t trophy_unlock(int32_t ctx, int32_t handle, int32_t id, int32_t *platinum) {
    (void)ctx;
    (void)handle;
    gz_log("Runtime: trophy %d unlocked", id);
    if (platinum)
        *platinum = -1;
    return 0;
}
/* OrbisNpTrophyFlagArray: 128 bits, none unlocked (no trophy file is kept yet). */
static ABI int32_t trophy_unlock_state(int32_t ctx, int32_t handle, uint32_t *flags,
                                       uint32_t *count) {
    (void)ctx;
    (void)handle;
    if (!flags || !count)
        return TROPHY_INVALID;
    memset(flags, 0, 16);
    *count = 0;
    return 0;
}

/* ---- Common dialogs: message choices appear inside the game window. ---- */
enum { DLG_MSG, DLG_SAVE, DLG_COMMERCE, DLG_FRIENDS, DLG_ERROR, DIALOGS };
static const char *dialog_names[DIALOGS] = {"MsgDialog", "SaveDataDialog", "NpCommerceDialog",
                                            "NpFriendListDialog", "ErrorDialog"};
static int dialog_initialized[DIALOGS],
    dialog_state[DIALOGS]; /* 0 none, 1 initialized, 2 running, 3 finished */
static ABI int32_t common_init(void) {
    return 0;
}
static int32_t dialog_init(int i) {
    if (dialog_initialized[i])
        return (int32_t)0x80B80004; /* ALREADY_INITIALIZED */
    dialog_initialized[i] = 1;
    dialog_state[i] = 1;
    return 0;
}
static int32_t dialog_open(int i) {
    if (!dialog_initialized[i])
        return DIALOG_NOT_INITIALIZED;
    gz_log("Runtime: %s opened; completed at once (no dialog UI yet)", dialog_names[i]);
    dialog_state[i] = 3;
    return 0;
}
static int32_t dialog_close(int i) {
    if (dialog_initialized[i])
        dialog_state[i] = 3;
    return 0;
}
static int32_t dialog_term(int i) {
    if (!dialog_initialized[i])
        return DIALOG_NOT_INITIALIZED;
    dialog_initialized[i] = 0;
    dialog_state[i] = 0;
    return 0;
}
#define DIALOG(tag, i)                                                                             \
    static ABI int32_t tag##_init(void) {                                                          \
        return dialog_init(i);                                                                     \
    }                                                                                              \
    static ABI int32_t tag##_open(const void *p) {                                                 \
        (void)p;                                                                                   \
        return dialog_open(i);                                                                     \
    }                                                                                              \
    static ABI int32_t tag##_status(void) {                                                        \
        return dialog_state[i];                                                                    \
    }                                                                                              \
    static ABI int32_t tag##_close(void) {                                                         \
        return dialog_close(i);                                                                    \
    }                                                                                              \
    static ABI int32_t tag##_term(void) {                                                          \
        return dialog_term(i);                                                                     \
    }
DIALOG(commerce, DLG_COMMERCE)
DIALOG(friends, DLG_FRIENDS)
    /* PS4 layouts from shadPS4 system/msgdialog_ui.h. Copy text before returning
 * from Open; a native worker keeps the guest thread and renderer running. */
    typedef struct {
    uint64_t size;
    char reserved[36];
    uint32_t magic;
} MsgBase;
typedef struct {
    uint32_t button_type, pad;
    const char *text;
    const void *buttons;
    char reserved[24];
} MsgUser;
typedef struct {
    MsgBase base;
    uint64_t size;
    uint32_t mode, pad;
    const MsgUser *user;
    const void *progress, *system;
    int32_t user_id;
    char reserved[40];
    int32_t tail;
} MsgParam;
_Static_assert(sizeof(MsgParam) == 136, "PS4 message parameter layout");
_Static_assert(offsetof(MsgParam, user) == 64, "PS4 message pointer offset");
static HANDLE msg_worker;
static volatile LONG msg_running;
static volatile LONG error_running;
static volatile LONG save_running;
static HANDLE error_worker;
static int32_t msg_button, msg_result_code;
typedef struct {
    wchar_t text[4096];
    unsigned type;
} MsgWork;
static DWORD WINAPI msg_show(void *arg) {
    MsgWork *work = arg;
    int answer = gz_dialog_show(work->text, work->type);
    msg_button = answer == IDNO ? 2 : answer == IDCANCEL || !answer ? 0 : 1;
    msg_result_code = msg_button ? 0 : 1;
    gz_log("Runtime: MsgDialog user choice: result=%d button=%d", msg_result_code, msg_button);
    free(work);
    InterlockedExchange(&msg_running, 0);
    return 0;
}
static ABI int32_t msg_init(void) {
    return dialog_init(DLG_MSG);
}
static ABI int32_t msg_status(void) {
    if (dialog_state[DLG_MSG] == 2 && !InterlockedCompareExchange(&msg_running, 0, 0))
        dialog_state[DLG_MSG] = 3;
    return dialog_state[DLG_MSG];
}
static ABI int32_t msg_close(void) {
    if (msg_status() != 2)
        return (int32_t)0x80B8000B;
    gz_dialog_cancel();
    return 0;
}
static ABI int32_t msg_term(void) {
    if (msg_status() == 2) {
        msg_close();
        return (int32_t)0x80B80008;
    }
    if (msg_worker) {
        CloseHandle(msg_worker);
        msg_worker = NULL;
    }
    return dialog_term(DLG_MSG);
}
static ABI int32_t msg_open(const MsgParam *p) {
    if (!dialog_initialized[DLG_MSG])
        return DIALOG_NOT_INITIALIZED;
    if (!p)
        return (int32_t)0x80B8000D;
    if (msg_status() == 2)
        return (int32_t)0x80B80008;
    if (InterlockedCompareExchange(&error_running, 0, 0))
        return (int32_t)0x80B80008;
    if (InterlockedCompareExchange(&save_running, 0, 0))
        return (int32_t)0x80B80008;
    if (p->size != sizeof(*p) || p->base.size != sizeof(MsgBase))
        return (int32_t)0x80B8000A;
    gz_log("Runtime: MsgDialog mode=%u", p->mode);
    if (p->mode != 1)
        return (int32_t)0x80B8000F;
    if (!p->user || !p->user->text)
        return (int32_t)0x80B8000A;
    const uint32_t type = p->user->button_type;
    gz_log("Runtime: MsgDialog buttons=%u text='%s'", type, p->user->text);
    switch (type) {
    case 0:
    case 1:
    case 3:
    case 7:
    case 8:
        break;
    default:
        return (int32_t)0x80B8000F;
    }
    MsgWork *work = calloc(1, sizeof(*work));
    if (!work)
        return (int32_t)0x80B80009;
    work->type = type;
    if (!MultiByteToWideChar(CP_UTF8, 0, p->user->text, -1, work->text, 4096)) {
        free(work);
        return (int32_t)0x80B8000A;
    }
    if (msg_worker) {
        CloseHandle(msg_worker);
        msg_worker = NULL;
    }
    msg_button = 0;
    msg_result_code = 1;
    InterlockedExchange(&msg_running, 1);
    dialog_state[DLG_MSG] = 2;
    gz_dialog_prepare();
    msg_worker = CreateThread(NULL, 0, msg_show, work, 0, NULL);
    if (!msg_worker) {
        free(work);
        InterlockedExchange(&msg_running, 0);
        dialog_state[DLG_MSG] = 1;
        return (int32_t)0x80B80009;
    }
    return 0;
}
static ABI int32_t msg_result(int32_t *r) {
    if (!r)
        return (int32_t)0x80B8000D;
    if (msg_status() != 3)
        return (int32_t)0x80B80005;
    memset(r, 0, 44);
    r[1] = msg_result_code;
    r[2] = msg_button;
    return 0;
}
static ABI int32_t commerce_result(int32_t *r) {
    if (!r)
        return (int32_t)0x80B80005;
    memset(r, 0, 8);
    return 0;
}
static ABI int32_t friends_result(int32_t *r) {
    if (!r)
        return (int32_t)0x80B80005;
    memset(r, 0, 8);
    return 0;
}
static ABI int32_t save_ready(void) {
    return 1;
}

/* ErrorDialog has its own 16-byte parameter, not a MsgDialog parameter. */
typedef struct {
    int32_t size, code, user, reserved;
} ErrorParam;
static DWORD WINAPI error_show(void *arg) {
    wchar_t *text = arg;
    gz_dialog_show(text, 0);
    free(text);
    InterlockedExchange(&error_running, 0);
    return 0;
}
static ABI int32_t error_init(void) {
    return dialog_init(DLG_ERROR);
}
static ABI int32_t error_status(void) {
    if (dialog_state[DLG_ERROR] == 2 && !InterlockedCompareExchange(&error_running, 0, 0))
        dialog_state[DLG_ERROR] = 3;
    return dialog_state[DLG_ERROR];
}
static ABI int32_t error_open(const ErrorParam *p) {
    if (!dialog_initialized[DLG_ERROR])
        return DIALOG_NOT_INITIALIZED;
    if (!p)
        return (int32_t)0x80B8000D;
    if (p->size != sizeof(*p))
        return (int32_t)0x80B8000A;
    if (error_status() == 2 || InterlockedCompareExchange(&msg_running, 0, 0))
        return (int32_t)0x80B80008;
    if (InterlockedCompareExchange(&save_running, 0, 0))
        return (int32_t)0x80B80008;
    wchar_t *text = calloc(256, sizeof(wchar_t));
    if (!text)
        return (int32_t)0x80B80009;
    /* The code stays in the log; the player gets a plain sentence. 0x8055xxxx
       are PlayStation Network (NP) errors: VS mode is online-only. */
    if (((unsigned)p->code & 0xFFFF0000u) == 0x80550000u)
        wcscpy(text, L"O modo VS é jogado pela internet, na rede do PlayStation.\n"
                     L"Essa rede não existe na versão de PC, então este modo não está disponível.");
    else
        wcscpy(text, L"Não foi possível concluir esta ação.");
    gz_log("Runtime: ErrorDialog code=0x%08x user=%d; waiting for confirmation", (unsigned)p->code,
           p->user);
    if (error_worker) {
        CloseHandle(error_worker);
        error_worker = NULL;
    }
    gz_dialog_prepare();
    InterlockedExchange(&error_running, 1);
    dialog_state[DLG_ERROR] = 2;
    error_worker = CreateThread(NULL, 0, error_show, text, 0, NULL);
    if (!error_worker) {
        free(text);
        InterlockedExchange(&error_running, 0);
        dialog_state[DLG_ERROR] = 1;
        return (int32_t)0x80B80009;
    }
    return 0;
}
static ABI int32_t error_close(void) {
    if (error_status() != 2)
        return (int32_t)0x80B8000B;
    gz_dialog_cancel();
    return 0;
}
static ABI int32_t error_term(void) {
    if (error_status() == 2) {
        error_close();
        return (int32_t)0x80B80008;
    }
    if (error_worker) {
        CloseHandle(error_worker);
        error_worker = NULL;
    }
    return dialog_term(DLG_ERROR);
}

/* SaveDataDialog layouts follow save_data/dialog/savedatadialog_ui.h.
 * Results contain caller-owned pointers: do not clear or overwrite them. */
typedef struct {
    uint32_t buttons, kind;
    const char *text;
    char reserved[32];
} SaveUser;
typedef struct {
    uint32_t type, pad;
    const char *text;
    uint32_t system;
    char reserved[28];
} SaveProgress;
typedef struct {
    MsgBase base;
    int32_t size;
    uint32_t mode, display, pad;
    const void *animation, *items;
    const SaveUser *user;
    const void *system;
    const uint32_t *error;
    const SaveProgress *progress;
    void *user_data;
    const void *option;
    char reserved[24];
} SaveParam;
typedef struct {
    uint32_t mode, result, button, pad;
    void *directory, *param, *user_data;
    char reserved[32];
} SaveResult;
_Static_assert(sizeof(SaveParam) == 152, "PS4 save dialog parameter layout");
_Static_assert(sizeof(SaveResult) == 72, "PS4 save dialog result layout");
static HANDLE save_worker;
static uint32_t save_mode, save_button, save_result_code;
static uint32_t save_percent;
static volatile LONG save_closed;
static void *save_user_data;
static DWORD WINAPI save_show(void *arg) {
    MsgWork *work = arg;
    int answer = gz_dialog_show(work->text, work->type);
    save_button = answer == IDNO ? 2 : answer == IDOK ? 1 : 0;
    save_result_code = save_button || InterlockedCompareExchange(&save_closed, 0, 0) ? 0 : 1;
    free(work);
    InterlockedExchange(&save_running, 0);
    return 0;
}
static ABI int32_t save_init(void) {
    return dialog_init(DLG_SAVE);
}
static ABI int32_t save_status(void) {
    if (dialog_state[DLG_SAVE] == 2 && !InterlockedCompareExchange(&save_running, 0, 0))
        dialog_state[DLG_SAVE] = 3;
    return dialog_state[DLG_SAVE];
}
static ABI int32_t save_open(const SaveParam *p) {
    if (!dialog_initialized[DLG_SAVE])
        return DIALOG_NOT_INITIALIZED;
    if (!p)
        return (int32_t)0x80B8000D;
    if (save_status() == 2 || InterlockedCompareExchange(&msg_running, 0, 0) ||
        InterlockedCompareExchange(&error_running, 0, 0))
        return (int32_t)0x80B80008;
    if (p->size != sizeof(*p) || p->base.size != sizeof(MsgBase))
        return (int32_t)0x80B8000A;
    /* List selection, system messages and progress are separate contracts.
     * Reject until implemented instead of silently answering Yes. */
    if (p->mode != 2 && p->mode != 4 && p->mode != 5) {
        gz_log("Runtime: SaveDataDialog mode=%u not supported yet", p->mode);
        return (int32_t)0x80B8000F;
    }
    MsgWork *work = calloc(1, sizeof(*work));
    if (!work)
        return (int32_t)0x80B80009;
    if (p->mode == 2) {
        if (!p->user || !p->user->text) {
            free(work);
            return (int32_t)0x80B8000A;
        }
        if (p->user->buttons != 0 && p->user->buttons != 1 && p->user->buttons != 3) {
            free(work);
            return (int32_t)0x80B8000F;
        }
        work->type = p->user->buttons;
        if (!MultiByteToWideChar(CP_UTF8, 0, p->user->text, -1, work->text, 4096)) {
            free(work);
            return (int32_t)0x80B8000A;
        }
    } else if (p->mode == 4) {
        if (!p->error) {
            free(work);
            return (int32_t)0x80B8000A;
        }
        swprintf(work->text, 4096, L"Erro nos dados salvos.\n\nCódigo: 0x%08X", *p->error);
    } else {
        if (!p->progress || p->progress->type != 0 || p->progress->system > 2 || p->display < 1 ||
            p->display > 3) {
            free(work);
            return (int32_t)0x80B8000A;
        }
        work->type = 256;
        if (p->progress->text) {
            if (!MultiByteToWideChar(CP_UTF8, 0, p->progress->text, -1, work->text, 4096)) {
                free(work);
                return (int32_t)0x80B8000A;
            }
        } else {
            const wchar_t *message = p->progress->system == 2   ? L"Restaurando dados salvos..."
                                     : p->progress->system == 0 ? L""
                                     : p->display == 1          ? L"Salvando..."
                                     : p->display == 2          ? L"Carregando..."
                                                                : L"Excluindo...";
            wcscpy(work->text, message);
        }
    }
    if (save_worker) {
        CloseHandle(save_worker);
        save_worker = NULL;
    }
    save_mode = p->mode;
    save_user_data = p->user_data;
    save_button = 0;
    save_result_code = 1;
    save_percent = 0;
    InterlockedExchange(&save_closed, 0);
    gz_dialog_prepare();
    InterlockedExchange(&save_running, 1);
    dialog_state[DLG_SAVE] = 2;
    save_worker = CreateThread(NULL, 0, save_show, work, 0, NULL);
    if (!save_worker) {
        free(work);
        InterlockedExchange(&save_running, 0);
        dialog_state[DLG_SAVE] = 1;
        return (int32_t)0x80B80009;
    }
    gz_log("Runtime: SaveDataDialog mode=%u waiting for user", p->mode);
    return 0;
}
static ABI int32_t save_close(void) {
    if (save_status() != 2)
        return (int32_t)0x80B8000B;
    InterlockedExchange(&save_closed, 1);
    gz_dialog_cancel();
    return 0;
}
static ABI int32_t save_term(void) {
    if (save_status() == 2) {
        save_close();
        return (int32_t)0x80B80008;
    }
    if (save_worker) {
        CloseHandle(save_worker);
        save_worker = NULL;
    }
    return dialog_term(DLG_SAVE);
}
static ABI int32_t save_result(SaveResult *r) {
    if (!r)
        return (int32_t)0x80B8000D;
    if (save_status() != 3)
        return (int32_t)0x80B80005;
    r->mode = save_mode;
    r->result = save_result_code;
    r->button = save_button;
    r->user_data = save_user_data;
    return 0;
}
static ABI int32_t save_progress(int32_t target, uint32_t rate) {
    if (save_status() != 2)
        return (int32_t)0x80B8000B;
    if (save_mode != 5)
        return (int32_t)0x80B8000F;
    if (target != 0)
        return (int32_t)0x80B8000A;
    save_percent = rate > 100 ? 100 : rate;
    gz_dialog_progress(save_percent);
    return 0;
}
static ABI int32_t save_progress_inc(int32_t target, uint32_t delta) {
    uint32_t remaining = 100 - save_percent;
    return save_progress(target, delta >= remaining ? 100 : save_percent + delta);
}

/* ---- PlayGo: fully installed package ---- */
static int playgo_handle, playgo_chunks = -1;
static ABI int32_t playgo_init(const void *params) {
    (void)params;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\sce_sys\\playgo-chunk.dat", gz_app0());
    FILE *f = fopen(path, "rb");
    unsigned char header[12];
    playgo_chunks = 1;
    if (f) {
        if (fread(header, 1, sizeof(header), f) == sizeof(header))
            playgo_chunks = header[10] | header[11] << 8;
        fclose(f);
    }
    gz_log("Runtime: PlayGo initialized; %d chunks, all installed locally", playgo_chunks);
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle, const void *param) {
    (void)param;
    if (!handle)
        return PLAYGO_BAD_POINTER;
    playgo_handle = 1;
    *handle = 1;
    return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle, uint16_t *ids, uint32_t count, uint32_t *out) {
    if (handle != playgo_handle || !handle)
        return PLAYGO_BAD_HANDLE;
    if (!out)
        return PLAYGO_BAD_POINTER;
    if (ids && !count)
        return PLAYGO_BAD_SIZE;
    if (!ids) {
        *out = (uint32_t)playgo_chunks;
        return 0;
    }
    uint32_t n = count < (uint32_t)playgo_chunks ? count : (uint32_t)playgo_chunks;
    for (uint32_t i = 0; i < n; ++i)
        ids[i] = (uint16_t)i;
    *out = n;
    return 0;
}
static ABI int32_t playgo_locus(int32_t handle, const uint16_t *ids, uint32_t count, int8_t *loci) {
    if (handle != playgo_handle || !handle)
        return PLAYGO_BAD_HANDLE;
    if (!ids || !loci)
        return PLAYGO_BAD_POINTER;
    if (!count)
        return PLAYGO_BAD_SIZE;
    for (uint32_t i = 0; i < count; ++i) {
        if (ids[i] >= playgo_chunks)
            return PLAYGO_BAD_CHUNK;
        loci[i] = 3; /* LocalFast */
    }
    return 0;
}
static ABI int32_t playgo_language_mask(int32_t handle, uint64_t mask) {
    (void)mask;
    return handle == playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE;
}

/* ---- DiscMap: the game is fully installed, no disc bitmap exists (bbport) ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_on_hdd(const char *path, int64_t offset, int64_t size, int32_t *result) {
    (void)path;
    (void)offset;
    (void)size;
    (void)result;
    return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char *path, int64_t offset, int64_t size, int32_t *flags,
                                int32_t *r1, int32_t *r2) {
    (void)path;
    (void)offset;
    (void)size;
    (void)flags;
    (void)r1;
    (void)r2;
    return DISC_MAP_NO_BITMAP;
}

static const GzExport exports[] = {
    {"libSceDiscMap", "sceDiscMapIsRequestOnHDD", (void *)discmap_on_hdd},
    {"libSceDiscMap", "sceDiscMap_8A828CAEE7EDD5E9", (void *)discmap_8a82},
    {"libSceSysmodule", "sceSysmoduleLoadModule", (void *)module_load},
    {"libSceSysmodule", "sceSysmoduleIsLoaded", (void *)module_is_loaded},
    {"libSceSysmodule", "sceSysmoduleUnloadModule", (void *)module_unload},
    {"libSceAppContent", "sceAppContentInitialize", (void *)content_init},
    {"libSceAppContent", "sceAppContentGetAddcontInfo", (void *)content_addcont_info},
    {"libSceAppContent", "sceAppContentGetEntitlementKey", (void *)content_entitlement_key},
    {NULL, "sceUserServiceInitialize", (void *)user_initialize},
    {NULL, "sceUserServiceTerminate", (void *)ok_void},
    {NULL, "sceUserServiceGetInitialUser", (void *)user_initial},
    {NULL, "sceUserServiceGetUserName", (void *)user_name},
    {NULL, "sceUserServiceGetEvent", (void *)user_event},
    {NULL, "sceUserServiceGetUserColor", (void *)user_color},
    {NULL, "sceSystemServiceParamGetInt", (void *)system_param},
    {NULL, "sceSystemServiceGetStatus", (void *)system_status},
    {NULL, "sceSystemServiceReceiveEvent", (void *)system_event},
    {NULL, "sceSystemServiceHideSplashScreen", (void *)hide_splash},
    {NULL, "sceNetPoolCreate", (void *)net_pool_create},
    {NULL, "sceNetPoolDestroy", (void *)ok_void},
    {NULL, "sceNetHtons", (void *)net_swap16},
    {NULL, "sceNetNtohs", (void *)net_swap16},
    {NULL, "sceNetInetPton", (void *)net_pton},
    {NULL, "sceNetInetNtop", (void *)net_ntop},
    {NULL, "sceNetSocket", (void *)net_unreachable},
    {NULL, "sceNetConnect", (void *)net_unreachable},
    {NULL, "sceNetBind", (void *)net_unreachable},
    {NULL, "sceNetListen", (void *)net_unreachable},
    {NULL, "sceNetAccept", (void *)net_unreachable},
    {NULL, "sceNetSend", (void *)net_unreachable},
    {NULL, "sceNetSendto", (void *)net_unreachable},
    {NULL, "sceNetRecv", (void *)net_unreachable},
    {NULL, "sceNetRecvfrom", (void *)net_unreachable},
    {NULL, "sceNetSetsockopt", (void *)net_unreachable},
    {NULL, "sceNetSocketClose", (void *)net_unreachable},
    {NULL, "sceNetGetSockInfo", (void *)net_zero_info},
    {NULL, "sceNetGetStatisticsInfo", (void *)net_zero_info},
    {NULL, "sceNetCtlGetState", (void *)netctl_state},
    {NULL, "sceNetCtlGetInfo", (void *)netctl_info},
    {NULL, "sceNetCtlRegisterCallback", (void *)netctl_register},
    {NULL, "sceNetCtlCheckCallback", (void *)ok_void},
    {NULL, "sceNetCtlUnregisterCallback", (void *)ok_void},
    {NULL, "sceNetCtlGetResult", (void *)netctl_result},
    {NULL, "sceSslInit", (void *)lib_init_id},
    {NULL, "sceSslTerm", (void *)ok_void},
    {NULL, "sceHttpInit", (void *)lib_init_id},
    {NULL, "sceHttpTerm", (void *)ok_void},
    {NULL, "sceRudpInit", (void *)ok_void},
    {NULL, "sceRudpEnd", (void *)ok_void},
    {NULL, "sceRudpTerminate", (void *)ok_void},
    {NULL, "sceRudpEnableInternalIOThread", (void *)ok_void},
    {NULL, "sceRudpSetEventHandler", (void *)ok_void},
    {NULL, "sceRudpSetOption", (void *)ok_void},
    {NULL, "sceRudpCreateContext", (void *)net_unreachable},
    {NULL, "sceRudpBind", (void *)net_unreachable},
    {NULL, "sceRudpInitiate", (void *)net_unreachable},
    {NULL, "sceRudpRead", (void *)net_unreachable},
    {NULL, "sceRudpWrite", (void *)net_unreachable},
    {NULL, "sceRudpFlush", (void *)ok_void},
    {NULL, "sceRudpNetFlush", (void *)ok_void},
    {NULL, "sceRudpGetNumberOfPacketsToRead", (void *)rudp_zero_count},
    {NULL, "sceRudpGetSizeReadable", (void *)rudp_zero_count},
    {NULL, "sceNpGetState", (void *)np_state},
    {NULL, "sceNpGetOnlineId", (void *)np_signed_out},
    {NULL, "sceNpGetNpId", (void *)np_signed_out},
    {NULL, "sceNpRegisterStateCallback", (void *)np_register},
    {NULL, "sceNpUnregisterStateCallback", (void *)ok_void},
    {NULL, "sceNpCheckCallback", (void *)ok_void},
    {NULL, "sceNpNotifyPlusFeature", (void *)ok_void},
    {NULL, "sceNpSetContentRestriction", (void *)ok_void},
    {NULL, "sceNpCreateRequest", (void *)np_request},
    {NULL, "sceNpDeleteRequest", (void *)ok_void},
    {NULL, "sceNpAbortRequest", (void *)ok_void},
    {NULL, "sceNpCheckNpAvailability", (void *)np_signed_out},
    {NULL, "sceNpCheckPlus", (void *)np_signed_out},
    {NULL, "sceNpCmpOnlineId", (void *)np_compare},
    {NULL, "sceNpScoreCreateNpTitleCtx", (void *)np_request},
    {NULL, "sceNpScoreDeleteNpTitleCtx", (void *)ok_void},
    {NULL, "sceNpScoreCreateRequest", (void *)np_request},
    {NULL, "sceNpScoreDeleteRequest", (void *)ok_void},
    {NULL, "sceNpScoreAbortRequest", (void *)ok_void},
    {NULL, "sceNpScoreGetRankingByRange", (void *)np_signed_out},
    {NULL, "sceNpScoreGetFriendsRanking", (void *)np_signed_out},
    {NULL, "sceNpScoreGetRankingByNpId", (void *)np_signed_out},
    {NULL, "sceNpScoreRecordScore", (void *)np_signed_out},
    {NULL, "sceNpWebApiInitialize", (void *)lib_init_id},
    {NULL, "sceNpWebApiTerminate", (void *)ok_void},
    {NULL, "sceNpWebApiCreateContext", (void *)np_signed_out},
    {NULL, "sceNpWebApiCreateRequest", (void *)np_signed_out},
    {NULL, "sceNpWebApiSendRequest", (void *)np_signed_out},
    {NULL, "sceNpWebApiDeleteRequest", (void *)ok_void},
    {NULL, "sceNpWebApiDeleteContext", (void *)ok_void},
    {NULL, "sceNpWebApiReadData", (void *)np_signed_out},
    {NULL, "sceNpWebApiGetHttpStatusCode", (void *)np_signed_out},
    {NULL, "sceNpWebApiGetErrorCode", (void *)np_signed_out},
    {NULL, "sceNpWebApiUtilityParseNpId", (void *)np_signed_out},
    {NULL, "sceNpMatching2Initialize", (void *)ok_void},
    {NULL, "sceNpMatching2Terminate", (void *)ok_void},
    {NULL, "sceNpMatching2CreateContext", (void *)np_signed_out},
    {NULL, "sceNpMatching2DestroyContext", (void *)ok_void},
    {NULL, "sceNpMatching2ContextStart", (void *)np_signed_out},
    {NULL, "sceNpMatching2ContextStop", (void *)ok_void},
    {NULL, "sceNpMatching2AbortContextStart", (void *)ok_void},
    {NULL, "sceNpMatching2RegisterContextCallback", (void *)ok_void},
    {NULL, "sceNpMatching2RegisterRoomEventCallback", (void *)ok_void},
    {NULL, "sceNpMatching2RegisterSignalingCallback", (void *)ok_void},
    {NULL, "sceNpMatching2SetDefaultRequestOptParam", (void *)ok_void},
    {NULL, "sceNpMatching2CreateJoinRoom", (void *)np_signed_out},
    {NULL, "sceNpMatching2JoinRoom", (void *)np_signed_out},
    {NULL, "sceNpMatching2LeaveRoom", (void *)np_signed_out},
    {NULL, "sceNpMatching2SearchRoom", (void *)np_signed_out},
    {NULL, "sceNpMatching2GetServerId", (void *)np_signed_out},
    {NULL, "sceNpMatching2GetWorldInfoList", (void *)np_signed_out},
    {NULL, "sceNpMatching2GetMemoryInfo", (void *)np_signed_out},
    {NULL, "sceNpMatching2GetRoomDataExternalList", (void *)np_signed_out},
    {NULL, "sceNpMatching2GetUserInfoList", (void *)np_signed_out},
    {NULL, "sceNpMatching2SetRoomDataInternal", (void *)np_signed_out},
    {NULL, "sceNpMatching2SetRoomMemberDataInternal", (void *)np_signed_out},
    {NULL, "sceNpMatching2SignalingGetConnectionStatus", (void *)np_signed_out},
    {NULL, "sceNpMatching2SignalingGetPingInfo", (void *)np_signed_out},
    {NULL, "sceNpMatching2SignalingGetConnectionInfo", (void *)np_signed_out},
    {NULL, "sceNpMatching2SignalingGetLocalNetInfo", (void *)np_signed_out},
    {NULL, "sceNpMatching2SignalingCancelPeerNetInfo", (void *)np_signed_out},
    {NULL, "sceNpTrophyCreateContext", (void *)trophy_context},
    {NULL, "sceNpTrophyCreateHandle", (void *)trophy_handle},
    {NULL, "sceNpTrophyRegisterContext", (void *)ok_void},
    {NULL, "sceNpTrophyUnlockTrophy", (void *)trophy_unlock},
    {NULL, "sceNpTrophyDestroyContext", (void *)ok_void},
    {NULL, "sceNpTrophyDestroyHandle", (void *)ok_void},
    {NULL, "sceNpTrophyGetTrophyUnlockState", (void *)trophy_unlock_state},
    {NULL, "sceCommonDialogInitialize", (void *)common_init},
    {NULL, "sceMsgDialogInitialize", (void *)msg_init},
    {NULL, "sceMsgDialogOpen", (void *)msg_open},
    {NULL, "sceMsgDialogUpdateStatus", (void *)msg_status},
    {NULL, "sceMsgDialogClose", (void *)msg_close},
    {NULL, "sceMsgDialogGetResult", (void *)msg_result},
    {NULL, "sceMsgDialogTerminate", (void *)msg_term},
    {NULL, "sceSaveDataDialogInitialize", (void *)save_init},
    {NULL, "sceSaveDataDialogOpen", (void *)save_open},
    {NULL, "sceSaveDataDialogUpdateStatus", (void *)save_status},
    {NULL, "sceSaveDataDialogClose", (void *)save_close},
    {NULL, "sceSaveDataDialogGetStatus", (void *)save_status},
    {NULL, "sceSaveDataDialogGetResult", (void *)save_result},
    {NULL, "sceSaveDataDialogTerminate", (void *)save_term},
    {NULL, "sceSaveDataDialogIsReadyToDisplay", (void *)save_ready},
    {NULL, "sceSaveDataDialogProgressBarSetValue", (void *)save_progress},
    {NULL, "sceSaveDataDialogProgressBarInc", (void *)save_progress_inc},
    {NULL, "sceNpCommerceDialogInitialize", (void *)commerce_init},
    {NULL, "sceNpCommerceDialogOpen", (void *)commerce_open},
    {NULL, "sceNpCommerceDialogUpdateStatus", (void *)commerce_status},
    {NULL, "sceNpCommerceDialogClose", (void *)commerce_close},
    {NULL, "sceNpCommerceDialogGetResult", (void *)commerce_result},
    {NULL, "sceNpCommerceDialogTerminate", (void *)commerce_term},
    {NULL, "sceNpFriendListDialogInitialize", (void *)friends_init},
    {NULL, "sceNpFriendListDialogOpen", (void *)friends_open},
    {NULL, "sceNpFriendListDialogUpdateStatus", (void *)friends_status},
    {NULL, "sceNpFriendListDialogGetStatus", (void *)friends_status},
    {NULL, "sceNpFriendListDialogClose", (void *)friends_close},
    {NULL, "sceNpFriendListDialogGetResult", (void *)friends_result},
    {NULL, "sceNpFriendListDialogTerminate", (void *)friends_term},
    {NULL, "sceErrorDialogInitialize", (void *)error_init},
    {NULL, "sceErrorDialogOpen", (void *)error_open},
    {NULL, "sceErrorDialogUpdateStatus", (void *)error_status},
    {NULL, "sceErrorDialogClose", (void *)error_close},
    {NULL, "sceErrorDialogGetStatus", (void *)error_status},
    {NULL, "sceErrorDialogTerminate", (void *)error_term},
    {NULL, "scePlayGoInitialize", (void *)playgo_init},
    {NULL, "scePlayGoOpen", (void *)playgo_open},
    {NULL, "scePlayGoGetChunkId", (void *)playgo_chunk_ids},
    {NULL, "scePlayGoGetLocus", (void *)playgo_locus},
    {NULL, "scePlayGoSetLanguageMask", (void *)playgo_language_mask},
    /* Console features without a PC counterpart: accepted, nothing happens. */
    {NULL, "sceScreenShotDisable", (void *)ok_void},
    {NULL, "sceScreenShotEnable", (void *)ok_void},
    {NULL, "sceScreenShotSetOverlayImage", (void *)ok_void},
    {NULL, "sceScreenShotSetParam", (void *)ok_void},
    {NULL, "sceGameLiveStreamingInitialize", (void *)ok_void},
    {NULL, "sceGameLiveStreamingTerminate", (void *)ok_void},
    {NULL, "sceGameLiveStreamingEnableLiveStreaming", (void *)ok_void},
    {NULL, "sceGameLiveStreamingPermitLiveStreaming", (void *)ok_void},
    {NULL, "sceGameLiveStreamingPermitServerSideRecording", (void *)ok_void},
    {NULL, "sceSharePlayInitialize", (void *)ok_void},
    {NULL, "sceSharePlaySetProhibition", (void *)ok_void},
    {NULL, "sceSharePlayTerminate", (void *)ok_void},
    {NULL, "sceVideoRecordingSetInfo", (void *)ok_void},
};
uintptr_t gz_services_resolve(const char *library, const char *symbol) {
    return GZ_LOOKUP(exports, library, symbol);
}
