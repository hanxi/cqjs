#include "polyfill.h"
#include <stdio.h>
#include <string.h>

static JSValue js_console_log_impl(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv,
                                    const char *prefix) {
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (!str) continue;
        if (i > 0) fprintf(stderr, " ");
        if (i == 0 && prefix && prefix[0]) {
            fprintf(stderr, "[%s] %s", prefix, str);
        } else {
            fprintf(stderr, "%s", str);
        }
        JS_FreeCString(ctx, str);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    return JS_UNDEFINED;
}

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, "");
}

static JSValue js_console_info(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, "INFO");
}

static JSValue js_console_warn(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, "WARN");
}

static JSValue js_console_error(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, "ERROR");
}

static JSValue js_console_debug(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, "DEBUG");
}

static JSValue js_console_trace(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    return js_console_log_impl(ctx, this_val, argc, argv, "TRACE");
}

void polyfill_inject_console(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, js_console_log, "log", 0));
    JS_SetPropertyStr(ctx, console, "info",
        JS_NewCFunction(ctx, js_console_info, "info", 0));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunction(ctx, js_console_warn, "warn", 0));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunction(ctx, js_console_error, "error", 0));
    JS_SetPropertyStr(ctx, console, "debug",
        JS_NewCFunction(ctx, js_console_debug, "debug", 0));
    JS_SetPropertyStr(ctx, console, "trace",
        JS_NewCFunction(ctx, js_console_trace, "trace", 0));

    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}
