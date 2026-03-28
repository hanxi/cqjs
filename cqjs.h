#ifndef CQJS_H
#define CQJS_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "quickjs/quickjs.h"

/* ============================================================
 * dispatch_tracker — tracks async dispatch requests
 * ============================================================ */

typedef struct {
    char *id;        /* request ID, e.g. "d_1" */
    char *value;     /* JSON string result (owned) */
    char *error;     /* error message (owned), NULL if success */
    int completed;   /* 1 when result/error is set */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} dispatch_entry_t;

typedef struct {
    dispatch_entry_t **entries;
    int count;
    int capacity;
    int64_t counter;  /* atomic counter for generating IDs */
    pthread_mutex_t mutex;
} dispatch_tracker_t;

/* Initialize a dispatch tracker */
void dt_init(dispatch_tracker_t *dt);

/* Generate a new request ID (format: "d_N"), caller must free */
char *dt_new_request(dispatch_tracker_t *dt);

/* Find entry by ID (caller must hold dt->mutex or call under lock) */
dispatch_entry_t *dt_find(dispatch_tracker_t *dt, const char *id);

/* Resolve a dispatch request with a value */
void dt_resolve(dispatch_tracker_t *dt, const char *id, const char *value);

/* Reject a dispatch request with an error */
void dt_reject(dispatch_tracker_t *dt, const char *id, const char *error);

/* Remove and free a completed entry */
void dt_remove(dispatch_tracker_t *dt, const char *id);

/* Free all entries and resources */
void dt_free(dispatch_tracker_t *dt);

/* ============================================================
 * timer_manager — setTimeout/setInterval
 * ============================================================ */

typedef struct timer_entry {
    int64_t id;
    JSValue callback;
    int interval_ms;
    int is_interval;
    int canceled;
    struct timespec next_fire;
    struct timer_entry *next;
} timer_entry_t;

typedef struct {
    timer_entry_t *head;
    int64_t counter;
    int notify_pipe[2];  /* pipe for event loop notification */
    pthread_mutex_t mutex;
} timer_manager_t;

/* Initialize timer manager (creates pipe) */
void tm_init(timer_manager_t *tm);

/* Add a setTimeout timer, returns timer ID */
int64_t tm_set_timeout(timer_manager_t *tm, JSValue callback, int delay_ms);

/* Add a setInterval timer, returns timer ID */
int64_t tm_set_interval(timer_manager_t *tm, JSValue callback, int interval_ms);

/* Cancel a timer by ID */
void tm_clear(timer_manager_t *tm, int64_t id);

/* Process expired timers, execute callbacks. Returns number processed. */
int tm_process_pending(timer_manager_t *tm, JSContext *ctx);

/* Get milliseconds until next timer fires, or -1 if no timers */
int tm_next_timeout_ms(timer_manager_t *tm);

/* Free all timers and close pipe */
void tm_free(timer_manager_t *tm, JSContext *ctx);

/* ============================================================
 * fetch_manager — async HTTP via libcurl + pthread
 * ============================================================ */

typedef struct {
    char *key;
    char *value;
} header_pair_t;

typedef struct fetch_entry {
    int64_t id;
    JSValue resolve;
    JSValue reject;
    /* request params (owned strings) */
    char *url;
    char *method;
    char *body;
    header_pair_t *headers;
    int header_count;
    /* result (filled by worker thread) */
    int status_code;
    char *status_text;
    char *response_body;
    size_t response_body_len;
    header_pair_t *response_headers;
    int response_header_count;
    int completed;  /* set to 1 by worker thread */
    int is_error;
    char *error_msg;
    struct fetch_entry *next;
} fetch_entry_t;

typedef struct {
    fetch_entry_t *head;
    int64_t counter;
    int notify_pipe[2];  /* pipe for event loop notification */
    pthread_mutex_t mutex;
} fetch_manager_t;

/* Initialize fetch manager (creates pipe) */
void fm_init(fetch_manager_t *fm);

/* Process completed fetch requests, resolve/reject promises. Returns count. */
int fm_process_pending(fetch_manager_t *fm, JSContext *ctx);

/* Check if there are pending fetch requests */
int fm_has_pending(fetch_manager_t *fm);

/* Free all entries and close pipe */
void fm_free(fetch_manager_t *fm, JSContext *ctx);

/* ============================================================
 * 64MB big-stack pthread helpers
 * ============================================================ */

/* Eval JS code on a 64MB stack pthread */
JSValue cqjs_eval(JSContext *ctx, const char *input, size_t input_len,
                  const char *filename, int eval_flags);

/* Call JS function on a 64MB stack pthread */
JSValue cqjs_call(JSContext *ctx, JSValueConst func_obj,
                  JSValueConst this_obj, int argc, JSValueConst *argv);

/* ============================================================
 * Utility helpers
 * ============================================================ */

/* Read entire file into malloc'd buffer, returns NULL on error */
char *cqjs_read_file(const char *path, size_t *out_len);

/* Send a JSON response line to stdout (thread-safe) */
void cqjs_send_response(const char *json_line);

/* Get current time in milliseconds */
int64_t cqjs_now_ms(void);

/* Get current timespec */
void cqjs_timespec_now(struct timespec *ts);

/* Add milliseconds to a timespec */
void cqjs_timespec_add_ms(struct timespec *ts, int ms);

/* Compare two timespecs: returns <0, 0, >0 */
int cqjs_timespec_cmp(const struct timespec *a, const struct timespec *b);

#endif /* CQJS_H */
