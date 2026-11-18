/* SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL */

#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif

#include "ecp.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define CHIAKI_ECP_VERSION "1.10.1-ecp1"
#define MAX_HOSTS 32
#define MAX_FILE_BYTES (64u * 1024u)

static const char *const DESC_SYSTEMS[] = { "psremote", NULL };
static const char *const DESC_EXTS[] = { "chiaki", "psremote", NULL };
static const char *const DESC_CAPS[] = { "settings", "oneshot", NULL };

static const ecp_descriptor DESC = {
    .id = "io.github.denarmo.chiaki",
    .name = "Chiaki-NG",
    .version = CHIAKI_ECP_VERSION,
    .level = 2,
    .systems = DESC_SYSTEMS,
    .extensions = DESC_EXTS,
    .capabilities = DESC_CAPS,
    .needs = NULL,
    .settings_schema = 1
};

/* ---- Portable C11 helpers ---- */

static int xstrcasecmp(const char *a, const char *b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb) return (int)ca - (int)cb;
        a++;
        b++;
    }
    return (int)(unsigned char)tolower((unsigned char)*a) -
           (int)(unsigned char)tolower((unsigned char)*b);
}

static void trim_inplace(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    size_t start = 0;
    while (start < len && isspace((unsigned char)s[start])) start++;
    while (len > start && isspace((unsigned char)s[len - 1])) len--;
    if (start > 0) memmove(s, s + start, len - start);
    s[len - start] = '\0';
}

#ifndef _WIN32
static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}
#endif

static void mkdir_p_parent(const char *file_path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", file_path);
    size_t len = strlen(buf);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char saved = buf[i];
            buf[i] = '\0';
#ifdef _WIN32
            if (strlen(buf) > 2 || buf[1] != ':') (void)_mkdir(buf);
#else
            (void)mkdir(buf, 0755);
#endif
            buf[i] = saved;
        }
    }
}

static char *read_file_capped(const char *path, size_t *out_len) {
    if (!path || !*path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > MAX_FILE_BYTES) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static int file_exists_exec(const char *path) {
    if (!path || !*path) return 0;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return access(path, X_OK) == 0;
#endif
}

static int normalize_mac(const char *raw, char out_hex[13]) {
    if (!raw || !*raw) return 0;
    size_t j = 0;
    for (const char *p = raw; *p; p++) {
        if (*p == ':' || *p == '-' || isspace((unsigned char)*p)) continue;
        if (!isxdigit((unsigned char)*p) || j >= 12) return 0;
        out_hex[j++] = (char)tolower((unsigned char)*p);
    }
    if (j != 12) return 0;
    out_hex[12] = '\0';
    return 1;
}

/* ---- Settings & Schema ---- */

typedef struct ecp_settings_state {
    int audio_volume;           /* 0..100 */
    char video_resolution[16];  /* 360p, 540p, 720p, 1080p */
    char video_fps[8];          /* 30, 60 */
    char video_codec[16];       /* h264, h265, h265_hdr */
    int video_bitrate;          /* 0..50000 */
    char video_display_mode[16];/* normal, zoom, stretch */
    int video_opengl;           /* 0 or 1 */
    int input_dualsense;        /* 0 or 1 */
} ecp_settings_state;

typedef struct app_state {
    ecp_settings_state settings;
    int dry_run;
    int should_exit;
    int running;
    int stop_requested;
#ifdef _WIN32
    HANDLE child_process;
    HANDLE child_job;
#else
    pid_t child_pid;
#endif
} app_state;

static void settings_init_defaults(ecp_settings_state *s) {
    memset(s, 0, sizeof *s);
    s->audio_volume = 100;
    snprintf(s->video_resolution, sizeof s->video_resolution, "1080p");
    snprintf(s->video_fps, sizeof s->video_fps, "60");
    snprintf(s->video_codec, sizeof s->video_codec, "h265");
    s->video_bitrate = 0;
    snprintf(s->video_display_mode, sizeof s->video_display_mode, "normal");
    s->video_opengl = 0;
    s->input_dualsense = 1;
}

static int settings_file_path(char *out, size_t out_cap) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata) return -1;
    snprintf(out, out_cap, "%s\\Chiaki\\ecp-settings.json", appdata);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, out_cap, "%s/Chiaki/ecp-settings.json", xdg);
    else if (home && *home)
        snprintf(out, out_cap, "%s/.config/Chiaki/ecp-settings.json", home);
    else
        return -1;
#endif
    return 0;
}

static void w_choice(ecp_writer *w, const char *val, const char *label) {
    ecp_w_array_begin(w);
    ecp_w_string(w, val);
    ecp_w_string(w, label);
    ecp_w_array_end(w);
}

static void write_schema_settings_array(ecp_writer *w) {
    ecp_w_array_begin(w);

    /* audio.volume */
    ecp_w_object_begin(w);
    ecp_w_kstring(w, "key", "audio.volume");
    ecp_w_kstring(w, "label", "Audio Volume (%)");
    ecp_w_kstring(w, "help", "Audio volume target for Remote Play.");
    ecp_w_kstring(w, "type", "range");
    ecp_w_kint(w, "min", 0);
    ecp_w_kint(w, "max", 100);
    ecp_w_kint(w, "step", 5);
    ecp_w_kint(w, "default", 100);
    ecp_w_kstring(w, "group", "Audio");
    ecp_w_kstring(w, "applies", "restart");
    ecp_w_object_end(w);

    /* video.resolution */
    ecp_w_object_begin(w);
    ecp_w_kstring(w, "key", "video.resolution");
    ecp_w_kstring(w, "label", "Stream Resolution");
    ecp_w_kstring(w, "help", "Video stream height requested from the console.");
    ecp_w_kstring(w, "type", "choice");
    ecp_w_key(w, "choices");
    ecp_w_array_begin(w);
    w_choice(w, "360p", "360p");
    w_choice(w, "540p", "540p");
    w_choice(w, "720p", "720p");
    w_choice(w, "1080p", "1080p");
    ecp_w_array_end(w);
    ecp_w_kstring(w, "default", "1080p");
    ecp_w_kstring(w, "group", "Video");
    ecp_w_kstring(w, "applies", "restart");
    ecp_w_object_end(w);

    /* video.fps */
    ecp_w_object_begin(w);
    ecp_w_kstring(w, "key", "video.fps");
    ecp_w_kstring(w, "label", "Frame Rate");
    ecp_w_kstring(w, "help", "Maximum frames per second requested from the console.");
    ecp_w_kstring(w, "type", "choice");
    ecp_w_key(w, "choices");
    ecp_w_array_begin(w);
    w_choice(w, "30", "30 FPS");
    w_choice(w, "60", "60 FPS");
    ecp_w_array_end(w);
    ecp_w_kstring(w, "default", "60");
    ecp_w_kstring(w, "group", "Video");
    ecp_w_kstring(w, "applies", "restart");
    ecp_w_object_end(w);

    /* video.codec */
    ecp_w_object_begin(w);
    ecp_w_kstring(w, "key", "video.codec");
    ecp_w_kstring(w, "label", "Video Codec");
    ecp_w_kstring(w, "help", "Video codec used for the stream.");
    ecp_w_kstring(w, "type", "choice");
    ecp_w_key(w, "choices");
    ecp_w_array_begin(w);
    w_choice(w, "h264", "H.264");
    w_choice(w, "h265", "H.265 (HEVC)");
    w_choice(w, "h265_hdr", "H.265 HDR");
    ecp_w_array_end(w);
    ecp_w_kstring(w, "default", "h265");
    ecp_w_kstring(w, "group", "Video");
    ecp_w_kstring(w, "applies", "restart");
    ecp_w_object_end(w);

    /* video.bitrate */
    ecp_w_object_begin(w);
    ecp_w_kstring(w, "key", "video.bitrate");
    ecp_w_kstring(w, "label", "Bitrate in kbps (0 = Auto)");
    ecp_w_kstring(w, "help", "0 lets Chiaki choose automatic bitrate; otherwise 2000 to 50000 kbps.");
    ecp_w_kstring(w, "type", "range");
    ecp_w_kint(w, "min", 0);
    ecp_w_kint(w, "max", 50000);
    ecp_w_kint(w, "step", 1000);
    ecp_w_kint(w, "default", 0);
    ecp_w_kstring(w, "group", "Video");
    ecp_w_kstring(w, "applies", "restart");
    ecp_w_object_end(w);

    /* video.display_mode */
    ecp_w_object_begin(w);
    ecp_w_kstring(w, "key", "video.display_mode");
    ecp_w_kstring(w, "label", "Fullscreen Mode");
    ecp_w_kstring(w, "help", "How chiaki-ng fills fullscreen when launched.");
    ecp_w_kstring(w, "type", "choice");
    ecp_w_key(w, "choices");
    ecp_w_array_begin(w);
    w_choice(w, "normal", "Normal (Letterboxed)");
    w_choice(w, "zoom", "Zoomed");
    w_choice(w, "stretch", "Stretched");
    ecp_w_array_end(w);
    ecp_w_kstring(w, "default", "normal");
    ecp_w_kstring(w, "group", "Video");
    ecp_w_kstring(w, "applies", "restart");
    ecp_w_object_end(w);

    /* video.opengl */
    ecp_w_object_begin(w);
    ecp_w_kstring(w, "key", "video.opengl");
    ecp_w_kstring(w, "label", "Force OpenGL Renderer");
    ecp_w_kstring(w, "help", "Passes --opengl to chiaki-ng if Vulkan has issues on this GPU.");
    ecp_w_kstring(w, "type", "toggle");
    ecp_w_kbool(w, "default", 0);
    ecp_w_kstring(w, "group", "Video");
    ecp_w_kstring(w, "applies", "restart");
    ecp_w_object_end(w);

    /* input.dualsense */
    ecp_w_object_begin(w);
    ecp_w_kstring(w, "key", "input.dualsense");
    ecp_w_kstring(w, "label", "DualSense Haptics & Adaptive Triggers");
    ecp_w_kstring(w, "help", "Passes --dualsense for USB-connected DualSense controllers.");
    ecp_w_kstring(w, "type", "toggle");
    ecp_w_kbool(w, "default", 1);
    ecp_w_kstring(w, "group", "Input");
    ecp_w_kstring(w, "applies", "restart");
    ecp_w_object_end(w);

    ecp_w_array_end(w);
}

static void write_settings_values(ecp_writer *w, const ecp_settings_state *s) {
    ecp_w_object_begin(w);
    ecp_w_kint(w, "audio.volume", s->audio_volume);
    ecp_w_kstring(w, "video.resolution", s->video_resolution);
    ecp_w_kstring(w, "video.fps", s->video_fps);
    ecp_w_kstring(w, "video.codec", s->video_codec);
    ecp_w_kint(w, "video.bitrate", s->video_bitrate);
    ecp_w_kstring(w, "video.display_mode", s->video_display_mode);
    ecp_w_kbool(w, "video.opengl", s->video_opengl);
    ecp_w_kbool(w, "input.dualsense", s->input_dualsense);
    ecp_w_object_end(w);
}

static int apply_one_setting(ecp_settings_state *s, const char *key, const ecp_json *val,
                             char *err, size_t errlen) {
    if (!key || !*key || !val || ecp_json_type_of(val) == ECP_JSON_NULL) {
        snprintf(err, errlen, "Missing key or value");
        return -1;
    }
    ecp_json_type t = ecp_json_type_of(val);
    if (strcmp(key, "audio.volume") == 0) {
        if (t != ECP_JSON_NUMBER) { snprintf(err, errlen, "audio.volume expects a number"); return -1; }
        int64_t n = ecp_json_int(val, -1);
        if (n < 0 || n > 100) { snprintf(err, errlen, "audio.volume out of range 0..100"); return -1; }
        s->audio_volume = (int)n;
        return 0;
    }
    if (strcmp(key, "video.resolution") == 0) {
        const char *v = t == ECP_JSON_STRING ? ecp_json_str(val, "") : NULL;
        if (!v || (strcmp(v, "360p") && strcmp(v, "540p") && strcmp(v, "720p") && strcmp(v, "1080p"))) {
            snprintf(err, errlen, "Invalid video.resolution");
            return -1;
        }
        snprintf(s->video_resolution, sizeof s->video_resolution, "%s", v);
        return 0;
    }
    if (strcmp(key, "video.fps") == 0) {
        const char *v = t == ECP_JSON_STRING ? ecp_json_str(val, "") : NULL;
        if (!v || (strcmp(v, "30") && strcmp(v, "60"))) {
            snprintf(err, errlen, "Invalid video.fps");
            return -1;
        }
        snprintf(s->video_fps, sizeof s->video_fps, "%s", v);
        return 0;
    }
    if (strcmp(key, "video.codec") == 0) {
        const char *v = t == ECP_JSON_STRING ? ecp_json_str(val, "") : NULL;
        if (!v || (strcmp(v, "h264") && strcmp(v, "h265") && strcmp(v, "h265_hdr"))) {
            snprintf(err, errlen, "Invalid video.codec");
            return -1;
        }
        snprintf(s->video_codec, sizeof s->video_codec, "%s", v);
        return 0;
    }
    if (strcmp(key, "video.bitrate") == 0) {
        if (t != ECP_JSON_NUMBER) { snprintf(err, errlen, "video.bitrate expects a number"); return -1; }
        int64_t n = ecp_json_int(val, -1);
        if (n < 0 || n > 50000) { snprintf(err, errlen, "video.bitrate out of range 0..50000"); return -1; }
        s->video_bitrate = (int)n;
        return 0;
    }
    if (strcmp(key, "video.display_mode") == 0) {
        const char *v = t == ECP_JSON_STRING ? ecp_json_str(val, "") : NULL;
        if (!v || (strcmp(v, "normal") && strcmp(v, "zoom") && strcmp(v, "stretch"))) {
            snprintf(err, errlen, "Invalid video.display_mode");
            return -1;
        }
        snprintf(s->video_display_mode, sizeof s->video_display_mode, "%s", v);
        return 0;
    }
    if (strcmp(key, "video.opengl") == 0) {
        if (t != ECP_JSON_BOOL) { snprintf(err, errlen, "video.opengl expects a boolean"); return -1; }
        s->video_opengl = ecp_json_bool(val, 0) ? 1 : 0;
        return 0;
    }
    if (strcmp(key, "input.dualsense") == 0) {
        if (t != ECP_JSON_BOOL) { snprintf(err, errlen, "input.dualsense expects a boolean"); return -1; }
        s->input_dualsense = ecp_json_bool(val, 1) ? 1 : 0;
        return 0;
    }
    snprintf(err, errlen, "Unknown setting '%s'", key);
    return -1;
}

static void settings_load(app_state *st) {
    char path[1024];
    if (settings_file_path(path, sizeof path) != 0) return;
    size_t len = 0;
    char *text = read_file_capped(path, &len);
    if (!text) return;
    ecp_json *obj = ecp_json_parse(text, len);
    free(text);
    if (!obj || ecp_json_type_of(obj) != ECP_JSON_OBJECT) {
        ecp_json_free(obj);
        return;
    }
    size_t n = ecp_json_len(obj);
    for (size_t i = 0; i < n; i++) {
        char dummy[128];
        (void)apply_one_setting(&st->settings, ecp_json_key_at(obj, i), ecp_json_at(obj, i), dummy, sizeof dummy);
    }
    ecp_json_free(obj);
}

static void settings_save(const app_state *st) {
    if (st->dry_run) return;
    char path[1024];
    if (settings_file_path(path, sizeof path) != 0) return;
    mkdir_p_parent(path);
    ecp_writer w = {0};
    write_settings_values(&w, &st->settings);
    if (!w.failed && w.buf) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(w.buf, 1, w.len, f);
            fputc('\n', f);
            fclose(f);
        }
    }
    ecp_w_free(&w);
}

/* ---- Locate sibling chiaki GUI executable ---- */

static int sibling_path(const char *dir, const char *name, char *out, size_t out_cap) {
    size_t dlen = strlen(dir), nlen = strlen(name);
    if (dlen + 1 + nlen >= out_cap) return -1;
    memcpy(out, dir, dlen);
#ifdef _WIN32
    out[dlen] = '\\';
#else
    out[dlen] = '/';
#endif
    memcpy(out + dlen + 1, name, nlen + 1);
    return file_exists_exec(out) ? 0 : -1;
}

static int find_chiaki_binary(char *out, size_t out_cap) {
    char exe[1024];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (n == 0 || n >= sizeof exe) return -1;
    char *slash = strrchr(exe, '\\');
    char *fslash = strrchr(exe, '/');
    if (fslash && (!slash || fslash > slash)) slash = fslash;
    if (!slash) return -1;
    *slash = '\0';
    if (sibling_path(exe, "chiaki.exe", out, out_cap) == 0) return 0;
    if (sibling_path(exe, "chiaki-ng.exe", out, out_cap) == 0) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0 || (size_t)n >= sizeof exe) return -1;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return -1;
    *slash = '\0';
    if (sibling_path(exe, "chiaki-ng.AppImage", out, out_cap) == 0) return 0;
    if (sibling_path(exe, "chiaki", out, out_cap) == 0) return 0;
    if (sibling_path(exe, "chiaki-ng", out, out_cap) == 0) return 0;
#endif
    out[0] = '\0';
    return -1;
}

/* ---- Qt registered_hosts reading ---- */

typedef struct reg_host {
    char nickname[256];
    char mac_hex[13];
    int has_mac;
    char regist_key_hex[32];
    int has_regist_key;
    int is_ps5;
    int target;
} reg_host;

typedef struct host_list {
    reg_host items[MAX_HOSTS];
    int count;
} host_list;

static size_t decode_qbytearray(const char *val, unsigned char *out, size_t max_out) {
    const char *start = strstr(val, "@ByteArray(");
    const char *p = start ? start + 11 : val;
    const char *end = start ? strrchr(p, ')') : (val + strlen(val));
    if (!end || end < p) end = val + strlen(val);

    size_t len = 0;
    while (p < end && len < max_out) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            if (*p == 'x' && p + 2 < end && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                char hx[3] = { p[1], p[2], '\0' };
                out[len++] = (unsigned char)strtoul(hx, NULL, 16);
                p += 3;
            } else if (*p >= '0' && *p <= '7') {
                unsigned v = 0;
                for (int k = 0; k < 3 && p < end && *p >= '0' && *p <= '7'; k++, p++)
                    v = (v * 8u) + (unsigned)(*p - '0');
                out[len++] = (unsigned char)(v & 0xffu);
            } else if (*p == '0') {
                out[len++] = 0;
                p++;
            } else if (*p == 'n') {
                out[len++] = '\n';
                p++;
            } else if (*p == 'r') {
                out[len++] = '\r';
                p++;
            } else if (*p == 't') {
                out[len++] = '\t';
                p++;
            } else {
                out[len++] = (unsigned char)*p++;
            }
        } else {
            out[len++] = (unsigned char)*p++;
        }
    }
    return len;
}

static void mac_bytes_to_hex(const unsigned char mac[6], char out[13]) {
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

#ifdef _WIN32
static void load_hosts_windows_registry(host_list *list) {
    HKEY root = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Chiaki\\Chiaki\\registered_hosts", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return;
    DWORD idx = 0;
    char subname[64];
    for (;;) {
        DWORD sublen = sizeof subname;
        if (RegEnumKeyExA(root, idx++, subname, &sublen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (list->count >= MAX_HOSTS) break;
        HKEY hsub = NULL;
        if (RegOpenKeyExA(root, subname, 0, KEY_READ, &hsub) != ERROR_SUCCESS)
            continue;
        reg_host rh;
        memset(&rh, 0, sizeof rh);

        DWORD type = 0, cb = sizeof rh.nickname - 1;
        if (RegQueryValueExA(hsub, "server_nickname", NULL, &type, (LPBYTE)rh.nickname, &cb) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            rh.nickname[cb < sizeof rh.nickname ? cb : sizeof rh.nickname - 1] = '\0';
        }

        DWORD target_val = 0;
        cb = sizeof target_val;
        if (RegQueryValueExA(hsub, "target", NULL, &type, (LPBYTE)&target_val, &cb) == ERROR_SUCCESS) {
            if (type == REG_DWORD) rh.target = (int)target_val;
            else if (type == REG_SZ) {
                char tbuf[32];
                DWORD tcb = sizeof tbuf - 1;
                if (RegQueryValueExA(hsub, "target", NULL, &type, (LPBYTE)tbuf, &tcb) == ERROR_SUCCESS)
                    rh.target = atoi(tbuf);
            }
            rh.is_ps5 = rh.target >= 1000000;
        }

        unsigned char raw[64];
        cb = sizeof raw;
        if (RegQueryValueExA(hsub, "server_mac", NULL, &type, raw, &cb) == ERROR_SUCCESS) {
            if (type == REG_BINARY && cb == 6) {
                mac_bytes_to_hex(raw, rh.mac_hex);
                rh.has_mac = 1;
            } else if (type == REG_SZ && cb > 0) {
                raw[cb < sizeof raw ? cb : sizeof raw - 1] = '\0';
                unsigned char mac[8];
                if (decode_qbytearray((const char *)raw, mac, sizeof mac) == 6) {
                    mac_bytes_to_hex(mac, rh.mac_hex);
                    rh.has_mac = 1;
                }
            }
        }

        cb = sizeof raw;
        if (RegQueryValueExA(hsub, "rp_regist_key", NULL, &type, raw, &cb) == ERROR_SUCCESS) {
            unsigned char dec[32];
            size_t dlen = 0;
            if (type == REG_BINARY && cb > 0) {
                dlen = cb < sizeof dec - 1 ? cb : sizeof dec - 1;
                memcpy(dec, raw, dlen);
            } else if (type == REG_SZ && cb > 0) {
                raw[cb < sizeof raw ? cb : sizeof raw - 1] = '\0';
                dlen = decode_qbytearray((const char *)raw, dec, sizeof dec - 1);
            }
            if (dlen > 0) {
                dec[dlen] = '\0';
                size_t k = 0;
                for (size_t z = 0; z < dlen && dec[z] != '\0' && k < sizeof rh.regist_key_hex - 1; z++) {
                    if (isxdigit(dec[z])) rh.regist_key_hex[k++] = (char)dec[z];
                }
                rh.regist_key_hex[k] = '\0';
                if (k > 0) rh.has_regist_key = 1;
            }
        }

        RegCloseKey(hsub);
        if (rh.nickname[0] || rh.has_mac) {
            list->items[list->count++] = rh;
        }
    }
    RegCloseKey(root);
}
#endif

static void load_hosts_chiaki_conf(host_list *list) {
    char path[1024];
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata) return;
    snprintf(path, sizeof path, "%s\\Chiaki\\Chiaki.conf", appdata);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(path, sizeof path, "%s/Chiaki/Chiaki.conf", xdg);
    else if (home && *home) snprintf(path, sizeof path, "%s/.config/Chiaki/Chiaki.conf", home);
    else return;
#endif
    FILE *f = fopen(path, "rb");
    if (!f) return;

    char line[2048];
    int in_reg = 0;
    int indices[MAX_HOSTS];
    for (int i = 0; i < MAX_HOSTS; i++) indices[i] = -1;

    while (fgets(line, sizeof line, f)) {
        trim_inplace(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            in_reg = (strcmp(line, "[registered_hosts]") == 0);
            continue;
        }
        if (!in_reg) continue;
        char *bs = strchr(line, '\\');
        char *eq = strchr(line, '=');
        if (!bs || !eq || bs > eq) continue;
        *bs = '\0';
        *eq = '\0';
        int idx = atoi(line);
        if (idx <= 0) continue;
        const char *key = bs + 1;
        char *val = eq + 1;
        size_t vlen = strlen(val);
        if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
            val[vlen - 1] = '\0';
            val++;
        }

        int slot = -1;
        for (int i = 0; i < list->count; i++) {
            if (indices[i] == idx) { slot = i; break; }
        }
        if (slot < 0) {
            if (list->count >= MAX_HOSTS) continue;
            slot = list->count++;
            memset(&list->items[slot], 0, sizeof list->items[slot]);
            indices[slot] = idx;
        }
        reg_host *rh = &list->items[slot];
        if (strcmp(key, "server_nickname") == 0) {
            snprintf(rh->nickname, sizeof rh->nickname, "%s", val);
        } else if (strcmp(key, "target") == 0) {
            rh->target = atoi(val);
            rh->is_ps5 = rh->target >= 1000000;
        } else if (strcmp(key, "server_mac") == 0) {
            unsigned char mac[8];
            if (decode_qbytearray(val, mac, sizeof mac) == 6) {
                mac_bytes_to_hex(mac, rh->mac_hex);
                rh->has_mac = 1;
            }
        } else if (strcmp(key, "rp_regist_key") == 0) {
            unsigned char dec[32];
            size_t dlen = decode_qbytearray(val, dec, sizeof dec - 1);
            if (dlen > 0) {
                dec[dlen] = '\0';
                size_t k = 0;
                for (size_t z = 0; z < dlen && dec[z] != '\0' && k < sizeof rh->regist_key_hex - 1; z++) {
                    if (isxdigit(dec[z])) rh->regist_key_hex[k++] = (char)dec[z];
                }
                rh->regist_key_hex[k] = '\0';
                if (k > 0) rh->has_regist_key = 1;
            }
        }
    }
    fclose(f);
}

static void load_hosts_cli_list(host_list *list, const char *chiaki_bin) {
    if (!chiaki_bin || !*chiaki_bin) return;
    char cmd[1200];
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "\"\"%s\" list\"", chiaki_bin);
    FILE *p = _popen(cmd, "r");
#else
    snprintf(cmd, sizeof cmd, "APPIMAGE_EXTRACT_AND_RUN=1 \"%s\" list 2>/dev/null", chiaki_bin);
    FILE *p = popen(cmd, "r");
#endif
    if (!p) return;
    char line[512];
    while (fgets(line, sizeof line, p)) {
        if (strncmp(line, "Host: ", 6) == 0 && list->count < MAX_HOSTS) {
            char *nick = line + 6;
            trim_inplace(nick);
            if (!nick[0]) continue;
            int exists = 0;
            for (int i = 0; i < list->count; i++) {
                if (strcmp(list->items[i].nickname, nick) == 0) { exists = 1; break; }
            }
            if (!exists) {
                reg_host rh;
                memset(&rh, 0, sizeof rh);
                snprintf(rh.nickname, sizeof rh.nickname, "%.255s", nick);
                list->items[list->count++] = rh;
            }
        }
    }
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
}

static void load_registered_hosts(host_list *list, const char *chiaki_bin) {
    memset(list, 0, sizeof *list);
#ifdef _WIN32
    load_hosts_windows_registry(list);
#endif
    if (list->count == 0)
        load_hosts_chiaki_conf(list);
    if (list->count == 0 && chiaki_bin && *chiaki_bin)
        load_hosts_cli_list(list, chiaki_bin);
}

/* ---- Sync video/audio settings into Chiaki Qt settings ---- */

#ifdef _WIN32
static void sync_chiaki_qt_settings(const ecp_settings_state *s) {
    HKEY hkey = NULL;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Chiaki\\Chiaki\\settings", 0, NULL,
                        0, KEY_WRITE, NULL, &hkey, NULL) != ERROR_SUCCESS)
        return;
    const char *res_keys[] = { "resolution_local_ps4", "resolution_remote_ps4", "resolution_local_ps5", "resolution_remote_ps5" };
    for (size_t i = 0; i < sizeof res_keys / sizeof res_keys[0]; i++) {
        RegSetValueExA(hkey, res_keys[i], 0, REG_SZ, (const BYTE *)s->video_resolution, (DWORD)strlen(s->video_resolution) + 1);
    }
    DWORD fps_dw = (DWORD)atoi(s->video_fps);
    const char *fps_keys[] = { "fps_local_ps4", "fps_remote_ps4", "fps_local_ps5", "fps_remote_ps5" };
    for (size_t i = 0; i < sizeof fps_keys / sizeof fps_keys[0]; i++) {
        RegSetValueExA(hkey, fps_keys[i], 0, REG_DWORD, (const BYTE *)&fps_dw, sizeof fps_dw);
    }
    const char *codec_ps4 = (strcmp(s->video_codec, "h265_hdr") == 0) ? "h265" : s->video_codec;
    RegSetValueExA(hkey, "codec_ps4", 0, REG_SZ, (const BYTE *)codec_ps4, (DWORD)strlen(codec_ps4) + 1);
    RegSetValueExA(hkey, "codec_local_ps5", 0, REG_SZ, (const BYTE *)s->video_codec, (DWORD)strlen(s->video_codec) + 1);
    RegSetValueExA(hkey, "codec_remote_ps5", 0, REG_SZ, (const BYTE *)s->video_codec, (DWORD)strlen(s->video_codec) + 1);

    DWORD br_dw = (DWORD)s->video_bitrate;
    const char *br_keys[] = { "bitrate_local_ps4", "bitrate_remote_ps4", "bitrate_local_ps5", "bitrate_remote_ps5" };
    for (size_t i = 0; i < sizeof br_keys / sizeof br_keys[0]; i++) {
        RegSetValueExA(hkey, br_keys[i], 0, REG_DWORD, (const BYTE *)&br_dw, sizeof br_dw);
    }
    RegCloseKey(hkey);
}
#else
static void sync_chiaki_qt_settings(const ecp_settings_state *s) {
    (void)s;
}
#endif

/* ---- Target specification parser ---- */

typedef struct target_spec {
    char host[256];
    char mac_hex[13];
    int has_mac;
    char nickname[256];
    char target_hint[32];
    int wakeup;
    char passcode[32];
} target_spec;

static int looks_like_ip(const char *s) {
    if (!s || !*s) return 0;
    struct in_addr a4;
    if (inet_pton(AF_INET, s, &a4) == 1) return 1;
    struct in6_addr a6;
    if (inet_pton(AF_INET6, s, &a6) == 1) return 1;
    return 0;
}

static int parse_inline_target(const char *raw, target_spec *out) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", raw ? raw : "");
    trim_inplace(buf);
    if (!buf[0]) return -1;

    char mac[13];
    if (normalize_mac(buf, mac)) {
        snprintf(out->mac_hex, sizeof out->mac_hex, "%s", mac);
        out->has_mac = 1;
        return 0;
    }
    if (looks_like_ip(buf)) {
        snprintf(out->host, sizeof out->host, "%.255s", buf);
        return 0;
    }
    snprintf(out->nickname, sizeof out->nickname, "%.255s", buf);
    return 0;
}

static void str_copy_opt(const ecp_json *obj, const char *k1, const char *k2, char *dst, size_t cap) {
    if (dst[0]) return;
    const char *v = ecp_json_str(ecp_json_get(obj, k1), NULL);
    if ((!v || !*v) && k2) v = ecp_json_str(ecp_json_get(obj, k2), NULL);
    if (v && *v) snprintf(dst, cap, "%s", v);
}

static int parse_target_text(const char *text, size_t len, target_spec *out) {
    memset(out, 0, sizeof *out);
    out->wakeup = 1;

    char *copy = (char *)malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, text, len);
    copy[len] = '\0';
    trim_inplace(copy);
    if (!copy[0]) {
        free(copy);
        return -1;
    }

    if (copy[0] == '{') {
        ecp_json *obj = ecp_json_parse(copy, strlen(copy));
        free(copy);
        if (!obj || ecp_json_type_of(obj) != ECP_JSON_OBJECT) {
            ecp_json_free(obj);
            return -1;
        }
        str_copy_opt(obj, "host", "ip", out->host, sizeof out->host);
        str_copy_opt(obj, "address", NULL, out->host, sizeof out->host);

        char raw_mac[64] = {0};
        str_copy_opt(obj, "mac", "server_mac", raw_mac, sizeof raw_mac);
        str_copy_opt(obj, "serverMac", NULL, raw_mac, sizeof raw_mac);
        if (raw_mac[0]) {
            if (!normalize_mac(raw_mac, out->mac_hex)) {
                ecp_json_free(obj);
                return -1;
            }
            out->has_mac = 1;
        }

        str_copy_opt(obj, "nickname", "name", out->nickname, sizeof out->nickname);
        str_copy_opt(obj, "server_nickname", "serverNickname", out->nickname, sizeof out->nickname);
        str_copy_opt(obj, "target", NULL, out->target_hint, sizeof out->target_hint);
        str_copy_opt(obj, "passcode", "pin", out->passcode, sizeof out->passcode);
        out->wakeup = ecp_json_bool(ecp_json_get(obj, "wakeup"), 1) ? 1 : 0;
        ecp_json_free(obj);

        if (!out->host[0] && !out->has_mac && !out->nickname[0] && !out->target_hint[0])
            return -1;
        return 0;
    }

    if (strchr(copy, '=')) {
        char *cur = copy;
        while (cur && *cur) {
            char *nl = strchr(cur, '\n');
            if (nl) *nl++ = '\0';
            trim_inplace(cur);
            if (cur[0] && cur[0] != '#' && cur[0] != ';') {
                char *eq = strchr(cur, '=');
                if (eq && eq > cur) {
                    *eq = '\0';
                    char *k = cur;
                    char *v = eq + 1;
                    trim_inplace(k);
                    trim_inplace(v);
                    size_t vl = strlen(v);
                    if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') {
                        v[vl - 1] = '\0';
                        v++;
                    }
                    if (xstrcasecmp(k, "host") == 0 || xstrcasecmp(k, "ip") == 0 || xstrcasecmp(k, "address") == 0)
                        snprintf(out->host, sizeof out->host, "%s", v);
                    else if (xstrcasecmp(k, "mac") == 0 || xstrcasecmp(k, "server_mac") == 0) {
                        if (normalize_mac(v, out->mac_hex)) out->has_mac = 1;
                    } else if (xstrcasecmp(k, "nickname") == 0 || xstrcasecmp(k, "name") == 0 || xstrcasecmp(k, "server_nickname") == 0)
                        snprintf(out->nickname, sizeof out->nickname, "%s", v);
                    else if (xstrcasecmp(k, "target") == 0)
                        snprintf(out->target_hint, sizeof out->target_hint, "%s", v);
                    else if (xstrcasecmp(k, "wakeup") == 0)
                        out->wakeup = (xstrcasecmp(v, "true") == 0 || strcmp(v, "1") == 0) ? 1 : 0;
                    else if (xstrcasecmp(k, "passcode") == 0 || xstrcasecmp(k, "pin") == 0)
                        snprintf(out->passcode, sizeof out->passcode, "%s", v);
                }
            }
            cur = nl;
        }
        free(copy);
        if (!out->host[0] && !out->has_mac && !out->nickname[0] && !out->target_hint[0])
            return -1;
        return 0;
    }

    int rc = parse_inline_target(copy, out);
    free(copy);
    return rc;
}

static int load_target_from_content(const char *content, target_spec *out, char *err, size_t errlen, const char **err_code) {
    if (!content || !*content) {
        *err_code = "content_missing";
        snprintf(err, errlen, "Missing content target");
        return -1;
    }
    size_t flen = 0;
    char *text = read_file_capped(content, &flen);
    if (text) {
        int rc = parse_target_text(text, flen, out);
        free(text);
        if (rc != 0) {
            *err_code = "content_invalid";
            snprintf(err, errlen, "Invalid target file '%s'", content);
            return -1;
        }
        return 0;
    }
    size_t clen = strlen(content);
    if ((clen > 7 && xstrcasecmp(content + clen - 7, ".chiaki") == 0) ||
        (clen > 9 && xstrcasecmp(content + clen - 9, ".psremote") == 0)) {
        *err_code = "content_missing";
        snprintf(err, errlen, "Target file not found: '%s'", content);
        return -1;
    }
    memset(out, 0, sizeof *out);
    out->wakeup = 1;
    if (parse_inline_target(content, out) != 0) {
        *err_code = "content_invalid";
        snprintf(err, errlen, "Could not parse target '%s'", content);
        return -1;
    }
    return 0;
}

/* ---- UDP DDP Discovery & Wakeup ---- */

typedef struct ddp_result {
    int ready;
    int standby;
    int is_ps5;
    char ip[64];
    char mac_hex[13];
    int has_mac;
    char host_name[256];
} ddp_result;

static int parse_ddp_packet(const char *src_ip, char *pkt, ddp_result *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->ip, sizeof out->ip, "%s", src_ip);
    char *line = pkt;
    char *nl = strchr(line, '\n');
    if (!nl) return -1;
    *nl++ = '\0';

    int code = 0;
    if (sscanf(line, "HTTP/%*s %d", &code) != 1) return -1;
    if (code == 200) out->ready = 1;
    else if (code == 620) out->standby = 1;

    while (nl && *nl) {
        char *next = strchr(nl, '\n');
        if (next) *next++ = '\0';
        size_t l = strlen(nl);
        if (l > 0 && nl[l - 1] == '\r') nl[l - 1] = '\0';
        char *colon = strchr(nl, ':');
        if (colon) {
            *colon = '\0';
            char *k = nl;
            char *v = colon + 1;
            trim_inplace(k);
            trim_inplace(v);
            if (xstrcasecmp(k, "host-id") == 0) {
                if (normalize_mac(v, out->mac_hex)) out->has_mac = 1;
            } else if (xstrcasecmp(k, "host-name") == 0) {
                snprintf(out->host_name, sizeof out->host_name, "%s", v);
            } else if (xstrcasecmp(k, "device-discovery-protocol-version") == 0) {
                if (strcmp(v, "00030010") == 0) out->is_ps5 = 1;
            }
        }
        nl = next;
    }
    return 0;
}

static int ddp_matches(const ddp_result *r, const target_spec *spec, const reg_host *reg) {
    if (spec->has_mac && r->has_mac && strcmp(spec->mac_hex, r->mac_hex) == 0) return 1;
    if (reg && reg->has_mac && r->has_mac && strcmp(reg->mac_hex, r->mac_hex) == 0) return 1;
    if (spec->host[0] && strcmp(spec->host, r->ip) == 0) return 1;
    if (spec->nickname[0] && r->host_name[0] && xstrcasecmp(spec->nickname, r->host_name) == 0) return 1;
    if (reg && reg->nickname[0] && r->host_name[0] && xstrcasecmp(reg->nickname, r->host_name) == 0) return 1;
    return 0;
}

#ifdef _WIN32
typedef SOCKET udp_sock_t;
#define UDP_INVALID INVALID_SOCKET
static void udp_close(udp_sock_t s) { if (s != INVALID_SOCKET) closesocket(s); }
#else
typedef int udp_sock_t;
#define UDP_INVALID (-1)
static void udp_close(udp_sock_t s) { if (s >= 0) close(s); }
#endif

static void udp_send_str(udp_sock_t s, const char *ip, uint16_t port, const char *msg) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        if (getaddrinfo(ip, NULL, &hints, &res) == 0 && res) {
            addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        } else {
            return;
        }
    }
    sendto(s, msg, (int)strlen(msg), 0, (const struct sockaddr *)&addr, sizeof addr);
}

static int ddp_discover_and_wake(
    ecp_conn *c, app_state *st, const target_spec *spec, const reg_host *reg,
    char *out_ip, size_t out_ip_cap, char *err, size_t errlen, const char **err_code) {
#ifdef _WIN32
    WSADATA wsa;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    udp_sock_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == UDP_INVALID) {
        if (spec->host[0]) {
            snprintf(out_ip, out_ip_cap, "%s", spec->host);
            return 0;
        }
        *err_code = "internal";
        snprintf(err, errlen, "Failed to create UDP discovery socket");
        return -1;
    }

    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&bcast, sizeof bcast);

    const char *srch_ps5 = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";
    const char *srch_ps4 = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00020020\n";

    ddp_result best;
    memset(&best, 0, sizeof best);
    int found = 0;

    double start_ms = ecp_time_ms();
    double discover_ms = spec->host[0] ? 1400.0 : 3200.0;
    double next_srch_ms = 0.0;

    while (!st->stop_requested && (ecp_time_ms() - start_ms) < discover_ms) {
        if (c) {
            if (ecp_poll(c, 0) < 0) {
                st->should_exit = 1;
                udp_close(sock);
                *err_code = "internal";
                snprintf(err, errlen, "Channel closed");
                return -1;
            }
            if (st->stop_requested) {
                udp_close(sock);
                *err_code = "internal";
                snprintf(err, errlen, "Launch canceled");
                return -1;
            }
        }
        double now = ecp_time_ms();
        if (now >= next_srch_ms) {
            udp_send_str(sock, "255.255.255.255", 9302, srch_ps5);
            udp_send_str(sock, "255.255.255.255", 987, srch_ps4);
            if (spec->host[0]) {
                udp_send_str(sock, spec->host, 9302, srch_ps5);
                udp_send_str(sock, spec->host, 987, srch_ps4);
            }
            next_srch_ms = now + 500.0;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = { 0, 100000 };
#ifdef _WIN32
        int sel = select(0, &rfds, NULL, NULL, &tv);
#else
        int sel = select(sock + 1, &rfds, NULL, NULL, &tv);
#endif
        if (sel > 0 && FD_ISSET(sock, &rfds)) {
            char buf[2048];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof from;
            int got = (int)recvfrom(sock, buf, (int)sizeof buf - 1, 0, (struct sockaddr *)&from, &fromlen);
            if (got > 0) {
                buf[got] = '\0';
                char ipbuf[64] = {0};
                inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof ipbuf);
                ddp_result r;
                if (parse_ddp_packet(ipbuf, buf, &r) == 0 && ddp_matches(&r, spec, reg)) {
                    best = r;
                    found = 1;
                    if (best.ready) break;
                }
            }
        }
    }

    if (found && best.standby && spec->wakeup) {
        if (!reg || !reg->has_regist_key) {
            udp_close(sock);
            *err_code = "keys_missing";
            snprintf(err, errlen, "PlayStation is in Rest Mode and registered credentials are missing in Chiaki settings");
            return -1;
        }
        unsigned long long cred = strtoull(reg->regist_key_hex, NULL, 16);
        int use_ps5 = reg->is_ps5 || best.is_ps5 || (xstrcasecmp(spec->target_hint, "ps5") == 0);
        const char *ver = use_ps5 ? "00030010" : "00020020";
        uint16_t port = use_ps5 ? 9302 : 987;
        char wake_pkt[512];
        snprintf(wake_pkt, sizeof wake_pkt,
                 "WAKEUP * HTTP/1.1\nclient-type:vr\nauth-type:R\nmodel:w\napp-type:r\n"
                 "user-credential:%llu\ndevice-discovery-protocol-version:%s\n",
                 cred, ver);

        const char *wake_ip = best.ip[0] ? best.ip : (spec->host[0] ? spec->host : "255.255.255.255");
        double wake_start = ecp_time_ms();
        double next_wake = 0.0;
        double next_srch = 0.0;
        if (c) ecp_send_loading(c, 0.25, "Waking PlayStation...");

        while (!st->stop_requested && (ecp_time_ms() - wake_start) < 25000.0) {
            if (c) {
                if (ecp_poll(c, 0) < 0) {
                    st->should_exit = 1;
                    udp_close(sock);
                    *err_code = "internal";
                    snprintf(err, errlen, "Channel closed");
                    return -1;
                }
                if (st->stop_requested) {
                    udp_close(sock);
                    *err_code = "internal";
                    snprintf(err, errlen, "Launch canceled");
                    return -1;
                }
            }
            double now = ecp_time_ms();
            if (now >= next_wake) {
                udp_send_str(sock, wake_ip, port, wake_pkt);
                if (strcmp(wake_ip, "255.255.255.255") != 0)
                    udp_send_str(sock, "255.255.255.255", port, wake_pkt);
                next_wake = now + 2000.0;
            }
            if (now >= next_srch) {
                udp_send_str(sock, wake_ip, 9302, srch_ps5);
                udp_send_str(sock, wake_ip, 987, srch_ps4);
                udp_send_str(sock, "255.255.255.255", 9302, srch_ps5);
                udp_send_str(sock, "255.255.255.255", 987, srch_ps4);
                double p = 0.25 + 0.65 * ((now - wake_start) / 25000.0);
                if (c) ecp_send_loading(c, p > 0.9 ? 0.9 : p, "Waking PlayStation...");
                next_srch = now + 600.0;
            }

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            struct timeval tv = { 0, 100000 };
#ifdef _WIN32
            int sel = select(0, &rfds, NULL, NULL, &tv);
#else
            int sel = select(sock + 1, &rfds, NULL, NULL, &tv);
#endif
            if (sel > 0 && FD_ISSET(sock, &rfds)) {
                char buf[2048];
                struct sockaddr_in from;
                socklen_t fromlen = sizeof from;
                int got = (int)recvfrom(sock, buf, (int)sizeof buf - 1, 0, (struct sockaddr *)&from, &fromlen);
                if (got > 0) {
                    buf[got] = '\0';
                    char ipbuf[64] = {0};
                    inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof ipbuf);
                    ddp_result r;
                    if (parse_ddp_packet(ipbuf, buf, &r) == 0 && ddp_matches(&r, spec, reg)) {
                        best = r;
                        if (best.ready) break;
                    }
                }
            }
        }
    }

    udp_close(sock);

    if (found && best.ready && best.ip[0]) {
        snprintf(out_ip, out_ip_cap, "%s", best.ip);
        return 0;
    }
    if (found && best.standby && !best.ready) {
        *err_code = "internal";
        snprintf(err, errlen, "PlayStation is in Rest Mode and did not wake up");
        return -1;
    }
    if (spec->host[0]) {
        snprintf(out_ip, out_ip_cap, "%.63s", spec->host);
        return 0;
    }

    *err_code = "content_missing";
    snprintf(err, errlen, "PlayStation did not respond on the local network");
    return -1;
}

/* ---- Child Process Supervision ---- */

static void kill_child(app_state *st) {
#ifdef _WIN32
    if (st->child_job) {
        TerminateJobObject(st->child_job, 0);
        CloseHandle(st->child_job);
        st->child_job = NULL;
    }
    if (st->child_process) {
        TerminateProcess(st->child_process, 0);
        WaitForSingleObject(st->child_process, 2000);
        CloseHandle(st->child_process);
        st->child_process = NULL;
    }
#else
    if (st->child_pid > 0) {
        kill(st->child_pid, SIGTERM);
        int waited = 0;
        int status = 0;
        while (waited < 2000) {
            pid_t w = waitpid(st->child_pid, &status, WNOHANG);
            if (w == st->child_pid || w < 0) {
                st->child_pid = 0;
                return;
            }
            sleep_ms(50);
            waited += 50;
        }
        kill(st->child_pid, SIGKILL);
        waitpid(st->child_pid, &status, 0);
        st->child_pid = 0;
    }
#endif
}

static int poll_child_exited(app_state *st, int *out_rc) {
#ifdef _WIN32
    if (!st->child_process) return 0;
    if (WaitForSingleObject(st->child_process, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(st->child_process, &code);
        CloseHandle(st->child_process);
        st->child_process = NULL;
        if (st->child_job) {
            CloseHandle(st->child_job);
            st->child_job = NULL;
        }
        *out_rc = (int)code;
        return 1;
    }
    return 0;
#else
    if (st->child_pid <= 0) return 0;
    int status = 0;
    pid_t w = waitpid(st->child_pid, &status, WNOHANG);
    if (w == st->child_pid) {
        st->child_pid = 0;
        if (WIFEXITED(status)) *out_rc = WEXITSTATUS(status);
        else *out_rc = 1;
        return 1;
    }
    return 0;
#endif
}

static int wait_child_blocking(app_state *st) {
    int rc = 0;
#ifdef _WIN32
    if (!st->child_process) return 0;
    WaitForSingleObject(st->child_process, INFINITE);
    poll_child_exited(st, &rc);
#else
    if (st->child_pid <= 0) return 0;
    int status = 0;
    while (waitpid(st->child_pid, &status, 0) < 0 && errno == EINTR) {}
    st->child_pid = 0;
    rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
    return rc;
}

#ifdef _WIN32
static void append_win_arg(char *dst, size_t cap, const char *arg) {
    size_t len = strlen(dst);
    if (len > 0 && len + 1 < cap) {
        dst[len++] = ' ';
        dst[len] = '\0';
    }
    int need_quote = (!*arg || strchr(arg, ' ') || strchr(arg, '\t') || strchr(arg, '"'));
    if (!need_quote) {
        snprintf(dst + len, cap - len, "%s", arg);
        return;
    }
    if (len + 1 < cap) dst[len++] = '"';
    for (const char *p = arg; *p && len + 4 < cap; p++) {
        if (*p == '"') {
            dst[len++] = '\\';
            dst[len++] = '"';
        } else {
            dst[len++] = *p;
        }
    }
    if (len + 1 < cap) dst[len++] = '"';
    dst[len] = '\0';
}
#endif

static int spawn_chiaki_stream(
    app_state *st, const char *chiaki_bin, const char *nickname, const char *ip,
    int fullscreen, const char *passcode, char *err, size_t errlen) {
    const char *args[16];
    int argc = 0;
    args[argc++] = chiaki_bin;
    args[argc++] = "stream";
    args[argc++] = nickname;
    args[argc++] = ip;
    args[argc++] = "--exit-app-on-stream-exit";
    if (fullscreen) {
        if (strcmp(st->settings.video_display_mode, "zoom") == 0) args[argc++] = "--zoom";
        else if (strcmp(st->settings.video_display_mode, "stretch") == 0) args[argc++] = "--stretch";
        else args[argc++] = "--fullscreen";
    }
    if (st->settings.input_dualsense) args[argc++] = "--dualsense";
    if (st->settings.video_opengl) args[argc++] = "--opengl";
    if (passcode && *passcode) {
        args[argc++] = "--passcode";
        args[argc++] = passcode;
    }
    args[argc] = NULL;

#ifdef _WIN32
    char cmdline[4096];
    cmdline[0] = '\0';
    for (int i = 0; i < argc; i++) append_win_arg(cmdline, sizeof cmdline, args[i]);

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE hNul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hNul != INVALID_HANDLE_VALUE ? hNul : NULL;
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput = (hErr && hErr != INVALID_HANDLE_VALUE) ? hErr : hNul;
    si.hStdError = si.hStdOutput;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessA(chiaki_bin, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    if (!ok) {
        snprintf(err, errlen, "CreateProcess failed (%lu)", (unsigned long)GetLastError());
        return -1;
    }

    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof jeli);
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof jeli);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    CloseHandle(pi.hThread);
    st->child_process = pi.hProcess;
    st->child_job = job;
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errlen, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        dup2(STDERR_FILENO, STDOUT_FILENO);
        setenv("APPIMAGE_EXTRACT_AND_RUN", "1", 1);
        execv(chiaki_bin, (char *const *)args);
        _exit(127);
    }
    st->child_pid = pid;
    return 0;
#endif
}

/* ---- Resolve + Wake + Launch ---- */

static int resolve_host_and_wake(
    ecp_conn *c, app_state *st, const char *content, const char *chiaki_bin,
    char *out_nickname, size_t nick_cap, char *out_ip, size_t ip_cap,
    char *out_passcode, size_t pass_cap, char *err, size_t errlen, const char **err_code) {
    target_spec spec;
    if (load_target_from_content(content, &spec, err, errlen, err_code) != 0)
        return -1;

    host_list list;
    load_registered_hosts(&list, chiaki_bin);
    if (list.count == 0) {
        *err_code = "keys_missing";
        snprintf(err, errlen, "No registered PlayStation consoles found in Chiaki-NG. Open Chiaki-NG to register your console first.");
        return -1;
    }

    const reg_host *matched = NULL;
    for (int i = 0; i < list.count; i++) {
        if (spec.has_mac && list.items[i].has_mac && strcmp(spec.mac_hex, list.items[i].mac_hex) == 0) {
            matched = &list.items[i];
            break;
        }
    }
    if (!matched && spec.nickname[0]) {
        for (int i = 0; i < list.count; i++) {
            if (xstrcasecmp(spec.nickname, list.items[i].nickname) == 0) {
                matched = &list.items[i];
                break;
            }
        }
    }
    if (!matched && spec.target_hint[0]) {
        int want_ps5 = (xstrcasecmp(spec.target_hint, "ps5") == 0);
        int want_ps4 = (xstrcasecmp(spec.target_hint, "ps4") == 0);
        for (int i = 0; i < list.count; i++) {
            if ((want_ps5 && list.items[i].is_ps5) || (want_ps4 && !list.items[i].is_ps5)) {
                matched = &list.items[i];
                break;
            }
        }
    }
    if (!matched && !spec.has_mac && !spec.nickname[0] && list.count == 1) {
        matched = &list.items[0];
    }

    if (!matched) {
        *err_code = "keys_missing";
        snprintf(err, errlen, "Console '%.120s' is not registered in Chiaki-NG",
                 spec.nickname[0] ? spec.nickname : (spec.has_mac ? spec.mac_hex : "PlayStation"));
        return -1;
    }

    if (c) ecp_send_loading(c, 0.15, "Discovering PlayStation...");
    if (ddp_discover_and_wake(c, st, &spec, matched, out_ip, ip_cap, err, errlen, err_code) != 0)
        return -1;

    snprintf(out_nickname, nick_cap, "%s", matched->nickname);
    snprintf(out_passcode, pass_cap, "%s", spec.passcode);
    return 0;
}

/* ---- ECP Handlers ---- */

static void h_launch(void *user, ecp_conn *c, const ecp_request *req) {
    app_state *st = (app_state *)user;
    st->stop_requested = 0;

    const ecp_json *jsettings = ecp_json_get(req->msg, "settings");
    if (ecp_json_type_of(jsettings) == ECP_JSON_OBJECT) {
        size_t n = ecp_json_len(jsettings);
        for (size_t i = 0; i < n; i++) {
            char serr[128];
            if (apply_one_setting(&st->settings, ecp_json_key_at(jsettings, i), ecp_json_at(jsettings, i), serr, sizeof serr) != 0) {
                ecp_reply_error(c, req->id, "protocol", serr, NULL);
                return;
            }
        }
        settings_save(st);
    }

    const char *content = ecp_json_str(ecp_json_get(req->msg, "content"), NULL);
    if (!content || !*content) {
        ecp_reply_error(c, req->id, "content_missing", "Missing content in launch", NULL);
        return;
    }

    if (st->dry_run) {
        st->running = 1;
        ecp_reply_ok(c, req->id);
        ecp_send_first_frame(c);
        ecp_send_running(c);
        return;
    }

    char chiaki_bin[1024];
    if (find_chiaki_binary(chiaki_bin, sizeof chiaki_bin) != 0) {
        ecp_reply_error(c, req->id, "internal", "Sibling chiaki-ng executable not found", NULL);
        return;
    }

    char nick[256] = {0}, ip[256] = {0}, pass[32] = {0}, err[256] = {0};
    const char *err_code = "internal";
    if (resolve_host_and_wake(c, st, content, chiaki_bin, nick, sizeof nick, ip, sizeof ip,
                              pass, sizeof pass, err, sizeof err, &err_code) != 0) {
        ecp_reply_error(c, req->id, err_code, err, NULL);
        return;
    }

    sync_chiaki_qt_settings(&st->settings);

    int fullscreen = ecp_json_bool(ecp_json_get(req->msg, "fullscreen"), 1);
    if (c) ecp_send_loading(c, 0.95, "Starting Chiaki-NG...");
    if (spawn_chiaki_stream(st, chiaki_bin, nick, ip, fullscreen, pass, err, sizeof err) != 0) {
        ecp_reply_error(c, req->id, "internal", err, NULL);
        return;
    }

    st->running = 1;
    ecp_reply_ok(c, req->id);
    ecp_send_first_frame(c);
    ecp_send_running(c);
}

static void h_stop(void *user, ecp_conn *c, const ecp_request *req) {
    app_state *st = (app_state *)user;
    st->stop_requested = 1;
    st->running = 0;
    kill_child(st);
    ecp_reply_ok(c, req->id);
    ecp_send_stopped(c, "frontend", NULL, NULL, NULL);
    st->should_exit = 1;
}

static void h_get_schema(void *user, ecp_conn *c, const ecp_request *req) {
    (void)user;
    ecp_writer w;
    ecp_msg_result(&w, req->id);
    ecp_w_key(&w, "settings");
    write_schema_settings_array(&w);
    ecp_send(c, &w);
}

static void h_get_settings(void *user, ecp_conn *c, const ecp_request *req) {
    app_state *st = (app_state *)user;
    ecp_writer w;
    ecp_msg_result(&w, req->id);
    ecp_w_key(&w, "values");
    write_settings_values(&w, &st->settings);
    ecp_send(c, &w);
}

static void h_set_setting(void *user, ecp_conn *c, const ecp_request *req) {
    app_state *st = (app_state *)user;
    const char *key = ecp_json_str(ecp_json_get(req->msg, "key"), NULL);
    const ecp_json *val = ecp_json_get(req->msg, "value");
    char err[128];
    if (apply_one_setting(&st->settings, key, val, err, sizeof err) != 0) {
        ecp_reply_error(c, req->id, "protocol", err, NULL);
        return;
    }
    settings_save(st);
    ecp_writer w;
    ecp_msg_result(&w, req->id);
    ecp_w_kstring(&w, "applied", "restart");
    ecp_send(c, &w);
}

static void h_closed(void *user, ecp_conn *c) {
    (void)c;
    app_state *st = (app_state *)user;
    st->stop_requested = 1;
    st->should_exit = 1;
    kill_child(st);
}

/* ---- Oneshot Handlers ---- */

static int run_oneshot(app_state *st, const char *cmd) {
    if (strcmp(cmd, "describe") == 0) {
        return ecp_print_descriptor(&DESC);
    }
    if (strcmp(cmd, "schema") == 0) {
        ecp_writer w = {0};
        ecp_w_object_begin(&w);
        ecp_w_key(&w, "settings");
        write_schema_settings_array(&w);
        return ecp_oneshot_print(&w);
    }
    if (strcmp(cmd, "status") == 0) {
        ecp_writer w = {0};
        ecp_w_object_begin(&w);
        if (st->dry_run) {
            ecp_w_kbool(&w, "ready", 1);
            return ecp_oneshot_print(&w);
        }
        char chiaki_bin[1024];
        int has_bin = (find_chiaki_binary(chiaki_bin, sizeof chiaki_bin) == 0);
        host_list list;
        if (has_bin) load_registered_hosts(&list, chiaki_bin);
        else memset(&list, 0, sizeof list);

        int ready = (has_bin && list.count > 0) ? 1 : 0;
        ecp_w_kbool(&w, "ready", ready);
        if (!ready) {
            ecp_w_key(&w, "problems");
            ecp_w_array_begin(&w);
            if (!has_bin) {
                ecp_w_object_begin(&w);
                ecp_w_kstring(&w, "code", "internal");
                ecp_w_kstring(&w, "message", "Chiaki-NG executable not found beside chiaki-ecp");
                ecp_w_object_end(&w);
            } else {
                ecp_w_object_begin(&w);
                ecp_w_kstring(&w, "code", "keys_missing");
                ecp_w_kstring(&w, "message", "No PlayStation console registered in Chiaki-NG yet");
                ecp_w_object_end(&w);
            }
            ecp_w_array_end(&w);
        }
        return ecp_oneshot_print(&w);
    }
    if (strcmp(cmd, "verify") == 0) {
        ecp_json *req = ecp_oneshot_read_request();
        const char *content = req ? ecp_json_str(ecp_json_get(req, "content"), NULL) : NULL;
        target_spec spec;
        char err[256] = {0};
        const char *err_code = "content_invalid";
        int ok = (load_target_from_content(content, &spec, err, sizeof err, &err_code) == 0);
        ecp_writer w = {0};
        ecp_w_object_begin(&w);
        ecp_w_kbool(&w, "ok", ok);
        if (ok) {
            const char *title = spec.nickname[0] ? spec.nickname
                                : (spec.has_mac ? spec.mac_hex : (spec.host[0] ? spec.host : "PlayStation"));
            ecp_w_kstring(&w, "title", title);
        } else {
            ecp_w_key(&w, "error");
            ecp_w_error(&w, err_code, err, NULL);
        }
        ecp_json_free(req);
        return ecp_oneshot_print(&w);
    }
    fprintf(stderr, "chiaki-ecp: unknown oneshot command '%s'\n", cmd);
    return 2;
}

/* ---- Entry Point ---- */

int main(int argc, char **argv) {
    app_state st;
    memset(&st, 0, sizeof st);
    settings_init_defaults(&st.settings);

    /* Strip --dry-run before ecp_config_parse */
    int out_argc = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            st.dry_run = 1;
        } else {
            argv[out_argc++] = argv[i];
        }
    }
    argv[out_argc] = NULL;
    argc = out_argc;

    ecp_config cfg;
    if (ecp_config_parse(&cfg, &argc, argv) != 0) {
        fprintf(stderr, "chiaki-ecp: %s\n", cfg.error);
        return 2;
    }

    if (!st.dry_run) settings_load(&st);

    if (cfg.describe) return ecp_print_descriptor(&DESC);
    if (cfg.oneshot[0]) return run_oneshot(&st, cfg.oneshot);

    if (cfg.transport == ECP_NONE) {
        if (argc < 2) {
            fprintf(stderr, "Usage: chiaki-ecp [--dry-run] [--ecp <stdio|tcp:host:port>] [--ecp-describe] [--ecp-oneshot <cmd>] [<target.chiaki>]\n");
            return 2;
        }
        char chiaki_bin[1024];
        if (find_chiaki_binary(chiaki_bin, sizeof chiaki_bin) != 0) {
            fprintf(stderr, "chiaki-ecp: sibling chiaki executable not found\n");
            return 1;
        }
        char nick[256] = {0}, ip[256] = {0}, pass[32] = {0}, err[256] = {0};
        const char *err_code = "internal";
        if (resolve_host_and_wake(NULL, &st, argv[1], chiaki_bin, nick, sizeof nick,
                                  ip, sizeof ip, pass, sizeof pass, err, sizeof err, &err_code) != 0) {
            fprintf(stderr, "chiaki-ecp: %s\n", err);
            return 1;
        }
        sync_chiaki_qt_settings(&st.settings);
        if (spawn_chiaki_stream(&st, chiaki_bin, nick, ip, 1, pass, err, sizeof err) != 0) {
            fprintf(stderr, "chiaki-ecp: %s\n", err);
            return 1;
        }
        return wait_child_blocking(&st);
    }

    ecp_handlers handlers;
    memset(&handlers, 0, sizeof handlers);
    handlers.launch = h_launch;
    handlers.stop = h_stop;
    handlers.get_schema = h_get_schema;
    handlers.get_settings = h_get_settings;
    handlers.set_setting = h_set_setting;
    handlers.closed = h_closed;

    char err[256];
    ecp_conn *c = ecp_open(&cfg, &DESC, &handlers, &st, err, sizeof err);
    if (!c) {
        fprintf(stderr, "chiaki-ecp: %s\n", err);
        return 1;
    }

    while (!st.should_exit) {
        if (ecp_poll(c, 50) < 0) {
            st.should_exit = 1;
            kill_child(&st);
            break;
        }
        if (st.running && !st.dry_run) {
            int rc = 0;
            if (poll_child_exited(&st, &rc)) {
                st.running = 0;
                if (rc == 0) {
                    ecp_send_stopped(c, "user", NULL, NULL, NULL);
                } else {
                    char emsg[128];
                    snprintf(emsg, sizeof emsg, "Chiaki-NG exited with code %d", rc);
                    ecp_send_stopped(c, "error", "internal", emsg, NULL);
                }
                st.should_exit = 1;
                break;
            }
        }
    }

    kill_child(&st);
    ecp_close(c);
    return 0;
}
