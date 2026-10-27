// fork()/execv()/sigprocmask() and friends are POSIX, not plain C11 -- see
// meson.build's c_std note for why this file must ask for them explicitly.
#define _GNU_SOURCE

#include "eq/eq_state.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Where the DacController checkout lives, relative to $HOME. Overridable via
// the BLACKPEARL_EQ_SCRIPT environment variable (an absolute path to
// blackpearl-eq.py) without rebuilding, in case the checkout ever moves.
#define DEFAULT_SCRIPT_REL "Code/github.com/StoicaTielemans/DacController/blackpearl-eq.py"

// The script has no third-party dependencies, so the system interpreter is
// enough -- no need to go through `uv run` (which also may not be on the
// PATH Hyprland launches the bar with). An absolute path rather than a PATH
// lookup: the exec happens in a forked child of a *multi-threaded* process
// (pulse/pipewire threads), where only async-signal-safe calls are allowed
// before exec, and execvp()'s PATH search isn't guaranteed to be.
#define PYTHON_PATH "/usr/bin/python3"

#define STATE_FILE_NAME "state.json"
#define STATE_FILE_MAX_BYTES (256 * 1024) // the real file is ~3 KB; anything huge is garbage
#define NAME_MAX_LEN 64

// How long a requested profile may stay "pending" before the bar gives up
// on it. A normal apply takes ~1 s (10 band writes + commit + read-back); the
// only way to exceed this is the script failing before writing any state
// (DAC unplugged, no permission), which it reports only on its own stderr.
#define PENDING_TIMEOUT_SEC 15

typedef struct {
    bool available;
    char profile[NAME_MAX_LEN];
    char gain[16];
    double preamp;
    bool enabled;
    int curve_count;
    double freqs[EQ_STATE_MAX_POINTS];
    double db[EQ_STATE_MAX_POINTS];
    double peak_db, min_db;
} EqSnapshot;

static EqSnapshot g_snap;

static int g_inotify_fd = -1;
static char g_state_dir[PATH_MAX];
// Room for g_state_dir plus "/state.json", so the compiler can prove the
// snprintf below never truncates (a 4 KB cache path isn't realistic anyway).
static char g_state_path[PATH_MAX + sizeof("/" STATE_FILE_NAME)];
static char g_script_path[PATH_MAX];

static char g_pending[NAME_MAX_LEN];
static time_t g_pending_since;

// $XDG_CACHE_HOME/blackpearl-eq, falling back to ~/.cache/blackpearl-eq --
// the exact same rule the script uses to decide where to write.
static void resolve_paths(void) {
    const char *home = getenv("HOME");
    const char *cache = getenv("XDG_CACHE_HOME");
    if (cache && cache[0]) {
        snprintf(g_state_dir, sizeof(g_state_dir), "%s/blackpearl-eq", cache);
    } else {
        snprintf(g_state_dir, sizeof(g_state_dir), "%s/.cache/blackpearl-eq", home ? home : "");
    }
    snprintf(g_state_path, sizeof(g_state_path), "%s/%s", g_state_dir, STATE_FILE_NAME);

    const char *script = getenv("BLACKPEARL_EQ_SCRIPT");
    if (script && script[0]) {
        snprintf(g_script_path, sizeof(g_script_path), "%s", script);
    } else {
        snprintf(g_script_path, sizeof(g_script_path), "%s/%s", home ? home : "", DEFAULT_SCRIPT_REL);
    }
}

// mkdir -p for the two levels that can be missing (~/.cache itself, then
// blackpearl-eq/) -- inotify can only watch a directory that exists, and
// watching the *directory* (not the file) is required anyway: the script
// replaces state.json via rename(), which would orphan a watch on the old
// file's inode after the first update.
static void ensure_state_dir(void) {
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", g_state_dir);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        mkdir(parent, 0755); // EEXIST is the normal case; any real failure surfaces below
    }
    if (mkdir(g_state_dir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "stickBar: eq: mkdir %s: %s\n", g_state_dir, strerror(errno));
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *buf = malloc(STATE_FILE_MAX_BYTES + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, STATE_FILE_MAX_BYTES, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void copy_string_field(cJSON *root, const char *key, char *out, size_t out_size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(out, out_size, "%s", item->valuestring);
    } else {
        out[0] = '\0'; // null (e.g. an EQ that matches no preset) or missing
    }
}

static double number_field(cJSON *root, const char *key, double fallback) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

// Parses state.json into a fresh snapshot and swaps it in only if parsing
// succeeded -- a malformed or half-written file (shouldn't happen given the
// script's temp-file-then-rename write, but cheap to guard) leaves the
// previous snapshot on screen rather than blanking the module.
static bool load_snapshot(void) {
    char *text = read_file(g_state_path);
    if (!text) {
        return false;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        return false;
    }

    EqSnapshot snap = { .available = true, .enabled = true };
    copy_string_field(root, "profile", snap.profile, sizeof(snap.profile));
    copy_string_field(root, "gain", snap.gain, sizeof(snap.gain));
    snap.preamp = number_field(root, "preamp", 0.0);
    snap.peak_db = number_field(root, "peak_db", 0.0);
    snap.min_db = number_field(root, "min_db", 0.0);
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsBool(enabled)) {
        snap.enabled = cJSON_IsTrue(enabled);
    }

    cJSON *curve = cJSON_GetObjectItemCaseSensitive(root, "curve");
    cJSON *freqs = cJSON_GetObjectItemCaseSensitive(curve, "freqs");
    cJSON *db = cJSON_GetObjectItemCaseSensitive(curve, "db");
    if (cJSON_IsArray(freqs) && cJSON_IsArray(db)) {
        int n = cJSON_GetArraySize(freqs);
        if (cJSON_GetArraySize(db) < n) {
            n = cJSON_GetArraySize(db);
        }
        if (n > EQ_STATE_MAX_POINTS) {
            n = EQ_STATE_MAX_POINTS;
        }
        for (int i = 0; i < n; i++) {
            snap.freqs[i] = cJSON_GetArrayItem(freqs, i)->valuedouble;
            snap.db[i] = cJSON_GetArrayItem(db, i)->valuedouble;
        }
        snap.curve_count = n;
    }
    cJSON_Delete(root);

    g_snap = snap;
    // Any state write means the script finished (or something else changed
    // the EQ) -- either way, nothing is pending from the bar's point of view.
    g_pending[0] = '\0';
    return true;
}

bool eq_state_init(void) {
    resolve_paths();
    ensure_state_dir();
    load_snapshot(); // fine if it fails: the script may simply never have run yet

    g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotify_fd < 0) {
        perror("stickBar: eq: inotify_init1");
        return false;
    }
    // IN_MOVED_TO is the one that actually fires for the script's own
    // rename()-based writes; IN_CLOSE_WRITE additionally covers anything
    // that rewrites the file in place (e.g. editing it by hand to test).
    if (inotify_add_watch(g_inotify_fd, g_state_dir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        fprintf(stderr, "stickBar: eq: inotify_add_watch %s: %s\n", g_state_dir, strerror(errno));
        close(g_inotify_fd);
        g_inotify_fd = -1;
        return false;
    }
    return true;
}

int eq_state_watch_fd(void) {
    return g_inotify_fd;
}

bool eq_state_pump(void) {
    if (g_inotify_fd < 0) {
        return false;
    }
    // Aligned as the inotify(7) man page requires for reading struct
    // inotify_event records straight out of the buffer.
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool touched = false;
    for (;;) {
        ssize_t len = read(g_inotify_fd, buf, sizeof(buf));
        if (len <= 0) {
            break; // EAGAIN once drained (non-blocking fd)
        }
        for (char *p = buf; p < buf + len;) {
            struct inotify_event *ev = (struct inotify_event *)p;
            // The directory also sees the script's own state.json.tmp being
            // written -- only the final file's arrival matters.
            if (ev->len > 0 && strcmp(ev->name, STATE_FILE_NAME) == 0) {
                touched = true;
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return touched && load_snapshot();
}

void eq_state_shutdown(void) {
    if (g_inotify_fd >= 0) {
        close(g_inotify_fd);
        g_inotify_fd = -1;
    }
}

bool eq_state_available(void) { return g_snap.available; }
const char *eq_state_profile(void) { return g_snap.profile; }
const char *eq_state_gain(void) { return g_snap.gain; }
double eq_state_preamp(void) { return g_snap.preamp; }
bool eq_state_enabled(void) { return g_snap.enabled; }
int eq_state_curve_count(void) { return g_snap.curve_count; }
double eq_state_peak_db(void) { return g_snap.peak_db; }
double eq_state_min_db(void) { return g_snap.min_db; }

double eq_state_curve_freq(int i) {
    return (i >= 0 && i < g_snap.curve_count) ? g_snap.freqs[i] : 0.0;
}

double eq_state_curve_db(int i) {
    return (i >= 0 && i < g_snap.curve_count) ? g_snap.db[i] : 0.0;
}

void eq_state_apply_profile(const char *name) {
    if (!name || !name[0]) {
        return;
    }
    // Everything the child needs is prepared here, in the parent: between
    // fork() and exec in a multi-threaded process, only async-signal-safe
    // calls are allowed (no malloc, no snprintf, no stdio).
    char name_copy[NAME_MAX_LEN];
    snprintf(name_copy, sizeof(name_copy), "%s", name);
    char *argv[] = { PYTHON_PATH, g_script_path, "profile", name_copy, NULL };
    sigset_t empty;
    sigemptyset(&empty);

    // Double fork: the intermediate child exits immediately and is reaped
    // right here, so the grandchild that actually runs the script is
    // reparented to init and never becomes a zombie of the bar -- without
    // installing a global SIGCHLD handler, which would interfere with
    // sysmon_client.c's own popen()/pclose() of nvidia-smi.
    pid_t pid = fork();
    if (pid < 0) {
        perror("stickBar: eq: fork");
        return;
    }
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
            }
            // main.c blocks SIGUSR1 for its signalfd, and a blocked mask
            // survives exec -- reset it so the script starts with a clean one.
            sigprocmask(SIG_SETMASK, &empty, NULL);
            execv(PYTHON_PATH, argv);
            _exit(127);
        }
        _exit(grandchild < 0 ? 1 : 0);
    }
    waitpid(pid, NULL, 0); // the intermediate child exits right away

    snprintf(g_pending, sizeof(g_pending), "%s", name);
    g_pending_since = time(NULL);
}

const char *eq_state_pending_profile(void) {
    if (g_pending[0] && time(NULL) - g_pending_since > PENDING_TIMEOUT_SEC) {
        g_pending[0] = '\0'; // evidently failed; stop showing "applying"
    }
    return g_pending;
}
