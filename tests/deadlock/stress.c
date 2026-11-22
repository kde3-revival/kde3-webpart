/* Headless stress test for the releaseUnusedResources() self-deadlock:
 * constant critical memory pressure while tiles with promise images change.
 *   stress PAGE SECONDS      exit 0 = responsive, 3 = hung, 4 = web process died */
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>
#include <stdio.h>
#include <stdlib.h>

static WebKitWebView *view;
static GMainLoop *loop;
static gint64 start, last;
static int seconds, replies, frames, rc;
static gboolean pending, loaded;

static gboolean quit(gpointer d) { (void)d; g_main_loop_quit(loop); return G_SOURCE_REMOVE; }

static void pong(GObject *o, GAsyncResult *r, gpointer d)
{
    (void)d;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(o), r, NULL);
    pending = FALSE;
    if (!v) return;
    last = g_get_monotonic_time();
    frames = jsc_value_to_int32(v);
    replies++;
    g_object_unref(v);
}

static gboolean tick(gpointer d)
{
    (void)d;
    gint64 now = g_get_monotonic_time();
    if (!loaded) {
        if (now - start > 15000000) { puts("NOT LOADED"); rc = 5; g_main_loop_quit(loop); }
        return G_SOURCE_CONTINUE;
    }
    if (pending && now - last > 8000000) {
        printf("HUNG after %.1f s, replies=%d frames=%d\n", (last - start) / 1e6, replies, frames);
        fflush(stdout);
        rc = 3;
        g_timeout_add_seconds(12, quit, NULL); /* time to take the stacks */
        return G_SOURCE_REMOVE;
    }
    if (now - start > seconds * G_GINT64_CONSTANT(1000000)) {
        printf("OK %d s, replies=%d frames=%d\n", seconds, replies, frames);
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }
    if (!pending) {
        pending = TRUE;
        webkit_web_view_evaluate_javascript(view, "window.frames_||0", -1, NULL, NULL, NULL, pong, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static void changed(WebKitWebView *v, WebKitLoadEvent e, gpointer d)
{
    (void)v; (void)d;
    if (e == WEBKIT_LOAD_FINISHED) { loaded = TRUE; last = g_get_monotonic_time(); }
}

static void died(WebKitWebView *v, WebKitWebProcessTerminationReason r, gpointer d)
{
    (void)v; (void)d;
    printf("DIED reason=%d\n", (int)r);
    rc = 4;
    g_main_loop_quit(loop);
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    gchar *html;
    if (!g_file_get_contents(argv[1], &html, NULL, NULL)) return 2;
    seconds = atoi(argv[2]);
    GError *err = NULL;
    WPEDisplay *display = wpe_display_headless_new();
    if (!wpe_display_connect(display, &err)) { printf("display: %s\n", err->message); return 2; }
    WebKitMemoryPressureSettings *mp = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(mp, 32);
    webkit_memory_pressure_settings_set_poll_interval(mp, g_getenv("POLL") ? g_ascii_strtod(g_getenv("POLL"), NULL) : 0.03);
    WebKitWebContext *ctx = g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", mp, NULL);
    view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", ctx, "display", display, NULL);
    WPEView *wv = webkit_web_view_get_wpe_view(view);
    WPEToplevel *tl = wpe_view_get_toplevel(wv);
    if (tl) wpe_toplevel_resize(tl, 1000, 700);
    wpe_view_map(wv);
    g_signal_connect(view, "load-changed", G_CALLBACK(changed), NULL);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(died), NULL);
    webkit_web_view_load_html(view, html, "http://localhost/");
    loop = g_main_loop_new(NULL, FALSE);
    start = last = g_get_monotonic_time();
    g_timeout_add(250, tick, NULL);
    g_main_loop_run(loop);
    return rc;
}
