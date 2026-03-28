#include "polyfill.h"
#include <stdlib.h>

polyfill_managers_t *polyfill_inject_all(JSContext *ctx) {
    polyfill_managers_t *pm = malloc(sizeof(polyfill_managers_t));
    if (!pm) return NULL;

    pm->timer_mgr = malloc(sizeof(timer_manager_t));
    pm->fetch_mgr = malloc(sizeof(fetch_manager_t));

    tm_init(pm->timer_mgr);
    fm_init(pm->fetch_mgr);

    polyfill_inject_console(ctx);
    polyfill_inject_encoding(ctx);
    polyfill_inject_buffer(ctx);
    polyfill_inject_url(ctx);
    polyfill_inject_timer(ctx, pm->timer_mgr);
    polyfill_inject_fetch(ctx, pm->fetch_mgr);
    polyfill_inject_crypto(ctx);
    polyfill_inject_zlib(ctx);

    return pm;
}

void polyfill_close(polyfill_managers_t *pm, JSContext *ctx) {
    if (!pm) return;
    if (pm->fetch_mgr) {
        fm_free(pm->fetch_mgr, ctx);
        free(pm->fetch_mgr);
    }
    if (pm->timer_mgr) {
        tm_free(pm->timer_mgr, ctx);
        free(pm->timer_mgr);
    }
    free(pm);
}
