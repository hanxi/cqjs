#include "polyfill.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>

/* ---- Timer Manager implementation ---- */

void tm_init(timer_manager_t *tm) {
    memset(tm, 0, sizeof(*tm));
    tm->head = NULL;
    tm->counter = 0;
    pipe(tm->notify_pipe);
    /* Set read end to non-blocking so drain loops don't block */
    fcntl(tm->notify_pipe[0], F_SETFL,
          fcntl(tm->notify_pipe[0], F_GETFL) | O_NONBLOCK);
    pthread_mutex_init(&tm->mutex, NULL);
}

static void timespec_now(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static void timespec_add_ms(struct timespec *ts, int ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int timespec_cmp(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec < b->tv_sec) return -1;
    if (a->tv_sec > b->tv_sec) return 1;
    if (a->tv_nsec < b->tv_nsec) return -1;
    if (a->tv_nsec > b->tv_nsec) return 1;
    return 0;
}

int64_t tm_set_timeout(timer_manager_t *tm, JSValue callback, int delay_ms) {
    timer_entry_t *entry = calloc(1, sizeof(timer_entry_t));
    if (!entry) return -1;

    pthread_mutex_lock(&tm->mutex);
    entry->id = ++tm->counter;
    entry->callback = callback;  /* caller must have DupValue'd */
    entry->interval_ms = 0;
    entry->is_interval = 0;
    entry->canceled = 0;
    timespec_now(&entry->next_fire);
    timespec_add_ms(&entry->next_fire, delay_ms < 0 ? 0 : delay_ms);

    /* insert at head */
    entry->next = tm->head;
    tm->head = entry;
    int64_t id = entry->id;
    pthread_mutex_unlock(&tm->mutex);

    /* notify event loop */
    char c = 't';
    write(tm->notify_pipe[1], &c, 1);

    return id;
}

int64_t tm_set_interval(timer_manager_t *tm, JSValue callback, int interval_ms) {
    timer_entry_t *entry = calloc(1, sizeof(timer_entry_t));
    if (!entry) return -1;

    if (interval_ms <= 0) interval_ms = 1;

    pthread_mutex_lock(&tm->mutex);
    entry->id = ++tm->counter;
    entry->callback = callback;  /* caller must have DupValue'd */
    entry->interval_ms = interval_ms;
    entry->is_interval = 1;
    entry->canceled = 0;
    timespec_now(&entry->next_fire);
    timespec_add_ms(&entry->next_fire, interval_ms);

    entry->next = tm->head;
    tm->head = entry;
    int64_t id = entry->id;
    pthread_mutex_unlock(&tm->mutex);

    char c = 't';
    write(tm->notify_pipe[1], &c, 1);

    return id;
}

void tm_clear(timer_manager_t *tm, int64_t id) {
    pthread_mutex_lock(&tm->mutex);
    for (timer_entry_t *e = tm->head; e; e = e->next) {
        if (e->id == id) {
            e->canceled = 1;
            break;
        }
    }
    pthread_mutex_unlock(&tm->mutex);
}

int tm_process_pending(timer_manager_t *tm, JSContext *ctx) {
    /* drain notification pipe */
    char buf[64];
    while (read(tm->notify_pipe[0], buf, sizeof(buf)) > 0) {}

    struct timespec now;
    timespec_now(&now);

    int processed = 0;

    pthread_mutex_lock(&tm->mutex);

    /* Collect fired timers */
    timer_entry_t **pp = &tm->head;
    while (*pp) {
        timer_entry_t *e = *pp;
        if (e->canceled) {
            /* remove canceled */
            *pp = e->next;
            JS_FreeValue(ctx, e->callback);
            free(e);
            continue;
        }
        if (timespec_cmp(&now, &e->next_fire) >= 0) {
            /* timer fired */
            JSValue cb = JS_DupValue(ctx, e->callback);

            if (e->is_interval) {
                /* reschedule */
                timespec_now(&e->next_fire);
                timespec_add_ms(&e->next_fire, e->interval_ms);
                pp = &e->next;
            } else {
                /* remove one-shot */
                *pp = e->next;
                JS_FreeValue(ctx, e->callback);
                free(e);
            }

            pthread_mutex_unlock(&tm->mutex);

            /* execute callback on big stack to avoid stack overflow */
            JSValue result = cqjs_call(ctx, cb, JS_UNDEFINED, 0, NULL);
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, cb);
            processed++;

            pthread_mutex_lock(&tm->mutex);
            /* restart scan since list may have changed */
            pp = &tm->head;
            continue;
        }
        pp = &e->next;
    }

    pthread_mutex_unlock(&tm->mutex);
    return processed;
}

int tm_next_timeout_ms(timer_manager_t *tm) {
    pthread_mutex_lock(&tm->mutex);
    if (!tm->head) {
        pthread_mutex_unlock(&tm->mutex);
        return -1;
    }

    struct timespec now;
    timespec_now(&now);

    int min_ms = -1;
    for (timer_entry_t *e = tm->head; e; e = e->next) {
        if (e->canceled) continue;
        int64_t diff_ms = (e->next_fire.tv_sec - now.tv_sec) * 1000 +
                          (e->next_fire.tv_nsec - now.tv_nsec) / 1000000;
        if (diff_ms < 0) diff_ms = 0;
        if (min_ms < 0 || diff_ms < min_ms) {
            min_ms = (int)diff_ms;
        }
    }

    pthread_mutex_unlock(&tm->mutex);
    return min_ms;
}

void tm_free(timer_manager_t *tm, JSContext *ctx) {
    pthread_mutex_lock(&tm->mutex);
    timer_entry_t *e = tm->head;
    while (e) {
        timer_entry_t *next = e->next;
        JS_FreeValue(ctx, e->callback);
        free(e);
        e = next;
    }
    tm->head = NULL;
    pthread_mutex_unlock(&tm->mutex);

    close(tm->notify_pipe[0]);
    close(tm->notify_pipe[1]);
    pthread_mutex_destroy(&tm->mutex);
}

/* ---- JS bindings ---- */

/* We store timer_manager_t* in a global property */
static timer_manager_t *get_tm(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, "__tm_ptr");
    JS_FreeValue(ctx, global);
    if (JS_IsUndefined(val)) return NULL;
    int64_t ptr;
    JS_ToInt64(ctx, &ptr, val);
    JS_FreeValue(ctx, val);
    return (timer_manager_t *)(uintptr_t)ptr;
}

static JSValue js_set_timeout(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    timer_manager_t *tm = get_tm(ctx);
    if (!tm || argc < 1) return JS_NewInt32(ctx, 0);

    JSValue fn = JS_DupValue(ctx, argv[0]);
    int delay = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &delay, argv[1]);
    }

    int64_t id = tm_set_timeout(tm, fn, delay);
    return JS_NewInt64(ctx, id);
}

static JSValue js_clear_timeout(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    timer_manager_t *tm = get_tm(ctx);
    if (!tm || argc < 1) return JS_UNDEFINED;

    int64_t id;
    JS_ToInt64(ctx, &id, argv[0]);
    tm_clear(tm, id);
    return JS_UNDEFINED;
}

static JSValue js_set_interval(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    timer_manager_t *tm = get_tm(ctx);
    if (!tm || argc < 1) return JS_NewInt32(ctx, 0);

    JSValue fn = JS_DupValue(ctx, argv[0]);
    int delay = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &delay, argv[1]);
    }

    int64_t id = tm_set_interval(tm, fn, delay);
    return JS_NewInt64(ctx, id);
}

static JSValue js_clear_interval(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    timer_manager_t *tm = get_tm(ctx);
    if (!tm || argc < 1) return JS_UNDEFINED;

    int64_t id;
    JS_ToInt64(ctx, &id, argv[0]);
    tm_clear(tm, id);
    return JS_UNDEFINED;
}

void polyfill_inject_timer(JSContext *ctx, timer_manager_t *tm) {
    JSValue global = JS_GetGlobalObject(ctx);

    /* Store tm pointer for JS callbacks to retrieve */
    JS_SetPropertyStr(ctx, global, "__tm_ptr",
        JS_NewInt64(ctx, (int64_t)(uintptr_t)tm));

    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunction(ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, js_clear_interval, "clearInterval", 1));

    JS_FreeValue(ctx, global);
}
