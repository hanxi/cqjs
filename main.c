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
#include <fcntl.h>

#include <execinfo.h>

#include "cqjs.h"
#include "quickjs/quickjs.h"
#include "env_manager.h"

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
 * Global state (multi-environment only)
 * ============================================================ */

/* Multi-environment manager and bytecode cache */
static env_manager_t g_env_mgr;
static bytecode_cache_t g_bc_cache;

/* Lightweight runtime for JSON parsing in main thread */
static JSRuntime *g_json_rt = NULL;
static JSContext *g_json_ctx = NULL;

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

/* Helper: extract int64 property from parsed JSON object */
static int64_t json_get_int64(JSContext *ctx, JSValue obj, const char *key, int64_t def) {
    JSValue val = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(val) || JS_IsNull(val)) {
        JS_FreeValue(ctx, val);
        return def;
    }
    int64_t result = def;
    JS_ToInt64(ctx, &result, val);
    JS_FreeValue(ctx, val);
    return result;
}

static void handle_request(const char *line) {
    /* Parse JSON using lightweight context */
    JSValue req = JS_ParseJSON(g_json_ctx, line, strlen(line), "<stdin>");
    if (JS_IsException(req)) {
        JSValue exc = JS_GetException(g_json_ctx);
        const char *msg = JS_ToCString(g_json_ctx, exc);
        send_json_response(NULL, "error", NULL,
                           NULL, 0, msg ? msg : "invalid JSON",
                           NULL, NULL, 0);
        if (msg) JS_FreeCString(g_json_ctx, msg);
        JS_FreeValue(g_json_ctx, exc);
        return;
    }

    char *id = json_get_string(g_json_ctx, req, "id");
    char *type = json_get_string(g_json_ctx, req, "type");

    if (!type) {
        send_json_response(id, "error", NULL,
                           NULL, 0, "missing 'type' field",
                           NULL, NULL, 0);
        free(id); free(type);
        JS_FreeValue(g_json_ctx, req);
        return;
    }

    if (strcmp(type, "create_env") == 0) {
        char *env_id = json_get_string(g_json_ctx, req, "env_id");
        char *init_code = json_get_string(g_json_ctx, req, "init_code");
        int64_t memory_limit = json_get_int64(g_json_ctx, req, "memory_limit", 0);
        int64_t stack_size = json_get_int64(g_json_ctx, req, "stack_size", 0);

        if (!env_id) {
            send_json_response(id, "error", NULL,
                               NULL, 0, "create_env: missing 'env_id'",
                               NULL, NULL, 0);
        } else {
            js_env_t *env = env_create(&g_env_mgr, &g_bc_cache,
                                        env_id, init_code,
                                        memory_limit, stack_size);
            if (env) {
                send_json_response(id, "result", env_id,
                                   NULL, 0, NULL, NULL, NULL, 0);
            } else {
                char errmsg[256];
                snprintf(errmsg, sizeof(errmsg),
                         "create_env: failed to create environment '%s'", env_id);
                send_json_response(id, "error", NULL,
                                   NULL, 0, errmsg, NULL, NULL, 0);
            }
        }
        free(env_id);
        free(init_code);

    } else if (strcmp(type, "destroy_env") == 0) {
        char *env_id = json_get_string(g_json_ctx, req, "env_id");
        if (!env_id) {
            send_json_response(id, "error", NULL,
                               NULL, 0, "destroy_env: missing 'env_id'",
                               NULL, NULL, 0);
        } else {
            int rc = env_destroy(&g_env_mgr, env_id);
            if (rc == 0) {
                send_json_response(id, "result", env_id,
                                   NULL, 0, "destroyed", NULL, NULL, 0);
            } else {
                char errmsg[256];
                snprintf(errmsg, sizeof(errmsg),
                         "destroy_env: environment '%s' not found", env_id);
                send_json_response(id, "error", NULL,
                                   NULL, 0, errmsg, NULL, NULL, 0);
            }
        }
        free(env_id);

    } else if (strcmp(type, "list_envs") == 0) {
        pthread_mutex_lock(&g_env_mgr.mutex);
        size_t cap = 256;
        for (int i = 0; i < g_env_mgr.count; i++) {
            cap += strlen(g_env_mgr.environments[i]->env_id) + 8;
        }
        char *arr = malloc(cap);
        char *p = arr;
        p += sprintf(p, "[");
        for (int i = 0; i < g_env_mgr.count; i++) {
            if (i > 0) p += sprintf(p, ",");
            char *eid = json_escape_string(g_env_mgr.environments[i]->env_id);
            p += sprintf(p, "%s", eid);
            free(eid);
        }
        p += sprintf(p, "]");
        pthread_mutex_unlock(&g_env_mgr.mutex);

        send_json_response(id, "result", NULL, arr, 1, NULL, NULL, NULL, 0);
        free(arr);

    } else if (strcmp(type, "eval") == 0) {
        char *env_id = json_get_string(g_json_ctx, req, "env_id");
        if (!env_id) {
            send_json_response(id, "error", NULL,
                               NULL, 0, "eval: missing 'env_id'",
                               NULL, NULL, 0);
        } else {
            char *code = json_get_string(g_json_ctx, req, "code");
            char *filename = json_get_string(g_json_ctx, req, "filename");

            if (!code) {
                send_json_response(id, "error", NULL,
                                   NULL, 0, "eval: missing 'code'",
                                   NULL, NULL, 0);
            } else {
                pthread_mutex_lock(&g_env_mgr.mutex);
                js_env_t *env = env_find(&g_env_mgr, env_id);
                pthread_mutex_unlock(&g_env_mgr.mutex);

                if (!env) {
                    char errmsg[256];
                    snprintf(errmsg, sizeof(errmsg),
                             "eval: environment '%s' not found", env_id);
                    send_json_response(id, "error", NULL,
                                       NULL, 0, errmsg, NULL, NULL, 0);
                } else {
                    env_request_t *ereq = calloc(1, sizeof(env_request_t));
                    ereq->request_id = id ? strdup(id) : NULL;
                    ereq->type = strdup("eval");
                    ereq->code = strdup(code);
                    ereq->filename = filename ? strdup(filename) : strdup("<eval>");
                    env_submit_request(env, ereq);
                }
            }
            free(code);
            free(filename);
            free(env_id);
        }

    } else if (strcmp(type, "eval_file") == 0) {
        char *env_id = json_get_string(g_json_ctx, req, "env_id");
        if (!env_id) {
            send_json_response(id, "error", NULL,
                               NULL, 0, "eval_file: missing 'env_id'",
                               NULL, NULL, 0);
        } else {
            char *path = json_get_string(g_json_ctx, req, "path");
            if (!path) {
                send_json_response(id, "error", NULL,
                                   NULL, 0, "eval_file: missing 'path'",
                                   NULL, NULL, 0);
            } else {
                pthread_mutex_lock(&g_env_mgr.mutex);
                js_env_t *env = env_find(&g_env_mgr, env_id);
                pthread_mutex_unlock(&g_env_mgr.mutex);

                if (!env) {
                    char errmsg[256];
                    snprintf(errmsg, sizeof(errmsg),
                             "eval_file: environment '%s' not found", env_id);
                    send_json_response(id, "error", NULL,
                                       NULL, 0, errmsg, NULL, NULL, 0);
                } else {
                    env_request_t *ereq = calloc(1, sizeof(env_request_t));
                    ereq->request_id = id ? strdup(id) : NULL;
                    ereq->type = strdup("eval_file");
                    ereq->path = strdup(path);
                    env_submit_request(env, ereq);
                }
            }
            free(path);
            free(env_id);
        }

    } else if (strcmp(type, "exit") == 0) {
        send_json_response(id, "result", NULL, NULL, 0, NULL, NULL, NULL, 0);
        free(id); free(type);
        JS_FreeValue(g_json_ctx, req);
        exit(0);

    } else {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg), "unknown request type: %s", type);
        send_json_response(id, "error", NULL, NULL, 0, errmsg, NULL, NULL, 0);
    }

    free(id);
    free(type);
    JS_FreeValue(g_json_ctx, req);
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

    (void)argc;
    (void)argv;

    /* Initialize multi-environment manager and bytecode cache */
    env_manager_init(&g_env_mgr);
    bc_init(&g_bc_cache, 256);

    /* Create lightweight QuickJS runtime for JSON parsing only */
    g_json_rt = JS_NewRuntime();
    if (!g_json_rt) {
        fprintf(stderr, "[FATAL] JS_NewRuntime failed\n");
        return 1;
    }
    g_json_ctx = JS_NewContext(g_json_rt);
    if (!g_json_ctx) {
        fprintf(stderr, "[FATAL] JS_NewContext failed\n");
        JS_FreeRuntime(g_json_rt);
        return 1;
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

    /* ── Event loop: only process stdin requests ── */
    struct pollfd fds[1];
    fds[0].fd = g_stdin.notify_pipe[0];
    fds[0].events = POLLIN;

    for (;;) {
        int ret = poll(fds, 1, 100);

        /* Drain notification pipe */
        if (fds[0].revents & POLLIN) {
            char buf[64];
            read(fds[0].fd, buf, sizeof(buf));
        }

        /* Process stdin lines */
        char *line;
        while ((line = stdin_reader_pop(&g_stdin)) != NULL) {
            handle_request(line);
            free(line);
        }

        /* Check for stdin EOF */
        pthread_mutex_lock(&g_stdin.mutex);
        int is_eof = g_stdin.eof && g_stdin.count == 0;
        pthread_mutex_unlock(&g_stdin.mutex);
        if (is_eof)
            break;

        (void)ret;
    }

    /* Cleanup */
    env_manager_free(&g_env_mgr);
    bc_free(&g_bc_cache);
    JS_FreeContext(g_json_ctx);
    JS_FreeRuntime(g_json_rt);

    return 0;
}
