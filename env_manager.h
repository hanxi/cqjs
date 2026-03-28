#ifndef ENV_MANAGER_H
#define ENV_MANAGER_H

#include <stdint.h>
#include <pthread.h>
#include "cqjs.h"
#include "polyfill/polyfill.h"
#include "bytecode_cache.h"
#include "quickjs/quickjs.h"

/* ============================================================
 * env_request_t — a request submitted to an environment
 * ============================================================ */

typedef struct env_request {
    char *request_id;       /* correlates with response "id" field */
    char *type;             /* "eval" or "eval_file" */
    char *code;             /* JS source code to evaluate */
    char *filename;         /* source filename for stack traces */
    char *path;             /* file path for eval_file */
    struct env_request *next;
} env_request_t;

/* ============================================================
 * js_env_t — an independent JS runtime environment
 *
 * Each environment has its own JSRuntime + JSContext + polyfills +
 * dispatch_tracker + event loop thread. Environments are fully
 * isolated and can execute JS concurrently.
 * ============================================================ */

typedef struct js_env {
    char *env_id;

    /* QuickJS engine */
    JSRuntime *runtime;
    JSContext *context;

    /* Polyfill managers (timer, fetch, etc.) */
    polyfill_managers_t *polyfills;
    timer_manager_t *timer_mgr;
    fetch_manager_t *fetch_mgr;

    /* Dispatch tracker for async request/response */
    dispatch_tracker_t dispatch_tracker;

    /* Event loop thread */
    pthread_t loop_thread;
    volatile int running;

    /* Request queue (thread-safe) */
    pthread_mutex_t request_mutex;
    env_request_t *request_head;
    env_request_t *request_tail;
    int notify_pipe[2];  /* write to wake up event loop */

    /* Configuration */
    int64_t memory_limit_mb;
    int64_t stack_size_mb;
} js_env_t;

/* Maximum number of concurrent environments (防止线程资源耗尽) */
#define MAX_ENVIRONMENTS 64

/* ============================================================
 * env_manager_t — global environment manager
 * ============================================================ */

typedef struct {
    js_env_t **environments;
    int count;
    int capacity;
    pthread_mutex_t mutex;
} env_manager_t;

/* ── Environment Manager API ── */

/* Initialize the environment manager */
void env_manager_init(env_manager_t *mgr);

/* Free the environment manager and all environments */
void env_manager_free(env_manager_t *mgr);

/* Create a new JS environment. Returns the environment or NULL on error.
 * init_code may be NULL. memory_limit_mb/stack_size_mb: 0 = use defaults. */
js_env_t *env_create(env_manager_t *mgr, bytecode_cache_t *bc_cache,
                     const char *env_id, const char *init_code,
                     int64_t memory_limit_mb, int64_t stack_size_mb);

/* Find an environment by ID. Returns NULL if not found.
 * Caller must hold mgr->mutex or accept race. */
js_env_t *env_find(env_manager_t *mgr, const char *env_id);

/* Destroy an environment by ID. Returns 0 on success, -1 if not found. */
int env_destroy(env_manager_t *mgr, const char *env_id);

/* Submit a request to an environment's queue (thread-safe).
 * The environment's event loop thread will process it. */
void env_submit_request(js_env_t *env, env_request_t *req);

#endif /* ENV_MANAGER_H */
