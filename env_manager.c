/*
 * env_manager.c — multi-environment manager implementation
 *
 * Each JS environment runs in its own thread with an independent
 * JSRuntime + JSContext + polyfills + dispatch_tracker.
 * Requests are submitted to an environment's queue and processed
 * by its dedicated event loop thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>

#include "env_manager.h"
#include "bytecode_cache.h"
#include "cqjs.h"
#include "polyfill/polyfill.h"
#include "quickjs/quickjs.h"


/* Global bytecode cache reference (set by main.c) */
static bytecode_cache_t *g_bc_cache = NULL;


/* ============================================================
 * __goqjs_send bridge — per-environment version
 *
 * Each environment gets its own __goqjs_send that references
 * its own dispatch_tracker via JS opaque data.
 * ============================================================ */

typedef struct {
    dispatch_tracker_t *dt;
    char *env_id;
} env_bridge_data_t;

static JSValue js_env_goqjs_send(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;

    const char *event_name = JS_ToCString(ctx, argv[0]);
    const char *data_str = JS_ToCString(ctx, argv[1]);
    if (!event_name || !data_str) {
        if (event_name) JS_FreeCString(ctx, event_name);
        if (data_str) JS_FreeCString(ctx, data_str);
        return JS_UNDEFINED;
    }

    /* Get bridge data from context opaque */
    env_bridge_data_t *bd = JS_GetContextOpaque(ctx);

    if (!bd) {
        fprintf(stderr, "[ERROR] __goqjs_send: no bridge data on context, dropping event '%s'\n", event_name);
        JS_FreeCString(ctx, event_name);
        JS_FreeCString(ctx, data_str);
        return JS_UNDEFINED;
    }

    /* Handle dispatch response events */
    if (strcmp(event_name, "response") == 0) {
        JSValue parsed = JS_ParseJSON(ctx, data_str, strlen(data_str), "<dispatch>");
        if (JS_IsObject(parsed)) {
            JSValue id_val = JS_GetPropertyStr(ctx, parsed, "id");
            JSValue data_val = JS_GetPropertyStr(ctx, parsed, "data");
            const char *id_str = JS_ToCString(ctx, id_val);

            if (id_str) {
                const char *result_str = JS_ToCString(ctx, data_val);
                dt_resolve(bd->dt, id_str, result_str ? result_str : "null");
                if (result_str) JS_FreeCString(ctx, result_str);
            }

            if (id_str) JS_FreeCString(ctx, id_str);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, data_val);
        }
        JS_FreeValue(ctx, parsed);
        JS_FreeCString(ctx, event_name);
        JS_FreeCString(ctx, data_str);
        return JS_UNDEFINED;
    }

    /* Handle dispatch error events */
    if (strcmp(event_name, "error") == 0) {
        JSValue parsed = JS_ParseJSON(ctx, data_str, strlen(data_str), "<dispatch>");
        if (JS_IsObject(parsed)) {
            JSValue id_val = JS_GetPropertyStr(ctx, parsed, "id");
            JSValue err_val = JS_GetPropertyStr(ctx, parsed, "error");
            const char *id_str = JS_ToCString(ctx, id_val);
            const char *err_str = JS_ToCString(ctx, err_val);

            if (id_str) {
                dt_reject(bd->dt, id_str,
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

    /* Other events: send as event response with env_id, name, and data */
    send_json_response(NULL, "event", bd->env_id,
                       NULL, 0, NULL, event_name, data_str, 1);

    JS_FreeCString(ctx, event_name);
    JS_FreeCString(ctx, data_str);
    return JS_UNDEFINED;
}

/* ============================================================
 * process_jobs — execute pending microtasks, timers, fetch results
 * (per-environment version)
 * ============================================================ */

static void env_process_jobs(js_env_t *env) {
    for (int i = 0; i < 100; i++) {
        JSContext *pctx = NULL;
        int executed = JS_ExecutePendingJob(env->runtime, &pctx);
        if (executed < 0) {
            JSValue exc = JS_GetException(env->context);
            const char *msg = JS_ToCString(env->context, exc);
            fprintf(stderr, "[ERROR] [%s] pending job: %s\n",
                    env->env_id, msg ? msg : "unknown");
            if (msg) JS_FreeCString(env->context, msg);
            JS_FreeValue(env->context, exc);
        }
        int timer_processed = tm_process_pending(env->timer_mgr, env->context);
        int fetch_processed = fm_process_pending(env->fetch_mgr, env->context);
        if (executed <= 0 && !timer_processed && !fetch_processed)
            break;
    }
}

/* ============================================================
 * env_handle_eval — handle an eval request with bytecode cache
 * ============================================================ */

static void env_handle_eval(js_env_t *env, env_request_t *req) {
    if (!req->code) {
        send_json_response(req->request_id, "error", NULL,
                           NULL, 0, "eval: missing 'code'",
                           NULL, NULL, 0);
        return;
    }

    const char *filename = req->filename ? req->filename : "<eval>";
    JSValue result;

    if (g_bc_cache) {
        /* Try bytecode cache */
        char *hash = bc_compute_hash(req->code, strlen(req->code));
        bytecode_entry_t *cached = bc_lookup(g_bc_cache, hash);

        if (cached) {
            /* Cache hit: deserialize and execute */
            JSValue obj = JS_ReadObject(env->context, cached->bytecode,
                                        cached->bytecode_len, JS_READ_OBJ_BYTECODE);
            if (JS_IsException(obj)) {
                /* Fallback to normal eval on deserialization error */
                JS_FreeValue(env->context, JS_GetException(env->context));
                result = cqjs_eval(env->context, req->code, strlen(req->code),
                                   filename, JS_EVAL_TYPE_GLOBAL);
            } else {
                result = JS_EvalFunction(env->context, obj);
            }
            fprintf(stderr, "[DEBUG] [%s] bytecode cache HIT for %s\n",
                    env->env_id, filename);
        } else {
            /* Cache miss: compile, cache, execute */
            JSValue compiled = JS_Eval(env->context, req->code, strlen(req->code),
                                       filename,
                                       JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(compiled)) {
                result = compiled;  /* propagate compile error */
            } else {
                /* Serialize bytecode for caching */
                size_t bc_len = 0;
                uint8_t *bc_data = JS_WriteObject(env->context, &bc_len, compiled,
                                                   JS_WRITE_OBJ_BYTECODE);
                if (bc_data && bc_len > 0) {
                    bc_insert(g_bc_cache, hash, bc_data, bc_len);
                    js_free(env->context, bc_data);
                }
                /* Execute the compiled function */
                result = JS_EvalFunction(env->context, compiled);
            }
            fprintf(stderr, "[DEBUG] [%s] bytecode cache MISS for %s\n",
                    env->env_id, filename);
        }
        free(hash);
    } else {
        /* No cache: direct eval */
        result = cqjs_eval(env->context, req->code, strlen(req->code),
                           filename, JS_EVAL_TYPE_GLOBAL);
    }

    /* Send response */
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(env->context);
        const char *msg = JS_ToCString(env->context, exc);
        send_json_response(req->request_id, "error", NULL,
                           NULL, 0, msg ? msg : "eval error",
                           NULL, NULL, 0);
        if (msg) JS_FreeCString(env->context, msg);
        JS_FreeValue(env->context, exc);
    } else {
        const char *val_str = JS_ToCString(env->context, result);
        send_json_response(req->request_id, "result", NULL,
                           val_str, 0, NULL, NULL, NULL, 0);
        if (val_str) JS_FreeCString(env->context, val_str);
    }
    JS_FreeValue(env->context, result);

    /* Process pending jobs after eval */
    env_process_jobs(env);
}

/* ============================================================
 * env_handle_eval_file — handle an eval_file request
 * ============================================================ */

static void env_handle_eval_file(js_env_t *env, env_request_t *req) {
    if (!req->path) {
        send_json_response(req->request_id, "error", NULL,
                           NULL, 0, "eval_file: missing 'path'",
                           NULL, NULL, 0);
        return;
    }

    size_t file_len = 0;
    char *file_content = cqjs_read_file(req->path, &file_len);
    if (!file_content) {
        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg), "eval_file: cannot read '%s'", req->path);
        send_json_response(req->request_id, "error", NULL,
                           NULL, 0, errmsg, NULL, NULL, 0);
        return;
    }

    JSValue result = cqjs_eval(env->context, file_content, file_len,
                                req->path, JS_EVAL_TYPE_GLOBAL);
    free(file_content);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(env->context);
        const char *msg = JS_ToCString(env->context, exc);
        send_json_response(req->request_id, "error", NULL,
                           NULL, 0, msg ? msg : "eval_file error",
                           NULL, NULL, 0);
        if (msg) JS_FreeCString(env->context, msg);
        JS_FreeValue(env->context, exc);
    } else {
        const char *val_str = JS_ToCString(env->context, result);
        send_json_response(req->request_id, "result", NULL,
                           val_str, 0, NULL, NULL, NULL, 0);
        if (val_str) JS_FreeCString(env->context, val_str);
    }
    JS_FreeValue(env->context, result);

    env_process_jobs(env);
}

/* ============================================================
 * env_event_loop — per-environment event loop thread
 * ============================================================ */

static void *env_event_loop(void *arg) {
    js_env_t *env = (js_env_t *)arg;

    struct pollfd fds[3];
    fds[0].fd = env->notify_pipe[0];           /* request notifications */
    fds[0].events = POLLIN;
    fds[1].fd = env->timer_mgr->notify_pipe[0]; /* timer notifications */
    fds[1].events = POLLIN;
    fds[2].fd = env->fetch_mgr->notify_pipe[0]; /* fetch notifications */
    fds[2].events = POLLIN;

    while (env->running) {
        /* Calculate poll timeout */
        int poll_timeout = 100; /* default 100ms */
        int next_timer = tm_next_timeout_ms(env->timer_mgr);
        if (next_timer >= 0 && next_timer < poll_timeout)
            poll_timeout = next_timer;

        int ret = poll(fds, 3, poll_timeout);
        (void)ret;

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

        /* Process request queue */
        for (;;) {
            pthread_mutex_lock(&env->request_mutex);
            env_request_t *req = env->request_head;
            if (req) {
                env->request_head = req->next;
                if (!env->request_head)
                    env->request_tail = NULL;
            }
            pthread_mutex_unlock(&env->request_mutex);

            if (!req) break;

            /* Handle request */
            if (strcmp(req->type, "eval") == 0) {
                env_handle_eval(env, req);
            } else if (strcmp(req->type, "eval_file") == 0) {
                env_handle_eval_file(env, req);
            } else {
                char errmsg[128];
                snprintf(errmsg, sizeof(errmsg),
                         "unknown request type for env: %s", req->type);
                send_json_response(req->request_id, "error", NULL,
                                   NULL, 0, errmsg, NULL, NULL, 0);
            }

            /* Free request */
            free(req->request_id);
            free(req->type);
            free(req->code);
            free(req->filename);
            free(req->path);
            free(req);
        }

        /* Process pending jobs (timers, fetch, microtasks) */
        env_process_jobs(env);
    }

    return NULL;
}

/* ============================================================
 * Environment Manager API
 * ============================================================ */

void env_manager_init(env_manager_t *mgr) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->capacity = 16;
    mgr->environments = calloc((size_t)mgr->capacity, sizeof(js_env_t *));
    pthread_mutex_init(&mgr->mutex, NULL);
}

void env_manager_free(env_manager_t *mgr) {
    pthread_mutex_lock(&mgr->mutex);
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->environments[i]) {
            js_env_t *env = mgr->environments[i];

            /* Stop event loop */
            env->running = 0;
            char c = 'q';
            write(env->notify_pipe[1], &c, 1);
            pthread_join(env->loop_thread, NULL);

            /* Cleanup */
            polyfill_close(env->polyfills, env->context);
            dt_free(&env->dispatch_tracker);
            JS_FreeContext(env->context);
            JS_FreeRuntime(env->runtime);

            /* Free request queue */
            env_request_t *req = env->request_head;
            while (req) {
                env_request_t *next = req->next;
                free(req->request_id);
                free(req->type);
                free(req->code);
                free(req->filename);
                free(req->path);
                free(req);
                req = next;
            }

            close(env->notify_pipe[0]);
            close(env->notify_pipe[1]);
            pthread_mutex_destroy(&env->request_mutex);
            free(env->env_id);
            free(env);
        }
    }
    free(mgr->environments);
    mgr->environments = NULL;
    mgr->count = 0;
    pthread_mutex_unlock(&mgr->mutex);
    pthread_mutex_destroy(&mgr->mutex);
}

js_env_t *env_find(env_manager_t *mgr, const char *env_id) {
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->environments[i] &&
            strcmp(mgr->environments[i]->env_id, env_id) == 0) {
            return mgr->environments[i];
        }
    }
    return NULL;
}

js_env_t *env_create(env_manager_t *mgr, bytecode_cache_t *bc_cache,
                     const char *env_id, const char *init_code,
                     int64_t memory_limit_mb, int64_t stack_size_mb) {
    pthread_mutex_lock(&mgr->mutex);

    /* Check environment count limit */
    if (mgr->count >= MAX_ENVIRONMENTS) {
        pthread_mutex_unlock(&mgr->mutex);
        fprintf(stderr, "[ERROR] env_create: max environments (%d) reached\n", MAX_ENVIRONMENTS);
        return NULL;
    }

    /* Check for duplicate env_id */
    if (env_find(mgr, env_id)) {
        pthread_mutex_unlock(&mgr->mutex);
        fprintf(stderr, "[ERROR] env_create: env_id '%s' already exists\n", env_id);
        return NULL;
    }

    /* Store global bc_cache reference */
    g_bc_cache = bc_cache;

    /* Allocate environment */
    js_env_t *env = calloc(1, sizeof(js_env_t));
    if (!env) {
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }

    env->env_id = strdup(env_id);
    env->memory_limit_mb = memory_limit_mb;
    env->stack_size_mb = stack_size_mb;

    /* Create JSRuntime */
    env->runtime = JS_NewRuntime();
    if (!env->runtime) {
        free(env->env_id);
        free(env);
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }

    if (memory_limit_mb > 0)
        JS_SetMemoryLimit(env->runtime, (size_t)(memory_limit_mb * 1024 * 1024));
    if (stack_size_mb > 0)
        JS_SetMaxStackSize(env->runtime, (size_t)(stack_size_mb * 1024 * 1024));
    else
        JS_SetMaxStackSize(env->runtime, 8 * 1024 * 1024); /* Default 8MB */

    /* Create JSContext */
    env->context = JS_NewContext(env->runtime);
    if (!env->context) {
        JS_FreeRuntime(env->runtime);
        free(env->env_id);
        free(env);
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }

    /* Initialize dispatch tracker */
    dt_init(&env->dispatch_tracker);

    /* Inject polyfills */
    env->polyfills = polyfill_inject_all(env->context);
    env->timer_mgr = env->polyfills->timer_mgr;
    env->fetch_mgr = env->polyfills->fetch_mgr;

    /* Inject __goqjs_send bridge with per-env dispatch tracker */
    env_bridge_data_t *bd = calloc(1, sizeof(env_bridge_data_t));
    bd->dt = &env->dispatch_tracker;
    bd->env_id = env->env_id;  /* shares lifetime with env */

    JSValue global = JS_GetGlobalObject(env->context);

    /* Store bridge data on the context for retrieval by __goqjs_send */
    JS_SetContextOpaque(env->context, bd);

    /* Set __goqjs_send function */
    JS_SetPropertyStr(env->context, global, "__goqjs_send",
        JS_NewCFunction(env->context, js_env_goqjs_send, "__goqjs_send", 2));

    JS_FreeValue(env->context, global);

    /* Execute init_code if provided */
    if (init_code && init_code[0]) {
        JSValue result = cqjs_eval(env->context, init_code, strlen(init_code),
                                   "<init>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(env->context);
            const char *msg = JS_ToCString(env->context, exc);
            fprintf(stderr, "[ERROR] [%s] init_code: %s\n",
                    env_id, msg ? msg : "unknown");
            if (msg) JS_FreeCString(env->context, msg);
            JS_FreeValue(env->context, exc);
        }
        JS_FreeValue(env->context, result);

        /* Process any pending jobs from init */
        env_process_jobs(env);
    }

    /* Setup request queue */
    pthread_mutex_init(&env->request_mutex, NULL);
    env->request_head = NULL;
    env->request_tail = NULL;
    pipe(env->notify_pipe);
    fcntl(env->notify_pipe[0], F_SETFL,
          fcntl(env->notify_pipe[0], F_GETFL) | O_NONBLOCK);

    /* Start event loop thread */
    env->running = 1;
    int tret = pthread_create(&env->loop_thread, NULL, env_event_loop, env);
    if (tret != 0) {
        fprintf(stderr, "[ERROR] [%s] pthread_create failed: %d\n", env_id, tret);
        polyfill_close(env->polyfills, env->context);
        dt_free(&env->dispatch_tracker);
        JS_FreeContext(env->context);
        JS_FreeRuntime(env->runtime);
        close(env->notify_pipe[0]);
        close(env->notify_pipe[1]);
        pthread_mutex_destroy(&env->request_mutex);
        free(env->env_id);
        free(env);
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }

    /* Add to manager */
    if (mgr->count >= mgr->capacity) {
        mgr->capacity *= 2;
        mgr->environments = realloc(mgr->environments,
                                     (size_t)mgr->capacity * sizeof(js_env_t *));
    }
    mgr->environments[mgr->count++] = env;

    pthread_mutex_unlock(&mgr->mutex);

    fprintf(stderr, "[INFO] env_create: '%s' created (mem=%lldMB, stack=%lldMB)\n",
            env_id, (long long)memory_limit_mb, (long long)stack_size_mb);
    return env;
}

int env_destroy(env_manager_t *mgr, const char *env_id) {
    pthread_mutex_lock(&mgr->mutex);

    int found = -1;
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->environments[i] &&
            strcmp(mgr->environments[i]->env_id, env_id) == 0) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        pthread_mutex_unlock(&mgr->mutex);
        return -1;
    }

    js_env_t *env = mgr->environments[found];

    /* Remove from array */
    for (int i = found; i < mgr->count - 1; i++)
        mgr->environments[i] = mgr->environments[i + 1];
    mgr->count--;

    pthread_mutex_unlock(&mgr->mutex);

    /* Stop event loop thread */
    env->running = 0;
    char c = 'q';
    write(env->notify_pipe[1], &c, 1);
    pthread_join(env->loop_thread, NULL);

    /* Cleanup resources */
    polyfill_close(env->polyfills, env->context);
    dt_free(&env->dispatch_tracker);
    JS_FreeContext(env->context);
    JS_FreeRuntime(env->runtime);

    /* Free remaining requests */
    env_request_t *req = env->request_head;
    while (req) {
        env_request_t *next = req->next;
        free(req->request_id);
        free(req->type);
        free(req->code);
        free(req->filename);
        free(req);
        req = next;
    }

    close(env->notify_pipe[0]);
    close(env->notify_pipe[1]);
    pthread_mutex_destroy(&env->request_mutex);

    fprintf(stderr, "[INFO] env_destroy: '%s' destroyed\n", env->env_id);
    free(env->env_id);
    free(env);

    return 0;
}

void env_submit_request(js_env_t *env, env_request_t *req) {
    pthread_mutex_lock(&env->request_mutex);
    req->next = NULL;
    if (env->request_tail) {
        env->request_tail->next = req;
    } else {
        env->request_head = req;
    }
    env->request_tail = req;
    pthread_mutex_unlock(&env->request_mutex);

    /* Wake up event loop */
    char c = 'r';
    write(env->notify_pipe[1], &c, 1);
}
