/*
 * cqjs.c - shared utility implementations
 *
 * Implements the public API declared in cqjs.h:
 * - Thread-safe stdout output
 * - File I/O helpers
 * - Time utilities
 * - Dispatch tracker (async request tracking)
 * - 64MB big-stack pthread wrappers for JS eval/call
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "cqjs.h"

/* ============================================================
 * Thread-safe stdout
 * ============================================================ */

static pthread_mutex_t g_stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

void cqjs_send_response(const char *json_line) {
    pthread_mutex_lock(&g_stdout_mutex);
    fprintf(stdout, "%s\n", json_line);
    fflush(stdout);
    pthread_mutex_unlock(&g_stdout_mutex);
}

/* ============================================================
 * File I/O
 * ============================================================ */

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

/* ============================================================
 * Time utilities
 * ============================================================ */

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
 * JSON escape
 * ============================================================ */

char *json_escape_string(const char *str) {
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

/* ============================================================
 * send_json_response
 * ============================================================ */

void send_json_response(const char *id, const char *type,
                        const char *env_id,
                        const char *value, int value_is_raw,
                        const char *message,
                        const char *name, const char *data, int data_is_raw) {
    size_t cap = 256;
    if (value) cap += strlen(value) * 2;
    if (message) cap += strlen(message) * 2;
    if (data) cap += strlen(data) * 2;
    if (env_id) cap += strlen(env_id) * 2;
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

    if (env_id) {
        char *eenv = json_escape_string(env_id);
        p += sprintf(p, ",\"env_id\":%s", eenv);
        free(eenv);
    }

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
    JS_UpdateStackTop(JS_GetRuntime(ea->ctx));
    ea->result = JS_Eval(ea->ctx, ea->input, ea->input_len,
                         ea->filename, ea->eval_flags);
    return NULL;
}

static void *big_stack_call_thread(void *arg) {
    call_args_t *ca = (call_args_t *)arg;
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

    fprintf(stderr, "[DEBUG] cqjs_eval: creating big_stack thread for input_len=%zu\n", input_len);
    fflush(stderr);
    int rc = pthread_create(&tid, &attr, big_stack_eval_thread, &ea);
    if (rc != 0) {
        fprintf(stderr, "[WARN] cqjs_eval: pthread_create failed rc=%d, fallback to direct eval\n", rc);
        fflush(stderr);
        pthread_attr_destroy(&attr);
        return JS_Eval(ctx, input, input_len, filename, eval_flags);
    }
    pthread_attr_destroy(&attr);
    fprintf(stderr, "[DEBUG] cqjs_eval: waiting for big_stack thread\n");
    fflush(stderr);
    pthread_join(tid, NULL);
    fprintf(stderr, "[DEBUG] cqjs_eval: big_stack thread completed\n");
    fflush(stderr);
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    return ea.result;
}

/* ============================================================
 * cqjs_eval_function — run JS_EvalFunction on a big-stack thread
 *
 * JS_EvalFunction executes compiled bytecode. For obfuscated code
 * (e.g. jsjiami), execution can require very deep recursion that
 * overflows the default thread stack. This wrapper runs it on a
 * 64MB stack thread, just like cqjs_eval does for compilation.
 * ============================================================ */

typedef struct {
    JSContext *ctx;
    JSValue func_obj;  /* consumed by JS_EvalFunction */
    JSValue result;
} eval_func_args_t;

static void *big_stack_eval_func_thread(void *arg) {
    eval_func_args_t *efa = (eval_func_args_t *)arg;
    JS_UpdateStackTop(JS_GetRuntime(efa->ctx));
    efa->result = JS_EvalFunction(efa->ctx, efa->func_obj);
    return NULL;
}

JSValue cqjs_eval_function(JSContext *ctx, JSValue func_obj) {
    eval_func_args_t efa = {
        .ctx = ctx,
        .func_obj = func_obj,
    };

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, BIG_STACK_SIZE);

    if (pthread_create(&tid, &attr, big_stack_eval_func_thread, &efa) != 0) {
        pthread_attr_destroy(&attr);
        return JS_EvalFunction(ctx, func_obj);
    }
    pthread_attr_destroy(&attr);
    pthread_join(tid, NULL);
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    return efa.result;
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
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    return ca.result;
}
