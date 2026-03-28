/*
 * cqjs - main program
 *
 * Pure C equivalent of goqjs's cmd/goqjs/main.go.
 * Provides stdin/stdout JSON Lines interaction with QuickJS-ng engine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>

#include <execinfo.h>

#include "cqjs.h"
#include "polyfill/polyfill.h"
#include "quickjs/quickjs.h"

/* Crash signal handler for debugging (uses sigaltstack for stack overflow) */
static void crash_signal_handler(int sig) {
    void *array[30];
    int size = backtrace(array, 30);
    fprintf(stderr, "\n=== CRASH: signal %d ===\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    _exit(128 + sig);
}

static void install_crash_handlers(void) {
    /* Set up alternate signal stack so we can catch stack overflows */
    static char alt_stack[SIGSTKSZ];
    stack_t ss = { .ss_sp = alt_stack, .ss_size = SIGSTKSZ, .ss_flags = 0 };
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_signal_handler;
    sa.sa_flags = SA_ONSTACK;  /* use alternate stack */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

/* ============================================================
 * Global state
 * ============================================================ */

static JSRuntime *g_rt = NULL;
static JSContext *g_ctx = NULL;
static dispatch_tracker_t g_dt;
static polyfill_managers_t *g_pm = NULL;

/* Pointers to timer/fetch managers owned by polyfill layer */
static timer_manager_t *g_tm = NULL;
static fetch_manager_t *g_fm = NULL;

/* stdout mutex for thread-safe JSON output */
static pthread_mutex_t g_stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

/* stdin reader thread state */
typedef struct {
    char **lines;
    int count;
    int capacity;
    int notify_pipe[2];
    pthread_mutex_t mutex;
    int eof;
} stdin_reader_t;

static stdin_reader_t g_stdin;

/* ============================================================
 * Utility implementations
 * ============================================================ */

void cqjs_send_response(const char *json_line) {
    pthread_mutex_lock(&g_stdout_mutex);
    fprintf(stdout, "%s\n", json_line);
    fflush(stdout);
    pthread_mutex_unlock(&g_stdout_mutex);
}

char *cqjs_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read_len = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[read_len] = '\0';
    if (out_len) *out_len = read_len;
    return buf;
}

int64_t cqjs_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void cqjs_timespec_now(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

void cqjs_timespec_add_ms(struct timespec *ts, int ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

int cqjs_timespec_cmp(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec < b->tv_sec) return -1;
    if (a->tv_sec > b->tv_sec) return 1;
    if (a->tv_nsec < b->tv_nsec) return -1;
    if (a->tv_nsec > b->tv_nsec) return 1;
    return 0;
}

/* ============================================================
 * dispatch_tracker implementation
 * ============================================================ */

void dt_init(dispatch_tracker_t *dt) {
    memset(dt, 0, sizeof(*dt));
    pthread_mutex_init(&dt->mutex, NULL);
    dt->capacity = 16;
    dt->entries = calloc((size_t)dt->capacity, sizeof(dispatch_entry_t *));
}

char *dt_new_request(dispatch_tracker_t *dt) {
    pthread_mutex_lock(&dt->mutex);
    int64_t id = ++dt->counter;

    /* Grow if needed */
    if (dt->count >= dt->capacity) {
        dt->capacity *= 2;
        dt->entries = realloc(dt->entries, (size_t)dt->capacity * sizeof(dispatch_entry_t *));
    }

    dispatch_entry_t *entry = calloc(1, sizeof(dispatch_entry_t));
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "d_%lld", (long long)id);
    entry->id = strdup(id_str);
    entry->completed = 0;
    pthread_mutex_init(&entry->mutex, NULL);
    pthread_cond_init(&entry->cond, NULL);

    dt->entries[dt->count++] = entry;
    pthread_mutex_unlock(&dt->mutex);

    return strdup(id_str);
}

dispatch_entry_t *dt_find(dispatch_tracker_t *dt, const char *id) {
    for (int i = 0; i < dt->count; i++) {
        if (dt->entries[i] && strcmp(dt->entries[i]->id, id) == 0)
            return dt->entries[i];
    }
    return NULL;
}

void dt_resolve(dispatch_tracker_t *dt, const char *id, const char *value) {
    pthread_mutex_lock(&dt->mutex);
    dispatch_entry_t *entry = dt_find(dt, id);
    if (entry) {
        pthread_mutex_lock(&entry->mutex);
        entry->value = value ? strdup(value) : NULL;
        entry->completed = 1;
        pthread_cond_signal(&entry->cond);
        pthread_mutex_unlock(&entry->mutex);
    }
    pthread_mutex_unlock(&dt->mutex);
}

void dt_reject(dispatch_tracker_t *dt, const char *id, const char *error) {
    pthread_mutex_lock(&dt->mutex);
    dispatch_entry_t *entry = dt_find(dt, id);
    if (entry) {
        pthread_mutex_lock(&entry->mutex);
        entry->error = error ? strdup(error) : strdup("unknown dispatch error");
        entry->completed = 1;
        pthread_cond_signal(&entry->cond);
        pthread_mutex_unlock(&entry->mutex);
    }
    pthread_mutex_unlock(&dt->mutex);
}

void dt_remove(dispatch_tracker_t *dt, const char *id) {
    pthread_mutex_lock(&dt->mutex);
    for (int i = 0; i < dt->count; i++) {
        if (dt->entries[i] && strcmp(dt->entries[i]->id, id) == 0) {
            dispatch_entry_t *e = dt->entries[i];
            free(e->id);
            free(e->value);
            free(e->error);
            pthread_mutex_destroy(&e->mutex);
            pthread_cond_destroy(&e->cond);
            free(e);
            /* Shift remaining entries */
            for (int j = i; j < dt->count - 1; j++)
                dt->entries[j] = dt->entries[j + 1];
            dt->count--;
            break;
        }
    }
    pthread_mutex_unlock(&dt->mutex);
}

void dt_free(dispatch_tracker_t *dt) {
    pthread_mutex_lock(&dt->mutex);
    for (int i = 0; i < dt->count; i++) {
        if (dt->entries[i]) {
            free(dt->entries[i]->id);
            free(dt->entries[i]->value);
            free(dt->entries[i]->error);
            pthread_mutex_destroy(&dt->entries[i]->mutex);
            pthread_cond_destroy(&dt->entries[i]->cond);
            free(dt->entries[i]);
        }
    }
    free(dt->entries);
    dt->entries = NULL;
    dt->count = 0;
    dt->capacity = 0;
    pthread_mutex_unlock(&dt->mutex);
    pthread_mutex_destroy(&dt->mutex);
}

/* ============================================================
 * 64MB big-stack pthread for eval/call
 * ============================================================ */

#define BIG_STACK_SIZE (64 * 1024 * 1024)

typedef struct {
    JSContext *ctx;
    const char *input;
    size_t input_len;
    const char *filename;
    int eval_flags;
    JSValue result;
} eval_args_t;

typedef struct {
    JSContext *ctx;
    JSValueConst func_obj;
    JSValueConst this_obj;
    int argc;
    JSValueConst *argv;
    JSValue result;
} call_args_t;

static void *big_stack_eval_thread(void *arg) {
    eval_args_t *ea = (eval_args_t *)arg;
    /* Update stack top so QuickJS sees the new thread's stack */
    JS_UpdateStackTop(JS_GetRuntime(ea->ctx));
    ea->result = JS_Eval(ea->ctx, ea->input, ea->input_len,
                         ea->filename, ea->eval_flags);
    return NULL;
}

static void *big_stack_call_thread(void *arg) {
    call_args_t *ca = (call_args_t *)arg;
    /* Update stack top so QuickJS sees the new thread's stack */
    JS_UpdateStackTop(JS_GetRuntime(ca->ctx));
    ca->result = JS_Call(ca->ctx, ca->func_obj, ca->this_obj,
                         ca->argc, ca->argv);
    return NULL;
}

JSValue cqjs_eval(JSContext *ctx, const char *input, size_t input_len,
                  const char *filename, int eval_flags) {
    eval_args_t ea = {
        .ctx = ctx,
        .input = input,
        .input_len = input_len,
        .filename = filename,
        .eval_flags = eval_flags,
    };

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, BIG_STACK_SIZE);

    if (pthread_create(&tid, &attr, big_stack_eval_thread, &ea) != 0) {
        pthread_attr_destroy(&attr);
        /* Fallback: eval on current thread */
        return JS_Eval(ctx, input, input_len, filename, eval_flags);
    }
    pthread_attr_destroy(&attr);
    pthread_join(tid, NULL);
    /* Restore stack top to caller's thread stack after big-stack thread returns */
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    return ea.result;
}

JSValue cqjs_call(JSContext *ctx, JSValueConst func_obj,
                  JSValueConst this_obj, int argc, JSValueConst *argv) {
    call_args_t ca = {
        .ctx = ctx,
        .func_obj = func_obj,
        .this_obj = this_obj,
        .argc = argc,
        .argv = argv,
    };

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, BIG_STACK_SIZE);

    if (pthread_create(&tid, &attr, big_stack_call_thread, &ca) != 0) {
        pthread_attr_destroy(&attr);
        return JS_Call(ctx, func_obj, this_obj, argc, argv);
    }
    pthread_attr_destroy(&attr);
    pthread_join(tid, NULL);
    /* Restore stack top to caller's thread stack after big-stack thread returns */
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    return ca.result;
}

/* ============================================================
 * JSON response helpers (using QuickJS JSON.stringify)
 * ============================================================ */

/* Escape a C string to JSON string (with quotes). Caller must free. */
static char *json_escape_string(const char *str) {
    if (!str) return strdup("null");
    size_t len = strlen(str);
    /* Worst case: every char needs escaping (\uXXXX = 6 chars) + quotes */
    char *out = malloc(len * 6 + 3);
    if (!out) return strdup("\"\"");
    char *p = out;
    *p++ = '"';
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        switch (c) {
            case '"':  *p++ = '\\'; *p++ = '"'; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\b': *p++ = '\\'; *p++ = 'b'; break;
            case '\f': *p++ = '\\'; *p++ = 'f'; break;
            case '\n': *p++ = '\\'; *p++ = 'n'; break;
            case '\r': *p++ = '\\'; *p++ = 'r'; break;
            case '\t': *p++ = '\\'; *p++ = 't'; break;
            default:
                if (c < 0x20) {
                    p += sprintf(p, "\\u%04x", c);
                } else {
                    *p++ = (char)c;
                }
                break;
        }
    }
    *p++ = '"';
    *p = '\0';
    return out;
}

/* Build and send a JSON response.
 * value_is_raw: if 1, value is already a JSON string (no extra quoting) */
static void send_json_response(const char *id, const char *type,
                                const char *value, int value_is_raw,
                                const char *message,
                                const char *name, const char *data, int data_is_raw) {
    /* Build JSON manually for efficiency */
    size_t cap = 256;
    if (value) cap += strlen(value) * 2;
    if (message) cap += strlen(message) * 2;
    if (data) cap += strlen(data) * 2;
    char *buf = malloc(cap + 256);
    if (!buf) return;

    char *p = buf;
    p += sprintf(p, "{");

    if (id && id[0]) {
        char *eid = json_escape_string(id);
        p += sprintf(p, "\"id\":%s,", eid);
        free(eid);
    }

    char *etype = json_escape_string(type);
    p += sprintf(p, "\"type\":%s", etype);
    free(etype);

    if (value) {
        if (value_is_raw) {
            p += sprintf(p, ",\"value\":%s", value);
        } else {
            char *ev = json_escape_string(value);
            p += sprintf(p, ",\"value\":%s", ev);
            free(ev);
        }
    }

    if (message) {
        char *em = json_escape_string(message);
        p += sprintf(p, ",\"message\":%s", em);
        free(em);
    }

    if (name) {
        char *en = json_escape_string(name);
        p += sprintf(p, ",\"name\":%s", en);
        free(en);
    }

    if (data) {
        if (data_is_raw) {
            p += sprintf(p, ",\"data\":%s", data);
        } else {
            char *ed = json_escape_string(data);
            p += sprintf(p, ",\"data\":%s", ed);
            free(ed);
        }
    }

    p += sprintf(p, "}");
    cqjs_send_response(buf);
    free(buf);
}

/* ============================================================
 * stdin reader thread
 * ============================================================ */

static void stdin_reader_init(stdin_reader_t *sr) {
    memset(sr, 0, sizeof(*sr));
    sr->capacity = 16;
    sr->lines = calloc((size_t)sr->capacity, sizeof(char *));
    pipe(sr->notify_pipe);
    /* Set read end to non-blocking for event loop drain */
    fcntl(sr->notify_pipe[0], F_SETFL,
          fcntl(sr->notify_pipe[0], F_GETFL) | O_NONBLOCK);
    pthread_mutex_init(&sr->mutex, NULL);
    sr->eof = 0;
}

static void stdin_reader_push(stdin_reader_t *sr, char *line) {
    pthread_mutex_lock(&sr->mutex);
    if (sr->count >= sr->capacity) {
        sr->capacity *= 2;
        sr->lines = realloc(sr->lines, (size_t)sr->capacity * sizeof(char *));
    }
    sr->lines[sr->count++] = line;
    pthread_mutex_unlock(&sr->mutex);
    /* Notify event loop */
    char c = 's';
    write(sr->notify_pipe[1], &c, 1);
}

static char *stdin_reader_pop(stdin_reader_t *sr) {
    char *line = NULL;
    pthread_mutex_lock(&sr->mutex);
    if (sr->count > 0) {
        line = sr->lines[0];
        for (int i = 0; i < sr->count - 1; i++)
            sr->lines[i] = sr->lines[i + 1];
        sr->count--;
    }
    pthread_mutex_unlock(&sr->mutex);
    return line;
}

static void *stdin_reader_thread(void *arg) {
    stdin_reader_t *sr = (stdin_reader_t *)arg;

    char buf[65536];
    while (fgets(buf, sizeof(buf), stdin) != NULL) {
        /* Strip trailing newline */
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (len == 0) continue;
        stdin_reader_push(sr, strdup(buf));
    }

    pthread_mutex_lock(&sr->mutex);
    sr->eof = 1;
    pthread_mutex_unlock(&sr->mutex);
    /* Notify event loop about EOF */
    char c = 'e';
    write(sr->notify_pipe[1], &c, 1);
    return NULL;
}

/* ============================================================
 * __goqjs_send bridge function
 * ============================================================ */

static JSValue js_goqjs_send(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;

    const char *event_name = JS_ToCString(ctx, argv[0]);
    const char *data_str = JS_ToCString(ctx, argv[1]);
    if (!event_name || !data_str) {
        if (event_name) JS_FreeCString(ctx, event_name);
        if (data_str) JS_FreeCString(ctx, data_str);
        return JS_UNDEFINED;
    }

    /* Handle dispatchResult: {"id":"d_N","result":...} */
    if (strcmp(event_name, "dispatchResult") == 0) {
        /* Parse data_str as JSON to extract id and result */
        JSValue parsed = JS_ParseJSON(ctx, data_str, strlen(data_str), "<dispatch>");
        if (JS_IsObject(parsed)) {
            JSValue id_val = JS_GetPropertyStr(ctx, parsed, "id");
            JSValue result_val = JS_GetPropertyStr(ctx, parsed, "result");
            const char *id_str = JS_ToCString(ctx, id_val);

            if (id_str) {
                /* Stringify the result value */
                JSValue json_str_val = JS_JSONStringify(ctx, result_val, JS_UNDEFINED, JS_UNDEFINED);
                const char *result_str = JS_ToCString(ctx, json_str_val);
                dt_resolve(&g_dt, id_str, result_str ? result_str : "null");
                if (result_str) JS_FreeCString(ctx, result_str);
                JS_FreeValue(ctx, json_str_val);
                JS_FreeCString(ctx, id_str);
            }

            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, result_val);
        }
        JS_FreeValue(ctx, parsed);
        JS_FreeCString(ctx, event_name);
        JS_FreeCString(ctx, data_str);
        return JS_UNDEFINED;
    }

    /* Handle dispatchError: {"id":"d_N","error":"..."} */
    if (strcmp(event_name, "dispatchError") == 0) {
        JSValue parsed = JS_ParseJSON(ctx, data_str, strlen(data_str), "<dispatch>");
        if (JS_IsObject(parsed)) {
            JSValue id_val = JS_GetPropertyStr(ctx, parsed, "id");
            JSValue err_val = JS_GetPropertyStr(ctx, parsed, "error");
            const char *id_str = JS_ToCString(ctx, id_val);
            const char *err_str = JS_ToCString(ctx, err_val);

            if (id_str) {
                dt_reject(&g_dt, id_str,
                          (err_str && err_str[0]) ? err_str : "unknown dispatch error");
            }

            if (id_str) JS_FreeCString(ctx, id_str);
            if (err_str) JS_FreeCString(ctx, err_str);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, err_val);
        }
        JS_FreeValue(ctx, parsed);
        JS_FreeCString(ctx, event_name);
        JS_FreeCString(ctx, data_str);
        return JS_UNDEFINED;
    }

    /* Other events: send as event response */
    send_json_response(NULL, "event", NULL, 0, NULL,
                       event_name, data_str, 1);

    JS_FreeCString(ctx, event_name);
    JS_FreeCString(ctx, data_str);
    return JS_UNDEFINED;
}

/* ============================================================
 * processJobs — execute pending microtasks, timers, fetch results
 * ============================================================ */

static void process_jobs(void) {
    for (int i = 0; i < 100; i++) {
        JSContext *pctx = NULL;
        int executed = JS_ExecutePendingJob(g_rt, &pctx);
        if (executed < 0) {
            JSValue exc = JS_GetException(g_ctx);
            const char *msg = JS_ToCString(g_ctx, exc);
            fprintf(stderr, "[ERROR] pending job: %s\n", msg ? msg : "unknown");
            if (msg) JS_FreeCString(g_ctx, msg);
            JS_FreeValue(g_ctx, exc);
        }
        int timer_processed = tm_process_pending(g_tm, g_ctx);
        int fetch_processed = fm_process_pending(g_fm, g_ctx);
        if (executed <= 0 && !timer_processed && !fetch_processed)
            break;
    }
}

/* ============================================================
 * handleRequest — process a single JSON Lines request
 * ============================================================ */

/* Helper: extract string property from parsed JSON object */
static char *json_get_string(JSContext *ctx, JSValue obj, const char *key) {
    JSValue val = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(val) || JS_IsNull(val)) {
        JS_FreeValue(ctx, val);
        return NULL;
    }
    const char *str = JS_ToCString(ctx, val);
    char *result = str ? strdup(str) : NULL;
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, val);
    return result;
}

static void handle_request(const char *line) {
    /* Parse JSON using QuickJS */
    JSValue req = JS_ParseJSON(g_ctx, line, strlen(line), "<stdin>");
    if (JS_IsException(req)) {
        JSValue exc = JS_GetException(g_ctx);
        const char *msg = JS_ToCString(g_ctx, exc);
        send_json_response(NULL, "error", NULL, 0,
                           msg ? msg : "invalid JSON", NULL, NULL, 0);
        if (msg) JS_FreeCString(g_ctx, msg);
        JS_FreeValue(g_ctx, exc);
        return;
    }

    char *id = json_get_string(g_ctx, req, "id");
    char *type = json_get_string(g_ctx, req, "type");

    if (!type) {
        send_json_response(id, "error", NULL, 0,
                           "missing 'type' field", NULL, NULL, 0);
        free(id); free(type);
        JS_FreeValue(g_ctx, req);
        return;
    }

    if (strcmp(type, "eval") == 0) {
        char *code = json_get_string(g_ctx, req, "code");
        char *filename = json_get_string(g_ctx, req, "filename");
        if (!filename) filename = strdup("<stdin>");

        if (!code) {
            send_json_response(id, "error", NULL, 0,
                               "eval: missing 'code'", NULL, NULL, 0);
        } else {
            JSValue result = cqjs_eval(g_ctx, code, strlen(code),
                                        filename, JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(result)) {
                JSValue exc = JS_GetException(g_ctx);
                const char *msg = JS_ToCString(g_ctx, exc);
                send_json_response(id, "error", NULL, 0,
                                   msg ? msg : "eval error", NULL, NULL, 0);
                if (msg) JS_FreeCString(g_ctx, msg);
                JS_FreeValue(g_ctx, exc);
            } else {
                const char *val_str = JS_ToCString(g_ctx, result);
                send_json_response(id, "result", val_str, 0,
                                   NULL, NULL, NULL, 0);
                if (val_str) JS_FreeCString(g_ctx, val_str);
            }
            JS_FreeValue(g_ctx, result);
        }
        free(code);
        free(filename);
        process_jobs();

    } else if (strcmp(type, "eval_file") == 0) {
        char *path = json_get_string(g_ctx, req, "path");
        if (!path) {
            send_json_response(id, "error", NULL, 0,
                               "eval_file: missing 'path'", NULL, NULL, 0);
        } else {
            size_t file_len = 0;
            char *file_content = cqjs_read_file(path, &file_len);
            if (!file_content) {
                char errmsg[512];
                snprintf(errmsg, sizeof(errmsg), "eval_file: cannot read '%s'", path);
                send_json_response(id, "error", NULL, 0, errmsg, NULL, NULL, 0);
            } else {
                JSValue result = cqjs_eval(g_ctx, file_content, file_len,
                                            path, JS_EVAL_TYPE_GLOBAL);
                if (JS_IsException(result)) {
                    JSValue exc = JS_GetException(g_ctx);
                    const char *msg = JS_ToCString(g_ctx, exc);
                    send_json_response(id, "error", NULL, 0,
                                       msg ? msg : "eval_file error", NULL, NULL, 0);
                    if (msg) JS_FreeCString(g_ctx, msg);
                    JS_FreeValue(g_ctx, exc);
                } else {
                    const char *val_str = JS_ToCString(g_ctx, result);
                    send_json_response(id, "result", val_str, 0,
                                       NULL, NULL, NULL, 0);
                    if (val_str) JS_FreeCString(g_ctx, val_str);
                }
                JS_FreeValue(g_ctx, result);
                free(file_content);
            }
        }
        free(path);
        process_jobs();

    } else if (strcmp(type, "dispatch") == 0) {
        char *event = json_get_string(g_ctx, req, "event");
        char *data = json_get_string(g_ctx, req, "data");

        char *dispatch_id = dt_new_request(&g_dt);

        /* Build JS code: lx._dispatch(dispatchId, event, data) */
        size_t code_len = 256 + (event ? strlen(event) : 0) + (data ? strlen(data) : 0) + strlen(dispatch_id);
        char *code = malloc(code_len);
        snprintf(code, code_len,
                 "if (globalThis.lx && globalThis.lx._dispatch) { "
                 "globalThis.lx._dispatch(\"%s\", \"%s\", %s); }",
                 dispatch_id, event ? event : "", data ? data : "null");

        JSValue result = cqjs_eval(g_ctx, code, strlen(code),
                                    "<dispatch>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(g_ctx);
            const char *msg = JS_ToCString(g_ctx, exc);
            dt_reject(&g_dt, dispatch_id, msg ? msg : "dispatch eval error");
            if (msg) JS_FreeCString(g_ctx, msg);
            JS_FreeValue(g_ctx, exc);
        }
        JS_FreeValue(g_ctx, result);
        free(code);
        process_jobs();

        /* Wait for dispatch result with 30s timeout */
        dispatch_entry_t *entry = NULL;
        pthread_mutex_lock(&g_dt.mutex);
        entry = dt_find(&g_dt, dispatch_id);
        pthread_mutex_unlock(&g_dt.mutex);

        if (entry) {
            int64_t deadline = cqjs_now_ms() + 30000;
            while (!entry->completed && cqjs_now_ms() < deadline) {
                /* Process jobs while waiting */
                process_jobs();
                /* Short sleep to avoid busy-wait */
                usleep(10000); /* 10ms */
            }

            if (entry->completed) {
                if (entry->error) {
                    send_json_response(id, "error", NULL, 0,
                                       entry->error, NULL, NULL, 0);
                } else {
                    send_json_response(id, "result", entry->value, 1,
                                       NULL, NULL, NULL, 0);
                }
            } else {
                send_json_response(id, "error", NULL, 0,
                                   "dispatch timeout", NULL, NULL, 0);
            }
        }

        dt_remove(&g_dt, dispatch_id);
        free(dispatch_id);
        free(event);
        free(data);

    } else if (strcmp(type, "callMusicUrl") == 0) {
        char *source = json_get_string(g_ctx, req, "source");
        char *song_info = json_get_string(g_ctx, req, "songInfo");
        char *quality = json_get_string(g_ctx, req, "quality");

        char *dispatch_id = dt_new_request(&g_dt);

        /* Build dispatch data: {"action":"musicUrl","source":"...","info":{"musicInfo":...,"quality":"...","type":"..."}} */
        size_t data_len = 256 + (source ? strlen(source) : 0) +
                          (song_info ? strlen(song_info) : 0) +
                          (quality ? strlen(quality) : 0);
        char *dispatch_data = malloc(data_len);
        snprintf(dispatch_data, data_len,
                 "{\"action\":\"musicUrl\",\"source\":\"%s\","
                 "\"info\":{\"musicInfo\":%s,\"quality\":\"%s\",\"type\":\"%s\"}}",
                 source ? source : "",
                 song_info ? song_info : "{}",
                 quality ? quality : "",
                 quality ? quality : "");

        /* Build JS code */
        size_t code_len = 256 + strlen(dispatch_id) + strlen(dispatch_data);
        char *code = malloc(code_len);
        snprintf(code, code_len,
                 "if (globalThis.lx && globalThis.lx._dispatch) { "
                 "globalThis.lx._dispatch(\"%s\", \"request\", %s); }",
                 dispatch_id, dispatch_data);

        JSValue result = cqjs_eval(g_ctx, code, strlen(code),
                                    "<callMusicUrl>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(g_ctx);
            const char *msg = JS_ToCString(g_ctx, exc);
            dt_reject(&g_dt, dispatch_id, msg ? msg : "callMusicUrl eval error");
            if (msg) JS_FreeCString(g_ctx, msg);
            JS_FreeValue(g_ctx, exc);
        }
        JS_FreeValue(g_ctx, result);
        free(code);
        free(dispatch_data);
        process_jobs();

        /* Wait for dispatch result with 60s timeout */
        dispatch_entry_t *entry = NULL;
        pthread_mutex_lock(&g_dt.mutex);
        entry = dt_find(&g_dt, dispatch_id);
        pthread_mutex_unlock(&g_dt.mutex);

        if (entry) {
            int64_t deadline = cqjs_now_ms() + 60000;
            while (!entry->completed && cqjs_now_ms() < deadline) {
                process_jobs();
                usleep(10000);
            }

            if (entry->completed) {
                if (entry->error) {
                    send_json_response(id, "error", NULL, 0,
                                       entry->error, NULL, NULL, 0);
                } else {
                    send_json_response(id, "result", entry->value, 1,
                                       NULL, NULL, NULL, 0);
                }
            } else {
                send_json_response(id, "error", NULL, 0,
                                   "callMusicUrl timeout", NULL, NULL, 0);
            }
        }

        dt_remove(&g_dt, dispatch_id);
        free(dispatch_id);
        free(source);
        free(song_info);
        free(quality);

    } else if (strcmp(type, "exit") == 0) {
        send_json_response(id, "result", NULL, 0, NULL, NULL, NULL, 0);
        free(id); free(type);
        JS_FreeValue(g_ctx, req);
        /* Cleanup and exit */
        exit(0);

    } else {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg), "unknown request type: %s", type);
        send_json_response(id, "error", NULL, 0, errmsg, NULL, NULL, 0);
    }

    free(id);
    free(type);
    JS_FreeValue(g_ctx, req);
}

/* ============================================================
 * Load lx_prelude.js
 * ============================================================ */

static int load_prelude(void) {
    /* Try multiple paths */
    const char *paths[] = {
        "js/lx_prelude.js",
        "../../js/lx_prelude.js",
        "../js/lx_prelude.js",
        "lx_prelude.js",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        size_t len = 0;
        char *content = cqjs_read_file(paths[i], &len);
        if (content) {
            JSValue result = cqjs_eval(g_ctx, content, len,
                                        "lx_prelude.js", JS_EVAL_TYPE_GLOBAL);
            free(content);
            if (JS_IsException(result)) {
                JSValue exc = JS_GetException(g_ctx);
                const char *msg = JS_ToCString(g_ctx, exc);
                fprintf(stderr, "[ERROR] lx_prelude.js: %s\n", msg ? msg : "unknown");
                if (msg) JS_FreeCString(g_ctx, msg);
                JS_FreeValue(g_ctx, exc);
                JS_FreeValue(g_ctx, result);
                return -1;
            }
            JS_FreeValue(g_ctx, result);
            return 0;
        }
    }

    /* Fallback: minimal prelude */
    const char *fallback =
        "globalThis.lx = { version: '2.0.0', send: function(){}, "
        "on: function(){}, request: function(){} };";
    JSValue result = cqjs_eval(g_ctx, fallback, strlen(fallback),
                                "lx_prelude.js", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(g_ctx, result);
    return 0;
}

/* ============================================================
 * Main entry point
 * ============================================================ */

int main(int argc, char **argv) {
    /* Disable buffering on stderr and stdout for immediate output */
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);


    /* Install crash signal handlers for debugging (with sigaltstack) */
    install_crash_handlers();

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Parse command-line options */
    const char *file_path = NULL;
    int64_t memory_limit = 0;
    int64_t stack_size = 0;

    static struct option long_options[] = {
        {"file",         required_argument, 0, 'f'},
        {"memory-limit", required_argument, 0, 'm'},
        {"stack-size",   required_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:m:s:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f': file_path = optarg; break;
            case 'm': memory_limit = atoll(optarg); break;
            case 's': stack_size = atoll(optarg); break;
            default: break;
        }
    }

    /* Create QuickJS runtime */
    g_rt = JS_NewRuntime();
    if (!g_rt) {
        fprintf(stderr, "[FATAL] JS_NewRuntime failed\n");
        return 1;
    }

    if (memory_limit > 0)
        JS_SetMemoryLimit(g_rt, (size_t)(memory_limit * 1024 * 1024));
    if (stack_size > 0)
        JS_SetMaxStackSize(g_rt, (size_t)(stack_size * 1024 * 1024));
    else
        JS_SetMaxStackSize(g_rt, 8 * 1024 * 1024); /* Default 8MB */

    /* Create context */
    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) {
        fprintf(stderr, "[FATAL] JS_NewContext failed\n");
        JS_FreeRuntime(g_rt);
        return 1;
    }

    /* Initialize dispatch tracker */
    dt_init(&g_dt);

    /* Inject polyfills (creates timer/fetch managers internally) */
    g_pm = polyfill_inject_all(g_ctx);
    g_tm = g_pm->timer_mgr;
    g_fm = g_pm->fetch_mgr;

    /* Inject __goqjs_send bridge function */
    JSValue global = JS_GetGlobalObject(g_ctx);
    JS_SetPropertyStr(g_ctx, global, "__goqjs_send",
        JS_NewCFunction(g_ctx, js_goqjs_send, "__goqjs_send", 2));
    JS_FreeValue(g_ctx, global);

    /* Load lx_prelude.js */
    if (load_prelude() != 0) {
        fprintf(stderr, "[FATAL] Failed to load lx_prelude.js\n");
        return 1;
    }
    process_jobs();

    /* Execute user script if specified */
    if (file_path) {
        size_t file_len = 0;
        char *file_content = cqjs_read_file(file_path, &file_len);
        if (!file_content) {
            fprintf(stderr, "[FATAL] Cannot read file: %s\n", file_path);
            return 1;
        }
        JSValue result = cqjs_eval(g_ctx, file_content, file_len,
                                    file_path, JS_EVAL_TYPE_GLOBAL);
        free(file_content);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(g_ctx);
            const char *msg = JS_ToCString(g_ctx, exc);
            send_json_response(NULL, "error", NULL, 0,
                               msg ? msg : "eval file error", NULL, NULL, 0);
            fprintf(stderr, "[ERROR] eval file: %s\n", msg ? msg : "unknown");
            if (msg) JS_FreeCString(g_ctx, msg);
            JS_FreeValue(g_ctx, exc);
            JS_FreeValue(g_ctx, result);
            return 1;
        }
        JS_FreeValue(g_ctx, result);
        process_jobs();
    }

    /* Start stdin reader thread */
    stdin_reader_init(&g_stdin);
    pthread_t stdin_tid;
    int tret = pthread_create(&stdin_tid, NULL, stdin_reader_thread, &g_stdin);
    if (tret != 0) {
        fprintf(stderr, "[FATAL] pthread_create for stdin reader failed: %d\n", tret);
        return 1;
    }
    pthread_detach(stdin_tid);

    /* ── Event loop ── */
    struct pollfd fds[3];
    int nfds = 3;

    fds[0].fd = g_stdin.notify_pipe[0];  /* stdin notifications */
    fds[0].events = POLLIN;
    fds[1].fd = g_tm->notify_pipe[0];     /* timer notifications */
    fds[1].events = POLLIN;
    fds[2].fd = g_fm->notify_pipe[0];     /* fetch notifications */
    fds[2].events = POLLIN;

    for (;;) {
        /* Calculate poll timeout based on next timer */
        int poll_timeout = 10; /* default 10ms */
        int next_timer = tm_next_timeout_ms(g_tm);
        if (next_timer >= 0 && next_timer < poll_timeout)
            poll_timeout = next_timer;

        int ret = poll(fds, (nfds_t)nfds, poll_timeout);

        /* Drain notification pipes */
        if (fds[0].revents & POLLIN) {
            char buf[64];
            read(fds[0].fd, buf, sizeof(buf));
        }
        if (fds[1].revents & POLLIN) {
            char buf[64];
            read(fds[1].fd, buf, sizeof(buf));
        }
        if (fds[2].revents & POLLIN) {
            char buf[64];
            read(fds[2].fd, buf, sizeof(buf));
        }

        /* Process stdin lines */
        char *line;
        while ((line = stdin_reader_pop(&g_stdin)) != NULL) {
            handle_request(line);
            free(line);
        }

        /* Check for stdin EOF (with lock to avoid race) */
        pthread_mutex_lock(&g_stdin.mutex);
        int is_eof = g_stdin.eof && g_stdin.count == 0;
        pthread_mutex_unlock(&g_stdin.mutex);
        if (is_eof) {
            /* Drain remaining jobs with 5s timeout */
            int64_t drain_deadline = cqjs_now_ms() + 5000;
            while (cqjs_now_ms() < drain_deadline) {
                JSContext *pctx = NULL;
                int executed = JS_ExecutePendingJob(g_rt, &pctx);
                int timer_p = tm_process_pending(g_tm, g_ctx);
                int fetch_p = fm_process_pending(g_fm, g_ctx);
                if (executed <= 0 && !timer_p && !fetch_p) {
                    if (!fm_has_pending(g_fm))
                        break;
                    usleep(10000);
                }
            }
            break;
        }

        /* Process jobs (timers, fetch, microtasks) */
        process_jobs();

        (void)ret;
    }

    /* Cleanup */
    polyfill_close(g_pm, g_ctx);
    dt_free(&g_dt);
    JS_FreeContext(g_ctx);
    JS_FreeRuntime(g_rt);

    return 0;
}
