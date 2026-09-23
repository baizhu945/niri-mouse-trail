#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "log.h"
#include "trail.h"
#include "wlr-layer-shell-client-protocol.h"
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <libevdev/libevdev.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <linux/input.h>
#include <limits.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>

FILE *g_log_file = NULL;
int g_log_level = 1;

#define MAX_OUTPUTS 8
#define BUFFER_SLOTS 3

typedef struct {
    struct wl_buffer *buffer;
    void *data;
    int width, height, stride;
    int busy;
} buffer_slot_t;

typedef struct {
    struct wl_output *wl_output;
    uint32_t registry_name;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    int global_x, global_y;
    int width, height;       /* logical surface dimensions */
    int cursor_width, cursor_height; /* niri cursor layout dimensions */
    int phys_w, phys_h;      /* physical pixel dimensions from mode */
    double scale;
    int configured;
    int removed;            /* marked when compositor removes this output */
    buffer_slot_t buffers[BUFFER_SLOTS];
    int has_committed;
    int damage_x, damage_y, damage_w, damage_h; /* previous visible bounds */
} output_t;

static struct wl_display *display = NULL;
static struct wl_registry *registry = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct zwlr_layer_shell_v1 *layer_shell = NULL;
static struct wl_seat *seat = NULL;
static struct wl_pointer *pointer = NULL;

static output_t outputs[MAX_OUTPUTS];
static int num_outputs = 0;

static double captured_cursor_x = 0, captured_cursor_y = 0;
static int cursor_captured = 0;
static struct wl_surface *current_pointer_surface = NULL;

static trail_state_t trail;
static uint64_t start_time_ms = 0;

#define MAX_MICE 8
static struct libevdev *evdev[MAX_MICE];
static int input_fd[MAX_MICE];
static int is_abs[MAX_MICE];
static double abs_last_x[MAX_MICE];
static double abs_last_y[MAX_MICE];
static int abs_has_x[MAX_MICE], abs_has_y[MAX_MICE];
static double abs_pending_dx[MAX_MICE];
static double abs_pending_dy[MAX_MICE];
static int num_mice = 0;
static struct libevdev *kbd_evdev[MAX_MICE];
static int kbd_fd[MAX_MICE];
static int num_kbd = 0;
static pthread_t input_thread, kbd_thread;
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;

static int ctrl_fd = -1;
static int owns_control_socket = 0;
#define MAX_CLIENTS 16
#define MAX_CONTROL_MESSAGE 255
typedef struct {
    int fd;
    size_t used;
    uint64_t connected_ms;
    char message[MAX_CONTROL_MESSAGE + 1];
} control_client_t;
static control_client_t clients[MAX_CLIENTS];
static int timer_fd = -1;
static _Atomic int running = 1;
static _Atomic int need_redraw = 0;
static _Atomic int restart_requested = 0;
static int center_region_set = 0;
static int outputs_locked = 0;   /* set after initial setup, triggers restart on new outputs */

static int color_cycle_on = 0;
static double cycle_speed = 5.0;
static int trail_style_comet = 1;   /* 1=comet line, 0=dots */

static int point_in_output(const output_t *out, double x, double y) {
    int width = out->cursor_width > 0 ? out->cursor_width : out->width;
    int height = out->cursor_height > 0 ? out->cursor_height : out->height;
    if (out->removed || !out->configured || out->width <= 0 || out->height <= 0)
        return 0;
    return x >= (double)out->global_x &&
           x < (double)out->global_x + (double)width &&
           y >= (double)out->global_y &&
           y < (double)out->global_y + (double)height;
}

static int point_in_any_output(double x, double y) {
    for (int i = 0; i < num_outputs; i++)
        if (point_in_output(&outputs[i], x, y)) return 1;
    return 0;
}

/* Keep an edge-clamped estimate infinitesimally inside its output. The
 * compositor permits moving back from an edge, while a strict half-open
 * rectangle would otherwise make the next inward event look invalid too. */
static int nudge_position_inside_outputs(double *x, double *y) {
    if (point_in_any_output(*x, *y)) return 1;
    for (int i = 0; i < num_outputs; i++) {
        output_t *out = &outputs[i];
        int width = out->cursor_width > 0 ? out->cursor_width : out->width;
        int height = out->cursor_height > 0 ? out->cursor_height : out->height;
        double left = out->global_x, right = left + width;
        double top = out->global_y, bottom = top + height;
        if (out->removed || !out->configured || width <= 0 || height <= 0 ||
            *x < left || *x > right || *y < top || *y > bottom)
            continue;
        if (*x >= right) *x = nextafter(right, left);
        if (*y >= bottom) *y = nextafter(bottom, top);
        if (point_in_any_output(*x, *y)) return 1;
    }
    return 0;
}

static int clip_axis(double origin, double delta, double min, double max,
                     double *lo, double *hi) {
    if (fabs(delta) < 1e-12)
        return origin >= min && origin < max;

    double a = (min - origin) / delta;
    double b = (max - origin) / delta;
    if (a > b) { double tmp = a; a = b; b = tmp; }
    if (a > *lo) *lo = a;
    if (b < *hi) *hi = b;
    return *lo <= *hi + 1e-12;
}

/* Return the furthest point along a motion segment that remains in the
 * connected union of output rectangles. A single bounding box is wrong for
 * monitors with different heights: it permits motion through the gap below
 * the shorter monitor. */
static double reachable_output_fraction(double x0, double y0,
                                         double x1, double y1) {
    if (!point_in_any_output(x0, y0)) return 0.0;

    double dx = x1 - x0, dy = y1 - y0;
    double reachable = 0.0;
    for (int pass = 0; pass < MAX_OUTPUTS; pass++) {
        double next = reachable;
        for (int i = 0; i < num_outputs; i++) {
            output_t *out = &outputs[i];
            if (out->removed || !out->configured || out->width <= 0 || out->height <= 0)
                continue;
            int cursor_width = out->cursor_width > 0 ? out->cursor_width : out->width;
            int cursor_height = out->cursor_height > 0 ? out->cursor_height : out->height;
            double lo = 0.0, hi = 1.0;
            if (!clip_axis(x0, dx, out->global_x, out->global_x + cursor_width, &lo, &hi) ||
                !clip_axis(y0, dy, out->global_y, out->global_y + cursor_height, &lo, &hi))
                continue;
            if (lo <= reachable + 1e-9 && hi > next) next = hi;
        }
        if (next <= reachable + 1e-9) break;
        reachable = next;
    }
    if (reachable < 0.0) return 0.0;
    if (reachable > 1.0) return 1.0;
    return reachable;
}

static void apply_cursor_delta_locked(double dx, double dy, uint64_t now) {
    if (dx == 0.0 && dy == 0.0) return;

    double x0 = trail.pos_x, y0 = trail.pos_y;
    if (!nudge_position_inside_outputs(&x0, &y0)) return;
    trail.pos_x = x0;
    trail.pos_y = y0;
    double fraction = reachable_output_fraction(x0, y0, x0 + dx, y0 + dy);
    double nx = x0 + dx * fraction;
    double ny = y0 + dy * fraction;
    if (!point_in_any_output(nx, ny)) {
        double backoff = 1e-6 / fmax(1.0, fmax(fabs(dx), fabs(dy)));
        fraction = fraction > backoff ? fraction - backoff : 0.0;
        nx = x0 + dx * fraction;
        ny = y0 + dy * fraction;
        if (!nudge_position_inside_outputs(&nx, &ny)) return;
    }
    if (fabs(nx - x0) < 1e-12 && fabs(ny - y0) < 1e-12) return;

    /* Visual smoothing/threshold affect samples, never the raw cursor estimate. */
    if (trail_feed(&trail, nx - x0, ny - y0, now))
        atomic_store(&need_redraw, 1);
}

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static double hue2rgb(double p, double q, double t) {
    if (t < 0.0) t += 1.0; else if (t > 1.0) t -= 1.0;
    if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0/2.0) return q;
    if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
    return p;
}
static void hsl_to_rgb(double h, double s, double l, double *r, double *g, double *b) {
    if (s == 0.0) { *r = *g = *b = l; return; }
    double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    double p = 2.0 * l - q;
    *r = hue2rgb(p, q, h + 1.0/3.0); *g = hue2rgb(p, q, h); *b = hue2rgb(p, q, h - 1.0/3.0);
}

static void output_geometry(void *data, struct wl_output *wo,
    int32_t x, int32_t y, int32_t pw, int32_t ph,
    int32_t subpixel, const char *make, const char *model, int32_t transform) {
    (void)data;(void)pw;(void)ph;(void)subpixel;(void)make;(void)model;(void)transform;
    for (int i = 0; i < num_outputs; i++)
        if (outputs[i].wl_output == wo) { outputs[i].global_x = x; outputs[i].global_y = y; return; }
}
static void output_mode(void *data, struct wl_output *wo,
    uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    (void)data;(void)refresh;
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    for (int i = 0; i < num_outputs; i++)
        if (outputs[i].wl_output == wo) {             outputs[i].phys_w = w; outputs[i].phys_h = h;
            LOG_INFO("Output %d mode: %dx%d", i, w, h); return; }
}
static void output_scale(void *data, struct wl_output *wo, int32_t factor) {
    for (int i = 0; i < num_outputs; i++)
        if (outputs[i].wl_output == wo) { outputs[i].scale = (double)factor; return; }
    (void)data;
}
static void output_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void output_name(void *d, struct wl_output *o, const char *n) { (void)d;(void)o;(void)n; }
static void output_desc(void *d, struct wl_output *o, const char *s) { (void)d;(void)o;(void)s; }
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode, .done = output_done,
    .scale = output_scale, .name = output_name, .description = output_desc,
};

static void ptr_enter(void *data, struct wl_pointer *p,
    uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)data;(void)p;(void)serial;
    current_pointer_surface = surface;
    double psx = wl_fixed_to_double(sx), psy = wl_fixed_to_double(sy);
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].surface == surface) {
            captured_cursor_x = outputs[i].global_x + psx;
            captured_cursor_y = outputs[i].global_y + psy;
            cursor_captured = 1;
            pthread_mutex_lock(&input_mutex);
            trail_set_position(&trail, captured_cursor_x, captured_cursor_y);
            need_redraw = 1;
            pthread_mutex_unlock(&input_mutex);
            LOG_INFO("Recalibrated via enter: output=%d global=(%.0f,%.0f)", i, captured_cursor_x, captured_cursor_y);
            return;
        }
    }
}
static void ptr_leave(void *d,struct wl_pointer *p,uint32_t s,struct wl_surface *sf){
    (void)d;(void)p;(void)s;
    if (current_pointer_surface == sf) current_pointer_surface = NULL;
}
static void ptr_motion(void *d,struct wl_pointer *p,uint32_t t,wl_fixed_t sx,wl_fixed_t sy){
    (void)d;(void)p;(void)t;
    if (!current_pointer_surface) return;
    double psx = wl_fixed_to_double(sx), psy = wl_fixed_to_double(sy);
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].surface == current_pointer_surface) {
            captured_cursor_x = outputs[i].global_x + psx;
            captured_cursor_y = outputs[i].global_y + psy;
            cursor_captured = 1;
            pthread_mutex_lock(&input_mutex);
            trail_set_position(&trail, captured_cursor_x, captured_cursor_y);
            need_redraw = 1;
            pthread_mutex_unlock(&input_mutex);
            LOG_DEBUG("Recalibrated via motion: global=(%.0f,%.0f)", captured_cursor_x, captured_cursor_y);
            return;
        }
    }
}
static void ptr_button(void *d,struct wl_pointer *p,uint32_t s,uint32_t t,uint32_t b,uint32_t st){(void)d;(void)p;(void)s;(void)t;(void)b;(void)st;}
static void ptr_axis(void *d,struct wl_pointer *p,uint32_t t,uint32_t a,wl_fixed_t v){(void)d;(void)p;(void)t;(void)a;(void)v;}
static void ptr_frame(void *d,struct wl_pointer *p){(void)d;(void)p;}
static const struct wl_pointer_listener pointer_listener = {
    .enter=ptr_enter,.leave=ptr_leave,.motion=ptr_motion,.button=ptr_button,.axis=ptr_axis,.frame=ptr_frame,
};

/* Workers only request a restart. The main thread performs the complete
 * Wayland/input cleanup and then execs the original command line. Forking a
 * multithreaded process and calling stdio/exec helpers in the child can
 * deadlock, and execing without closing descriptors leaks every input fd. */
static void request_restart(const char *reason) {
    if (atomic_exchange(&restart_requested, 1)) {
        atomic_store(&running, 0);
        return;
    }
    LOG_INFO("Restart requested: %s", reason);
    atomic_store(&running, 0);
}

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
    const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        if (version >= 4) compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
        else LOG_ERROR("wl_compositor v4 required (server offers v%u)", version);
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        if (version >= 2) layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 2);
        else LOG_ERROR("layer-shell v2 required (server offers v%u)", version);
    }
    else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (num_outputs < MAX_OUTPUTS) {
            if (outputs_locked && running) {
                request_restart("new output detected");
                return;
            }
            if (version < 2) { LOG_ERROR("wl_output v2 required (server offers v%u)", version); return; }
            struct wl_output *o = wl_registry_bind(reg, name, &wl_output_interface, version < 3 ? version : 3);
            wl_output_add_listener(o, &output_listener, NULL);
            memset(&outputs[num_outputs], 0, sizeof(output_t));
            outputs[num_outputs].wl_output = o;
            outputs[num_outputs].registry_name = name;
            outputs[num_outputs].scale = 1.0;
            num_outputs++;
        }
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (!seat) {
            if (version < 2) { LOG_ERROR("wl_seat v2 required (server offers v%u)", version); return; }
            seat = wl_registry_bind(reg, name, &wl_seat_interface, 2);
            pointer = wl_seat_get_pointer(seat);
        }
    }
}
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data;(void)reg;
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].wl_output && outputs[i].registry_name == name) {
            outputs[i].removed = 1;
            LOG_INFO("Output %d removed", i);
            return;
        }
    }
}
static const struct wl_registry_listener registry_listener = { .global=registry_global, .global_remove=registry_global_remove };

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *s,
    uint32_t serial, uint32_t w, uint32_t h) {
    (void)data; zwlr_layer_surface_v1_ack_configure(s, serial);
    for (int i = 0; i < num_outputs; i++)
        if (outputs[i].layer_surface == s) {
            if (outputs[i].width != (int)w || outputs[i].height != (int)h) {
                outputs[i].has_committed = 0;
                outputs[i].damage_w = outputs[i].damage_h = 0;
                atomic_store(&need_redraw, 1);
            }
            outputs[i].width=(int)w; outputs[i].height=(int)h; outputs[i].configured=1;
            outputs[i].cursor_width=(int)w; outputs[i].cursor_height=(int)h;
            if (outputs[i].phys_w > 0 && w > 0)
                outputs[i].scale = (double)outputs[i].phys_w / (double)w;
            if (outputs[i].phys_w > 0 && outputs[i].phys_h > 0 && outputs[i].scale > 0.0) {
                /* niri's cursor layout uses the floor of physical/scale,
                 * while layer-shell may round the surface size upward. */
                outputs[i].cursor_width = (int)floor(outputs[i].phys_w / outputs[i].scale + 1e-6);
                outputs[i].cursor_height = (int)floor(outputs[i].phys_h / outputs[i].scale + 1e-6);
            }
            LOG_INFO("Output %d: logical=%dx%d phys=%dx%d scale=%.2f",
                     i, outputs[i].cursor_width, outputs[i].cursor_height,
                     outputs[i].phys_w, outputs[i].phys_h, outputs[i].scale);
            return;
        }
}
static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *s) {
    (void)data;
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].layer_surface == s) {
            outputs[i].removed = 1;
            outputs[i].configured = 0;
            zwlr_layer_surface_v1_destroy(s);
            outputs[i].layer_surface = NULL;
            LOG_INFO("Layer surface %d closed, destroyed", i);
            return;
        }
    }
    zwlr_layer_surface_v1_destroy(s);
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = { .configure=layer_surface_configure, .closed=layer_surface_closed };

static void buffer_released(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    ((buffer_slot_t *)data)->busy = 0;
}
static const struct wl_buffer_listener buffer_listener = { .release = buffer_released };

static void destroy_buffer_slot(buffer_slot_t *slot) {
    if (slot->buffer) wl_buffer_destroy(slot->buffer);
    if (slot->data) munmap(slot->data, (size_t)slot->stride * slot->height);
    memset(slot, 0, sizeof(*slot));
}

static int create_buffer_slot(buffer_slot_t *slot, int w, int h) {
    if (w <= 0 || h <= 0 || w > INT32_MAX / 4 || h > INT32_MAX / (w * 4))
        return 0;
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("mouse-trail", MFD_CLOEXEC);
    if (fd < 0) return 0;
    if (ftruncate(fd, size) < 0) { close(fd); return 0; }
    void *data = mmap(NULL, (size_t)size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return 0; }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    close(fd);
    if (!pool) { munmap(data, (size_t)size); return 0; }
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!buffer) { munmap(data, (size_t)size); return 0; }
    slot->buffer = buffer;
    slot->data = data;
    slot->width = w; slot->height = h; slot->stride = stride;
    wl_buffer_add_listener(buffer, &buffer_listener, slot);
    return 1;
}

static buffer_slot_t *acquire_buffer_slot(output_t *out) {
    for (int i = 0; i < BUFFER_SLOTS; i++) {
        buffer_slot_t *slot = &out->buffers[i];
        if (slot->busy) continue; /* compositor still reading the old contents */
        if (slot->buffer && (slot->width != out->width || slot->height != out->height))
            destroy_buffer_slot(slot);
        if (!slot->buffer && !create_buffer_slot(slot, out->width, out->height))
            return NULL;
        return slot;
    }
    return NULL;
}

typedef struct { cairo_t *cr; double px, py; int has; } comet_ctx_t;

static void draw_trail_point(void *user, double x, double y, double radius,
    double alpha, double r, double g, double b) {
    comet_ctx_t *ctx = (comet_ctx_t*)user;
    if (trail_style_comet) {
        if (!ctx->has) {
            /* A short motion may yield only one sample: still draw its head. */
            cairo_save(ctx->cr);
            cairo_set_source_rgba(ctx->cr, r, g, b, alpha);
            cairo_arc(ctx->cr, x, y, radius, 0.0, 2.0 * M_PI);
            cairo_fill(ctx->cr);
            cairo_restore(ctx->cr);
            ctx->px = x; ctx->py = y; ctx->has = 1;
            return;
        }
        cairo_save(ctx->cr);
        cairo_set_source_rgba(ctx->cr, r, g, b, alpha);
        cairo_set_line_width(ctx->cr, radius * 2.0);
        cairo_set_line_cap(ctx->cr, CAIRO_LINE_CAP_BUTT);
        cairo_set_line_join(ctx->cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(ctx->cr, ctx->px, ctx->py);
        cairo_line_to(ctx->cr, x, y);
        cairo_stroke(ctx->cr);
        cairo_restore(ctx->cr);
        ctx->px = x; ctx->py = y;
    } else {
        cairo_save(ctx->cr);
        cairo_set_source_rgba(ctx->cr, r, g, b, alpha);
        cairo_arc(ctx->cr, x, y, radius, 0.0, 2.0*M_PI);
        cairo_fill(ctx->cr);
        cairo_restore(ctx->cr);
    }
}

static void render_output(output_t *out, const trail_state_t *state) {
    if (out->removed || out->width <= 0 || out->height <= 0 || !out->configured) return;
    uint64_t now = get_time_ms();
    int left = out->width, top = out->height, right = 0, bottom = 0;
    int has_fresh = 0;
    if (state->visible) for (int i = 0; i <= state->count; i++) {
        const trail_point_t *pt = NULL;
        if (i < state->count) {
            pt = &state->points[(state->head + i) % MAX_TRAIL_POINTS];
            if (now - pt->timestamp_ms > state->max_age_ms) continue;
            has_fresh = 1;
        } else if (!has_fresh) break;
        /* Include the actual cursor head as well as the smoothed tail. */
        double x = (pt ? pt->x : state->pos_x) - out->global_x;
        double y = (pt ? pt->y : state->pos_y) - out->global_y;
        double margin = state->max_radius + 2.0;
        /* Include off-output endpoints: a comet segment can cross this output. */
        int x0 = (int)fmax(0, fmin(out->width, floor(x - margin)));
        int y0 = (int)fmax(0, fmin(out->height, floor(y - margin)));
        int x1 = (int)fmax(0, fmin(out->width, ceil(x + margin)));
        int y1 = (int)fmax(0, fmin(out->height, ceil(y + margin)));
        if (x0 < left) left = x0;
        if (y0 < top) top = y0;
        if (x1 > right) right = x1;
        if (y1 > bottom) bottom = y1;
    }
    int visible = right > left && bottom > top;
    if (out->has_committed && !visible && !out->damage_w) return;
    int dx = 0, dy = 0, dw = out->width, dh = out->height;
    if (out->has_committed) {
        dx = visible ? left : out->damage_x;
        dy = visible ? top : out->damage_y;
        int far_x = visible ? right : out->damage_x + out->damage_w;
        int far_y = visible ? bottom : out->damage_y + out->damage_h;
        if (out->damage_w) {
            dx = dx < out->damage_x ? dx : out->damage_x;
            dy = dy < out->damage_y ? dy : out->damage_y;
            if (out->damage_x + out->damage_w > far_x) far_x = out->damage_x + out->damage_w;
            if (out->damage_y + out->damage_h > far_y) far_y = out->damage_y + out->damage_h;
        }
        dw = far_x - dx; dh = far_y - dy;
    }
    buffer_slot_t *slot = acquire_buffer_slot(out);
    if (!slot) { atomic_store(&need_redraw, 1); return; }
    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        slot->data, CAIRO_FORMAT_ARGB32, out->width, out->height, slot->stride);
    cairo_t *cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR); cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, -(double)out->global_x, -(double)out->global_y);
    comet_ctx_t ctx = { cr, 0, 0, 0 };
    int rendered = trail_render(state, now, draw_trail_point, &ctx);
    if (rendered > 0) {
        /* Smooth only the tail. Keep the head on the real cursor while giving
         * it the same age-based fade as the newest sample after a stop. */
        int latest = (state->head + state->count - 1) % MAX_TRAIL_POINTS;
        double age = fmin(1.0, (double)(now - state->points[latest].timestamp_ms) /
                               (double)state->max_age_ms);
        double alpha = state->a * (1.0 - age * age * age);
        double radius = state->max_radius * (1.0 - age) * (1.0 - age);
        if (trail_style_comet && ctx.has) {
            cairo_set_source_rgba(cr, state->r, state->g, state->b, alpha);
            cairo_set_line_width(cr, radius * 2.0);
            cairo_move_to(cr, ctx.px, ctx.py);
            cairo_line_to(cr, state->pos_x, state->pos_y);
            cairo_stroke(cr);
        }
        cairo_set_source_rgba(cr, state->r, state->g, state->b, alpha);
        cairo_arc(cr, state->pos_x, state->pos_y, radius, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    cairo_destroy(cr); cairo_surface_destroy(cs);
    wl_surface_attach(out->surface, slot->buffer, 0, 0);
    wl_surface_damage_buffer(out->surface, dx, dy, dw, dh);
    slot->busy = 1;
    wl_surface_commit(out->surface);
    out->has_committed = 1;
    out->damage_x = visible ? left : 0;
    out->damage_y = visible ? top : 0;
    out->damage_w = visible ? right - left : 0;
    out->damage_h = visible ? bottom - top : 0;
}

static void render_all(void) {
    trail_state_t snapshot;
    pthread_mutex_lock(&input_mutex);
    snapshot = trail;
    pthread_mutex_unlock(&input_mutex);
    for (int i = 0; i < num_outputs; i++) render_output(&outputs[i], &snapshot);
}

static int parse_number(const char *s, double min, double max, double *value) {
    if (!s || !*s) return 0;
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno || !isfinite(v) || v < min || v > max) return 0;
    *value = v;
    return 1;
}

static int parse_duration(const char *s, uint64_t *value) {
    if (!s || !*s || *s == '-') return 0;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end || errno || v == 0 || v > UINT32_MAX) return 0;
    *value = (uint64_t)v;
    return 1;
}

static int parse_color(const char *text, double *r, double *g, double *b) {
    const char *s = text && *text == '#' ? text + 1 : text;
    if (!s || strlen(s) != 6) return 0;
    for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    unsigned int ri, gi, bi;
    if (sscanf(s, "%2x%2x%2x", &ri, &gi, &bi) != 3) return 0;
    *r = ri / 255.0; *g = gi / 255.0; *b = bi / 255.0;
    return 1;
}

static int handle_control_msg(const char *msg) {
    double value, r, g, b;
    uint64_t duration;
    if (strncmp(msg, "color ", 6) == 0) {
        if (!parse_color(msg + 6, &r, &g, &b)) return 0;
        trail_set_color_rgb(&trail, r, g, b);
    } else if (strcmp(msg, "color-cycle on") == 0) color_cycle_on = 1;
    else if (strcmp(msg, "color-cycle off") == 0) color_cycle_on = 0;
    else if (strncmp(msg, "width ", 6) == 0) {
        if (!parse_number(msg + 6, 0.01, 1000.0, &value)) return 0;
        trail.max_radius = value;
    } else if (strncmp(msg, "speed ", 6) == 0) {
        if (!parse_duration(msg + 6, &duration)) return 0;
        trail.max_age_ms = duration;
    } else if (strncmp(msg, "alpha ", 6) == 0) {
        if (!parse_number(msg + 6, 0.0, 1.0, &value)) return 0;
        trail.a = value;
    } else if (strcmp(msg, "show") == 0) {
        trail.visible = true;
        trail.count = 0; /* no history from the hidden interval */
        trail.last_sample_x = trail.visual_x;
        trail.last_sample_y = trail.visual_y;
    } else if (strcmp(msg, "hide") == 0) {
        trail.visible = false;
        trail.count = 0;
    } else if (strcmp(msg, "warp") == 0) request_restart("manual warp command");
    else return 0;
    atomic_store(&need_redraw, 1);
    return 1;
}

static int setup_control_socket(const char *path) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        LOG_ERROR("Control socket path is too long: %s", path);
        return -1;
    }
    strcpy(addr.sun_path, path);
    ctrl_fd = socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
    if (ctrl_fd < 0) return -1;
    mode_t old_umask = umask(0077);
    int rc = bind(ctrl_fd, (struct sockaddr *)&addr, sizeof(addr));
    int saved_errno = errno;
    umask(old_umask);
    if (rc < 0 && saved_errno == EADDRINUSE) {
        struct stat st;
        int probe = socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
        if (probe < 0) goto fail;
        int connected = connect(probe, (struct sockaddr *)&addr, sizeof(addr));
        int probe_errno = errno;
        close(probe);
        if (connected == 0 || probe_errno == EINPROGRESS || probe_errno == EAGAIN) {
            LOG_ERROR("Another mouse-trail owns %s", path);
            goto fail;
        }
        if (probe_errno != ECONNREFUSED && probe_errno != ENOENT) {
            LOG_ERROR("Unable to verify whether control socket is stale: %s", path);
            goto fail;
        }
        if (lstat(path, &st) < 0 || !S_ISSOCK(st.st_mode) || st.st_uid != geteuid() ||
            unlink(path) < 0) {
            LOG_ERROR("Refusing to remove existing control path: %s", path);
            goto fail;
        }
        old_umask = umask(0077);
        rc = bind(ctrl_fd, (struct sockaddr *)&addr, sizeof(addr));
        saved_errno = errno;
        umask(old_umask);
    }
    if (rc < 0 || listen(ctrl_fd, MAX_CLIENTS) < 0) {
        LOG_ERROR("Control socket setup failed: %s", strerror(rc < 0 ? saved_errno : errno));
        if (rc == 0) unlink(path);
        goto fail;
    }
    owns_control_socket = 1;
    return 0;
fail:
    close(ctrl_fd); ctrl_fd = -1;
    return -1;
}

static void close_control_client(int epfd, control_client_t *client) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, client->fd, NULL);
    close(client->fd);
    client->fd = -1;
    client->used = 0;
}

static void finish_control_client(int epfd, control_client_t *client, int complete) {
    int ok = 0;
    if (complete && client->used) {
        client->message[client->used] = '\0';
        char *newline = strchr(client->message, '\n');
        if (newline) *newline = '\0';
        ok = handle_control_msg(client->message);
    }
    const char *response = ok ? "OK\n" : "ERR\n";
    (void)send(client->fd, response, strlen(response), MSG_NOSIGNAL | MSG_DONTWAIT);
    close_control_client(epfd, client);
}

static void accept_control_clients(int epfd) {
    for (;;) {
        int fd = accept4(ctrl_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                LOG_WARN("Control accept failed: %s", strerror(errno));
            return;
        }
        int index = 0;
        while (index < MAX_CLIENTS && clients[index].fd >= 0) index++;
        if (index == MAX_CLIENTS) { close(fd); continue; }
        control_client_t *client = &clients[index];
        client->fd = fd;
        client->used = 0;
        client->connected_ms = get_time_ms();
        struct epoll_event event = { .events = EPOLLIN | EPOLLRDHUP,
                                     .data.fd = fd };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) < 0)
            close_control_client(epfd, client);
    }
}

static void read_control_client(int epfd, control_client_t *client) {
    for (;;) {
        ssize_t n = recv(client->fd, client->message + client->used,
                         MAX_CONTROL_MESSAGE - client->used, 0);
        if (n > 0) {
            client->used += (size_t)n;
            client->connected_ms = get_time_ms();
            if (memchr(client->message, '\n', client->used)) {
                finish_control_client(epfd, client, 1);
                return;
            }
            if (client->used == MAX_CONTROL_MESSAGE) {
                finish_control_client(epfd, client, 0);
                return;
            }
        } else if (n == 0) {
            finish_control_client(epfd, client, 1);
            return;
        } else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        else { close_control_client(epfd, client); return; }
    }
}

static void process_input_event(int m, const struct input_event *ev) {
    if (ev->type == EV_REL && !is_abs[m] &&
        (ev->code == REL_X || ev->code == REL_Y)) {
        double dx = (ev->code == REL_X) ? (double)ev->value : 0.0;
        double dy = (ev->code == REL_Y) ? (double)ev->value : 0.0;
        pthread_mutex_lock(&input_mutex);
        apply_cursor_delta_locked(dx, dy, get_time_ms());
        pthread_mutex_unlock(&input_mutex);
    } else if (ev->type == EV_ABS && is_abs[m] &&
               (ev->code == ABS_X || ev->code == ABS_Y)) {
        double *last = (ev->code == ABS_X) ? &abs_last_x[m] : &abs_last_y[m];
        double *pending = (ev->code == ABS_X) ? &abs_pending_dx[m] : &abs_pending_dy[m];
        double cur = (double)ev->value;
        int *has_pos = (ev->code == ABS_X) ? &abs_has_x[m] : &abs_has_y[m];
        if (!*has_pos) { *last = cur; *has_pos = 1; }
        else if (cur != *last) {
            *pending += cur - *last;
            *last = cur;
        }
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT && is_abs[m]) {
        /* Apply accumulated ABS deltas on SYN_REPORT, scaled to logical px. */
        double dx = abs_pending_dx[m], dy = abs_pending_dy[m];
        abs_pending_dx[m] = 0;
        abs_pending_dy[m] = 0;
        /* Each ABS axis can report independently; the first event of each
         * establishes its own baseline, with no cross-axis dependency. */
        int ax = libevdev_get_abs_maximum(evdev[m], ABS_X) -
                 libevdev_get_abs_minimum(evdev[m], ABS_X);
        int ay = libevdev_get_abs_maximum(evdev[m], ABS_Y) -
                 libevdev_get_abs_minimum(evdev[m], ABS_Y);
        if (ax > 0 && outputs[0].width > 0)
            dx = dx / (double)ax * (double)outputs[0].width;
        if (ay > 0 && outputs[0].height > 0)
            dy = dy / (double)ay * (double)outputs[0].height;
        pthread_mutex_lock(&input_mutex);
        apply_cursor_delta_locked(dx, dy, get_time_ms());
        pthread_mutex_unlock(&input_mutex);
    } else if (ev->type == EV_KEY && is_abs[m] &&
               ev->code == BTN_TOUCH && ev->value == 0) {
        abs_has_x[m] = abs_has_y[m] = 0;
        abs_pending_dx[m] = 0;
        abs_pending_dy[m] = 0;
    }
}

static void drain_mouse_device(int m) {
    struct input_event ev;
    for (;;) {
        int rc = libevdev_next_event(evdev[m], LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
            process_input_event(m, &ev);
            continue;
        }
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            LOG_WARN("Input queue overrun on device #%d; resynchronizing", m);
            do {
                rc = libevdev_next_event(evdev[m], LIBEVDEV_READ_FLAG_SYNC, &ev);
                if (rc == LIBEVDEV_READ_STATUS_SYNC) process_input_event(m, &ev);
            } while (rc == LIBEVDEV_READ_STATUS_SYNC);
            if (rc == LIBEVDEV_READ_STATUS_SUCCESS) continue;
        }
        if (rc != -EAGAIN && rc != -EINTR && rc != -ENODEV)
            LOG_WARN("Input device #%d stopped: %s", m, strerror(-rc));
        return;
    }
}

static void *input_thread_fn(void *arg) {
    (void)arg;
    struct pollfd fds[MAX_MICE];
    for (int m = 0; m < num_mice; m++) {
        fds[m].fd = input_fd[m];
        fds[m].events = POLLIN;
    }
    while (atomic_load(&running)) {
        int ready = poll(fds, num_mice, 50);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int m = 0; m < num_mice; m++) {
            if (fds[m].revents & (POLLIN | POLLERR | POLLHUP))
                drain_mouse_device(m);
        }
    }
    return NULL;
}

/* Key name to Linux key code mapping */
typedef struct { const char *name; int code; } key_map_t;

static int lookup_keycode(const char *name) {
    static const key_map_t keys[] = {
        {"left", KEY_LEFT}, {"right", KEY_RIGHT}, {"up", KEY_UP}, {"down", KEY_DOWN},
        {"h", KEY_H}, {"j", KEY_J}, {"k", KEY_K}, {"l", KEY_L},
        {"a", KEY_A}, {"b", KEY_B}, {"c", KEY_C}, {"d", KEY_D}, {"e", KEY_E},
        {"f", KEY_F}, {"g", KEY_G}, {"i", KEY_I}, {"m", KEY_M}, {"n", KEY_N},
        {"o", KEY_O}, {"p", KEY_P}, {"q", KEY_Q}, {"r", KEY_R}, {"s", KEY_S},
        {"t", KEY_T}, {"u", KEY_U}, {"v", KEY_V}, {"w", KEY_W}, {"x", KEY_X},
        {"y", KEY_Y}, {"z", KEY_Z},
        {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4}, {"5", KEY_5},
        {"6", KEY_6}, {"7", KEY_7}, {"8", KEY_8}, {"9", KEY_9}, {"0", KEY_0},
        {"comma", KEY_COMMA}, {"period", KEY_DOT}, {"semicolon", KEY_SEMICOLON},
        {"bracketleft", KEY_LEFTBRACE}, {"bracketright", KEY_RIGHTBRACE},
        {"tab", KEY_TAB}, {"space", KEY_SPACE}, {"escape", KEY_ESC},
        {"return", KEY_ENTER}, {"enter", KEY_ENTER},
        {NULL, 0}
    };
    if (!name) return -1;
    char lower[32]; int i;
    for (i = 0; name[i] && i < 31; i++) lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
    lower[i] = '\0';
    for (const key_map_t *k = keys; k->name; k++)
        if (strcmp(k->name, lower) == 0) return k->code;
    return -1;
}

/* Detected warp binding */
typedef struct {
    int key_code;        /* e.g., KEY_LEFT */
    int need_super;      /* Mod/Mod4/Super required */
    int need_shift;
    int need_ctrl;
    int need_alt;
} warp_binding_t;

#define MAX_WARP_BINDINGS 8
static warp_binding_t warp_bindings[MAX_WARP_BINDINGS];
static int num_warp_bindings = 0;

/* Parse compositor config and extract monitor-switch keybindings */
static void detect_warp_bindings(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];

    /* Try each compositor config */
    const char *configs[] = {
        "/.config/niri/config.kdl",
        "/.config/sway/config",
        "/.config/hypr/hyprland.conf",
        "/.config/hypr/config",
        NULL
    };

    for (int ci = 0; configs[ci]; ci++) {
        snprintf(path, sizeof(path), "%s%s", home, configs[ci]);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[512];
        int is_niri = strstr(configs[ci], "niri") != NULL;
        int is_sway = strstr(configs[ci], "sway") != NULL;
        int is_hypr = strstr(configs[ci], "hypr") != NULL;

        LOG_INFO("Scanning %s for warp bindings", configs[ci] + 1);

        while (fgets(line, sizeof(line), f) && num_warp_bindings < MAX_WARP_BINDINGS) {
            /* Check for known monitor-switch action keywords */
            if (strstr(line, "focus-monitor-left") || strstr(line, "focus-monitor-right") ||
                strstr(line, "focus-monitor-up") || strstr(line, "focus-monitor-down") ||
                strstr(line, "focus output left") || strstr(line, "focus output right") ||
                strstr(line, "movefocus, monitor") ||
                strstr(line, "move-column-to-monitor-left") || strstr(line, "move-column-to-monitor-right") ||
                strstr(line, "move-column-to-monitor-up") || strstr(line, "move-column-to-monitor-down") ||
                strstr(line, "move workspace to output left") || strstr(line, "move workspace to output right")) {

                warp_binding_t wb;
                memset(&wb, 0, sizeof(wb));
                wb.key_code = -1;

                /* Extract modifiers and key from the line */
                char *pline = line;
                while (*pline == ' ' || *pline == '\t') pline++;

                if (is_niri) {
                    /* Niri format: Mod+Shift+Left { focus-monitor-left; } */
                    wb.need_super = (strstr(pline, "Mod") || strstr(pline, "Super"));
                    wb.need_shift = strstr(pline, "Shift") != NULL;
                    wb.need_ctrl  = strstr(pline, "Ctrl") != NULL;
                    wb.need_alt   = strstr(pline, "Alt") != NULL;
                    /* Extract the last key before { or ): */
                    char *last = strrchr(pline, '+');
                    if (last) {
                        char keyname[32] = {0};
                        char *ks = last + 1, *kd = keyname;
                        while (*ks && *ks != ' ' && *ks != '\t' && *ks != '{' && *ks != ')' &&
                               kd < keyname + sizeof(keyname) - 1) *kd++ = *ks++;
                        wb.key_code = lookup_keycode(keyname);
                    }
                } else if (is_sway) {
                    /* Sway format: bindsym Mod4+Shift+Left focus output left */
                    wb.need_super = (strstr(pline, "Mod4") || strstr(pline, "Super"));
                    wb.need_shift = strstr(pline, "Shift") != NULL;
                    wb.need_ctrl  = strstr(pline, "Ctrl") || strstr(pline, "Control");
                    wb.need_alt   = strstr(pline, "Mod1") || strstr(pline, "Alt");
                    char *last = strrchr(pline, '+');
                    if (last) {
                        char keyname[32] = {0}, *ks = last + 1, *kd = keyname;
                        while (*ks && *ks != ' ' && *ks != '\t' &&
                               kd < keyname + sizeof(keyname) - 1) *kd++ = *ks++;
                        wb.key_code = lookup_keycode(keyname);
                    }
                } else if (is_hypr) {
                    /* Hyprland format: bind = SUPER SHIFT, left, movefocus, monitor, -1 */
                    char *eq = strchr(pline, '=');
                    if (eq) {
                        char *mods = eq + 1;
                        while (*mods == ' ') mods++;
                        wb.need_super = (strstr(mods, "SUPER") || strstr(mods, "super"));
                        wb.need_shift = strstr(mods, "SHIFT") || strstr(mods, "shift");
                        wb.need_ctrl  = strstr(mods, "CTRL")  || strstr(mods, "ctrl");
                        wb.need_alt   = strstr(mods, "ALT")   || strstr(mods, "alt");
                        /* Key is after comma */
                        char *comma = strchr(mods, ',');
                        if (comma) {
                            char keyname[32] = {0}, *ks = comma + 1, *kd = keyname;
                            while (*ks == ' ') ks++;
                            while (*ks && *ks != ' ' && *ks != ',' && *ks != '\t' &&
                                   kd < keyname + sizeof(keyname) - 1) *kd++ = *ks++;
                            wb.key_code = lookup_keycode(keyname);
                        }
                    }
                }

                if (wb.key_code >= 0) {
                    /* Avoid duplicates */
                    int dup = 0;
                    for (int i = 0; i < num_warp_bindings; i++)
                        if (memcmp(&warp_bindings[i], &wb, sizeof(wb)) == 0) { dup = 1; break; }
                    if (!dup) {
                        warp_bindings[num_warp_bindings++] = wb;
                        LOG_INFO("  Detected binding: super=%d shift=%d ctrl=%d alt=%d key=%d",
                                 wb.need_super, wb.need_shift, wb.need_ctrl, wb.need_alt, wb.key_code);
                    }
                }
            }
        }
        fclose(f);
        if (num_warp_bindings > 0) break; /* Use first config found */
    }

    /* Fallback: hardcoded defaults */
    if (num_warp_bindings == 0) {
        warp_bindings[0].key_code = KEY_LEFT;  warp_bindings[0].need_super = 1; warp_bindings[0].need_shift = 1;
        warp_bindings[1].key_code = KEY_RIGHT; warp_bindings[1].need_super = 1; warp_bindings[1].need_shift = 1;
        num_warp_bindings = 2;
        LOG_INFO("No config found, using default bindings (Super+Shift+Left/Right)");
    }
}

static void process_key_event(const struct input_event *ev, int *super_down,
                               int *shift_down, int *ctrl_down, int *alt_down) {
    if (ev->type != EV_KEY) return;
    int pressed = ev->value == 1;
    int released = ev->value == 0;
    switch (ev->code) {
        case KEY_LEFTMETA: case KEY_RIGHTMETA:
            if (pressed) *super_down = 1; else if (released) *super_down = 0;
            break;
        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
            if (pressed) *shift_down = 1; else if (released) *shift_down = 0;
            break;
        case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
            if (pressed) *ctrl_down = 1; else if (released) *ctrl_down = 0;
            break;
        case KEY_LEFTALT: case KEY_RIGHTALT:
            if (pressed) *alt_down = 1; else if (released) *alt_down = 0;
            break;
        default:
            if (!pressed) break;
            for (int i = 0; i < num_warp_bindings; i++) {
                warp_binding_t *wb = &warp_bindings[i];
                if (ev->code == wb->key_code &&
                    *super_down == wb->need_super &&
                    *shift_down == wb->need_shift &&
                    *ctrl_down  == wb->need_ctrl &&
                    *alt_down   == wb->need_alt) {
                    request_restart("monitor-switch hotkey");
                    return;
                }
            }
            break;
    }
}

static void drain_keyboard_device(int k, int *super_down, int *shift_down,
                                  int *ctrl_down, int *alt_down) {
    struct input_event ev;
    for (;;) {
        int rc = libevdev_next_event(kbd_evdev[k], LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
            process_key_event(&ev, super_down, shift_down, ctrl_down, alt_down);
            continue;
        }
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            LOG_WARN("Keyboard queue overrun on device #%d; resynchronizing", k);
            do {
                rc = libevdev_next_event(kbd_evdev[k], LIBEVDEV_READ_FLAG_SYNC, &ev);
                if (rc == LIBEVDEV_READ_STATUS_SYNC)
                    process_key_event(&ev, super_down, shift_down, ctrl_down, alt_down);
            } while (rc == LIBEVDEV_READ_STATUS_SYNC);
            if (rc == LIBEVDEV_READ_STATUS_SUCCESS) continue;
        }
        if (rc != -EAGAIN && rc != -EINTR && rc != -ENODEV)
            LOG_WARN("Keyboard device #%d stopped: %s", k, strerror(-rc));
        return;
    }
}

/* Keyboard monitor: detect monitor-switch hotkeys and trigger recapture. */
static void *kbd_thread_fn(void *arg) {
    (void)arg;
    int super_down = 0, shift_down = 0, ctrl_down = 0, alt_down = 0;
    struct pollfd fds[MAX_MICE];
    for (int k = 0; k < num_kbd; k++) {
        fds[k].fd = kbd_fd[k];
        fds[k].events = POLLIN;
    }
    while (atomic_load(&running)) {
        int ready = poll(fds, num_kbd, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int k = 0; k < num_kbd; k++) {
            if (fds[k].revents & (POLLIN | POLLERR | POLLHUP))
                drain_keyboard_device(k, &super_down, &shift_down, &ctrl_down, &alt_down);
        }
    }
    return NULL;
}

static int send_control_cmd(const char *sock, const char *cmd) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    size_t len = strlen(cmd);
    if (strlen(sock) >= sizeof(addr.sun_path) || len == 0 || len >= MAX_CONTROL_MESSAGE)
        return 1;
    strcpy(addr.sun_path, sock);
    int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;
    struct timeval timeout = { .tv_sec = 2 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return 1; }
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, cmd + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return 1; }
        sent += (size_t)n;
    }
    if (shutdown(fd, SHUT_WR) < 0) { close(fd); return 1; }
    char reply[4] = {0};
    size_t received = 0;
    while (received < 3) {
        ssize_t n = recv(fd, reply + received, 3 - received, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        received += (size_t)n;
    }
    close(fd);
    return (received == 3 && strcmp(reply, "OK\n") == 0) ? 0 : 1;
}

/* Config file parser */
#define MAX_CONFIG_INCLUDES 8
static int config_include_count = 0;

static void parse_config(const char *path,
    double *cr, double *cg, double *cb, double *ca,
    double *width, uint64_t *length_ms, double *min_speed, double *smooth_factor,
    int *color_cycle_on, double *cycle_speed, int *trail_style_comet,
    const char **device, const char **kbd_device) {
    if (config_include_count >= MAX_CONFIG_INCLUDES) return;
    config_include_count++;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *nl = strchr(p, '\n'); if (nl) *nl = '\0';
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p, *val = eq + 1;
        while (*key && (key[strlen(key)-1]==' '||key[strlen(key)-1]=='\t')) key[strlen(key)-1]='\0';
        while (*val == ' ' || *val == '\t') val++;
        size_t vlen = strlen(val);
        while (vlen && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t' || val[vlen - 1] == '\r'))
            val[--vlen] = '\0';
        double number;
        uint64_t duration;
        int valid = 1;
        if (strcmp(key, "import") == 0) {
            parse_config(val, cr, cg, cb, ca, width, length_ms, min_speed,
                         smooth_factor, color_cycle_on, cycle_speed, trail_style_comet,
                         device, kbd_device);
        } else if (strcmp(key, "color") == 0) valid = parse_color(val, cr, cg, cb);
        else if (strcmp(key, "alpha") == 0) { valid = parse_number(val, 0.0, 1.0, &number); if (valid) *ca = number; }
        else if (strcmp(key, "width") == 0) { valid = parse_number(val, 0.01, 1000.0, &number); if (valid) *width = number; }
        else if (strcmp(key, "length") == 0) { valid = parse_duration(val, &duration); if (valid) *length_ms = duration; }
        else if (strcmp(key, "min_speed") == 0) { valid = parse_number(val, 0.0, 1000.0, &number); if (valid) *min_speed = number; }
        else if (strcmp(key, "smooth_factor") == 0) { valid = parse_number(val, 0.0, 1.0, &number); if (valid) *smooth_factor = number; }
        else if (strcmp(key, "color_cycle") == 0) { valid = strcmp(val, "on") == 0 || strcmp(val, "off") == 0; if (valid) *color_cycle_on = strcmp(val, "on") == 0; }
        else if (strcmp(key, "cycle_speed") == 0) { valid = parse_number(val, 0.01, 86400.0, &number); if (valid) *cycle_speed = number; }
        else if (strcmp(key, "device") == 0) { if (*val) *device = strdup(val); else valid = 0; }
        else if (strcmp(key, "kbd_device") == 0) { if (*val) *kbd_device = strdup(val); else valid = 0; }
        else if (strcmp(key, "trail_style") == 0) { valid = strcmp(val, "comet") == 0 || strcmp(val, "dots") == 0; if (valid) *trail_style_comet = strcmp(val, "comet") == 0; }
        if (!valid) fprintf(stderr, "mouse-trail: invalid %s in %s\n", key, path);
    }
    fclose(f);
}

static void usage(const char *p) {
        fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  --config PATH       Config file (default: ~/.config/mouse-trail/config)\n"
        "  --device PATH       Input device (default: auto-detect)\n"
        "  --kbd-device PATH   Keyboard for hotkey detection (default: auto-detect)\n"
        "  --color RRGGBB     Trail color (default: ffffff)\n  --alpha N       Opacity 0-1\n"
        "  --width N           Head radius px\n  --length N    Duration ms\n"
        "  --min-speed N       Stationary threshold px\n  --smooth-factor N EMA 0-1\n"
        "  --color-cycle on|off\n  --cycle-speed N  Cycle period s\n"
        "  --socket PATH       Control socket path\n"
        "  --log-level debug|info|warn|error\n  --log-file PATH\n"
        "  --ctl \"CMD\"         Send command\n  --help\n", p);
    }

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    const char *device_path = NULL;
    const char *kbd_device_path = NULL;
    double cr=1.0,cg=1.0,cb=1.0,ca=1.0, width=8.0;
    uint64_t length_ms=500; double min_speed=2.0, smooth_factor=0.6;
    int log_level=1; const char *log_path=NULL, *socket_path=NULL, *ctl_cmd=NULL;
    const char *config_path = NULL;

    /* Locate the config before applying CLI overrides, regardless of option order. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (strncmp(argv[i], "--", 2) == 0 && i + 1 < argc &&
                 strcmp(argv[i], "--config") != 0) i++;
    }
    if (!config_path) {
        const char *home = getenv("HOME");
        static char def_cfg[512];
        if (home) snprintf(def_cfg, sizeof(def_cfg), "%s/.config/mouse-trail/config", home);
        else snprintf(def_cfg, sizeof(def_cfg), "/tmp/mouse-trail-config");
        config_path = def_cfg;
    }
    parse_config(config_path, &cr, &cg, &cb, &ca, &width, &length_ms,
                 &min_speed, &smooth_factor, &color_cycle_on, &cycle_speed,
                 &trail_style_comet, &device_path, &kbd_device_path);

    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i];
        if (strcmp(opt, "--config") == 0 && i + 1 < argc) { i++; continue; }
        if (i + 1 >= argc) { fprintf(stderr, "Missing value for %s\n", opt); return 1; }
        const char *val = argv[++i];
        int valid = 1;
        if (strcmp(opt, "--device") == 0) device_path = val;
        else if (strcmp(opt, "--kbd-device") == 0) kbd_device_path = val;
        else if (strcmp(opt, "--color") == 0) valid = parse_color(val, &cr, &cg, &cb);
        else if (strcmp(opt, "--alpha") == 0) valid = parse_number(val, 0.0, 1.0, &ca);
        else if (strcmp(opt, "--width") == 0) valid = parse_number(val, 0.01, 1000.0, &width);
        else if (strcmp(opt, "--length") == 0) valid = parse_duration(val, &length_ms);
        else if (strcmp(opt, "--min-speed") == 0) valid = parse_number(val, 0.0, 1000.0, &min_speed);
        else if (strcmp(opt, "--smooth-factor") == 0) valid = parse_number(val, 0.0, 1.0, &smooth_factor);
        else if (strcmp(opt, "--color-cycle") == 0) {
            valid = strcmp(val, "on") == 0 || strcmp(val, "off") == 0;
            if (valid) color_cycle_on = strcmp(val, "on") == 0;
        } else if (strcmp(opt, "--cycle-speed") == 0) valid = parse_number(val, 0.01, 86400.0, &cycle_speed);
        else if (strcmp(opt, "--socket") == 0) socket_path = val;
        else if (strcmp(opt, "--log-level") == 0) {
            if (strcmp(val, "debug") == 0) log_level = 0;
            else if (strcmp(val, "info") == 0) log_level = 1;
            else if (strcmp(val, "warn") == 0) log_level = 2;
            else if (strcmp(val, "error") == 0) log_level = 3;
            else valid = 0;
        } else if (strcmp(opt, "--log-file") == 0) log_path = val;
        else if (strcmp(opt, "--ctl") == 0) ctl_cmd = val;
        else valid = 0;
        if (!valid) { fprintf(stderr, "Invalid option: %s %s\n", opt, val); return 1; }
    }

    /* Default keyboard device — NULL means auto-detect */
    /* (no default, NULL triggers auto-detect below) */

    if (log_path && strcmp(log_path,"-")!=0 && ctl_cmd==NULL) { FILE *f=fopen(log_path,"a"); if(f)log_init(f,log_level); else{log_init(stderr,log_level);LOG_ERROR("Cannot open: %s",log_path);} }
    else log_init(stderr, log_level);

    if (!socket_path) { const char *xdg=getenv("XDG_RUNTIME_DIR"); static char sbuf[256]; snprintf(sbuf,sizeof(sbuf),"%s/mouse-trail.sock",xdg?xdg:"/tmp"); socket_path=sbuf; }
    if (ctl_cmd) return send_control_cmd(socket_path, ctl_cmd);

    LOG_INFO("mouse-trail v0.11");

    /* Open mouse devices — try configured path, then auto-detect all matching */
    {
        /* Try configured device first */
        if (device_path) {
            int fd = open(device_path, O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if (fd >= 0 && libevdev_new_from_fd(fd, &evdev[0]) == 0) {
                num_mice = 1;
                input_fd[0] = fd;
                int has_rel = libevdev_has_event_type(evdev[0], EV_REL) &&
                              libevdev_has_event_code(evdev[0], EV_REL, REL_X) &&
                              libevdev_has_event_code(evdev[0], EV_REL, REL_Y);
                is_abs[0] = !has_rel &&
                            libevdev_has_event_type(evdev[0], EV_ABS) &&
                            libevdev_has_event_code(evdev[0], EV_ABS, ABS_X) &&
                            libevdev_has_event_code(evdev[0], EV_ABS, ABS_Y);
                abs_has_x[0] = abs_has_y[0] = 0;
                abs_last_x[0]=0; abs_last_y[0]=0;
                abs_pending_dx[0]=0; abs_pending_dy[0]=0;
                LOG_INFO("Configured: %s (%s)", libevdev_get_name(evdev[0]), device_path);
            } else {
                if (fd >= 0) close(fd);
            }
        }

        /* Always scan for additional/all devices */
        char trypath[32];
        for (int en = 0; en < 32 && num_mice < MAX_MICE; en++) {
                snprintf(trypath, sizeof(trypath), "/dev/input/event%d", en);
                int tfd = open(trypath, O_RDONLY|O_NONBLOCK|O_CLOEXEC);
                if (tfd < 0) continue;
                struct stat candidate;
                if (fstat(tfd, &candidate) < 0) { close(tfd); continue; }
                int already_open = 0;
                for (int m = 0; m < num_mice; m++) {
                    struct stat existing;
                    if (fstat(input_fd[m], &existing) == 0 &&
                        candidate.st_rdev == existing.st_rdev &&
                        candidate.st_dev == existing.st_dev) {
                        already_open = 1; break;
                    }
                }
                if (already_open) { close(tfd); continue; }
                struct libevdev *tdev = NULL;
                if (libevdev_new_from_fd(tfd, &tdev) == 0) {
                    int is_mouse = 0, is_absdev = 0;
                    if (libevdev_has_event_type(tdev, EV_REL) &&
                        libevdev_has_event_code(tdev, EV_REL, REL_X) &&
                        libevdev_has_event_code(tdev, EV_REL, REL_Y) &&
                        libevdev_has_event_type(tdev, EV_KEY) &&
                        libevdev_has_event_code(tdev, EV_KEY, BTN_LEFT))
                        is_mouse = 1;
                    if (libevdev_has_event_type(tdev, EV_ABS) &&
                        libevdev_has_event_code(tdev, EV_ABS, ABS_X) &&
                        libevdev_has_event_code(tdev, EV_ABS, ABS_Y))
                        is_absdev = 1;
                    /* A mixed REL+ABS node is treated as REL. This avoids
                     * integrating two coordinate streams from composite HID
                     * devices; pure ABS nodes remain a fallback for touchpads. */
                    if (is_mouse && is_absdev) is_absdev = 0;
                    if (is_mouse || is_absdev) {
                        /* Dedup pure ABS siblings when a REL interface exists. */
                        const char *phys = libevdev_get_phys(tdev);
                        if (phys && is_mouse) {
                            int has_abs = 0;
                            for (int m = 0; m < num_mice; m++)
                                if (evdev[m] && libevdev_get_phys(evdev[m]) &&
                                    strcmp(libevdev_get_phys(evdev[m]), phys) == 0 &&
                                    is_abs[m]) { has_abs = 1; break; }
                            if (has_abs) { libevdev_free(tdev); close(tfd); continue; }
                        }
                        if (phys && is_absdev) {
                            int has_rel = 0, rel_idx = -1;
                            for (int m = 0; m < num_mice; m++)
                                if (evdev[m] && libevdev_get_phys(evdev[m]) &&
                                    strcmp(libevdev_get_phys(evdev[m]), phys) == 0 &&
                                    !is_abs[m]) { has_rel = 1; rel_idx = m; break; }
                            if (has_rel) {
                                /* Replace silent REL with ABS */
                                libevdev_free(evdev[rel_idx]); close(input_fd[rel_idx]);
                                for (int k = rel_idx; k < num_mice - 1; k++) {
                                    evdev[k] = evdev[k+1]; input_fd[k] = input_fd[k+1];
                                    is_abs[k] = is_abs[k+1];
                                    abs_last_x[k] = abs_last_x[k+1]; abs_last_y[k] = abs_last_y[k+1];
                                    abs_has_x[k] = abs_has_x[k+1];
                                    abs_has_y[k] = abs_has_y[k+1];
                                    abs_pending_dx[k]=abs_pending_dx[k+1]; abs_pending_dy[k]=abs_pending_dy[k+1];
                                }
                                num_mice--;
                            }
                        }
                        evdev[num_mice] = tdev;
                        input_fd[num_mice] = tfd;
                        is_abs[num_mice] = is_absdev;
                        abs_last_x[num_mice] = 0;
                        abs_last_y[num_mice] = 0;
                        abs_has_x[num_mice] = abs_has_y[num_mice] = 0;
                        abs_pending_dx[num_mice] = 0;
                        abs_pending_dy[num_mice] = 0;
                        LOG_INFO("Auto-detected %s #%d: %s (%s)",
                                 is_mouse ? "mouse" : "touchpad",
                                 num_mice, libevdev_get_name(tdev), trypath);
                        num_mice++;
                        continue;
                    }
                    libevdev_free(tdev);
                }
                close(tfd);
            }
        }
        if (num_mice == 0) { LOG_ERROR("No mouse found"); return 1; }

    trail_init(&trail, width, length_ms, min_speed, smooth_factor, cr, cg, cb, ca);

    /* Open keyboard devices — config + auto-detect */
    {
        /* Try configured device first */
        if (kbd_device_path) {
            int fd = open(kbd_device_path, O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if (fd >= 0 && libevdev_new_from_fd(fd, &kbd_evdev[0]) == 0) {
                num_kbd = 1;
                kbd_fd[0] = fd;
                LOG_INFO("Keyboard: %s (%s)", libevdev_get_name(kbd_evdev[0]), kbd_device_path);
            } else {
                if (fd >= 0) close(fd);
            }
        }

        /* Auto-detect all keyboard devices */
        char trypath[32];
        for (int en = 0; en < 32 && num_kbd < MAX_MICE; en++) {
            snprintf(trypath, sizeof(trypath), "/dev/input/event%d", en);
            if (kbd_device_path && strcmp(trypath, kbd_device_path) == 0) continue;
            int tfd = open(trypath, O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if (tfd < 0) continue;
            struct libevdev *tdev = NULL;
            if (libevdev_new_from_fd(tfd, &tdev) == 0) {
                if (libevdev_has_event_type(tdev, EV_KEY) &&
                    libevdev_has_event_code(tdev, EV_KEY, KEY_A) &&
                    libevdev_has_event_code(tdev, EV_KEY, KEY_ESC)) {
                    kbd_evdev[num_kbd] = tdev;
                    kbd_fd[num_kbd] = tfd;
                    LOG_INFO("Auto-detected keyboard #%d: %s (%s)", num_kbd, libevdev_get_name(tdev), trypath);
                    num_kbd++;
                    continue;
                }
                libevdev_free(tdev);
            }
            close(tfd);
        }
        if (num_kbd == 0) LOG_WARN("No keyboard found, warp hotkey detection disabled");
    }

    display = wl_display_connect(NULL);
    if (!display) { LOG_ERROR("Wayland connect failed"); for(int m=0;m<num_mice;m++){if(evdev[m])libevdev_free(evdev[m]);if(input_fd[m]>=0)close(input_fd[m]);} return 1; }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor||!shm||!layer_shell) { LOG_ERROR("Missing globals"); return 1; }
    if (num_outputs==0) { LOG_ERROR("No outputs"); return 1; }

    if (pointer) wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    wl_display_roundtrip(display); /* output geometry/mode/scale */

    /* Refuse a second instance before it can map a full-input overlay. */
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
    if (setup_control_socket(socket_path) < 0) return 1;

    for(int i=0;i<num_outputs;i++){ output_t*o=&outputs[i];
        o->surface=wl_compositor_create_surface(compositor);
        o->layer_surface=zwlr_layer_shell_v1_get_layer_surface(layer_shell,o->surface,o->wl_output,ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,"mouse-trail");
        zwlr_layer_surface_v1_add_listener(o->layer_surface,&layer_surface_listener,NULL);
        zwlr_layer_surface_v1_set_anchor(o->layer_surface,ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM|ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface,-1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer_surface,0);
        wl_surface_commit(o->surface); }
    wl_display_roundtrip(display); wl_display_roundtrip(display);

    double est_x = outputs[0].global_x + outputs[0].width / 2.0;
    double est_y = outputs[0].global_y + outputs[0].height / 2.0;

    /* Map transparent frames using the same release-managed buffers as rendering. */
    for (int i = 0; i < num_outputs; i++)
        render_output(&outputs[i], &trail);
    wl_display_roundtrip(display);

    /* Give the compositor a few immediate chances to deliver enter. If the
     * cursor is hidden, capture may arrive much later; the main loop below
     * intentionally keeps the full input region until then. */
    for (int retry = 0; retry < 8 && pointer && !cursor_captured; retry++) {
        usleep(30000);
        wl_display_roundtrip(display);
        LOG_INFO("Capture retry %d: captured=%d", retry, cursor_captured);
    }

    if (cursor_captured) { est_x=captured_cursor_x; est_y=captured_cursor_y; }
    else {
        est_x = outputs[0].global_x + outputs[0].width / 2.0;
        est_y = outputs[0].global_y + outputs[0].height / 2.0;
        LOG_INFO("Cursor not captured yet; waiting indefinitely with full input region");
    }
    trail_set_position(&trail, est_x, est_y);

    LOG_INFO("Position: (%.0f,%.0f), %d outputs%s", est_x, est_y, num_outputs,
             cursor_captured ? " (captured)" : " (initial estimate; capture pending)");

    start_time_ms = get_time_ms();
    pthread_create(&input_thread, NULL, input_thread_fn, NULL);
    if (num_kbd > 0) {
        detect_warp_bindings();
        pthread_create(&kbd_thread, NULL, kbd_thread_fn, NULL);
    }

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
    struct itimerspec its = {{0,16666667},{0,1}};
    timerfd_settime(timer_fd, 0, &its, NULL);
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event evt; evt.events=EPOLLIN;
    evt.data.fd=timer_fd; epoll_ctl(epfd,EPOLL_CTL_ADD,timer_fd,&evt);
    if(ctrl_fd>=0){ evt.data.fd=ctrl_fd; epoll_ctl(epfd,EPOLL_CTL_ADD,ctrl_fd,&evt); }
    int display_fd = wl_display_get_fd(display);
    evt.data.fd = display_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, display_fd, &evt);

    LOG_INFO("Main loop");
    outputs_locked = 1;

    while (atomic_load(&running)) {
        /* Keep the full input region until wl_pointer gives us an absolute
         * position. There is deliberately no timeout/fallback here: a hidden
         * cursor can make the initial enter arrive only after later motion. */
        if (!center_region_set && cursor_captured) {
            for(int i=0;i<num_outputs;i++){
                if (outputs[i].removed) continue;
                struct wl_region *r = wl_compositor_create_region(compositor);
                int cx = outputs[i].width / 2, cy = outputs[i].height / 2;
                /* Closed ring: 200x200 hollow square, 2px thick */
                wl_region_add(r, cx - 100, cy - 101, 200, 2);   /* top */
                wl_region_add(r, cx - 100, cy + 99,  200, 2);   /* bottom */
                wl_region_add(r, cx - 101, cy - 99,  2,   198); /* left */
                wl_region_add(r, cx + 99,  cy - 99,  2,   198); /* right */
                wl_surface_set_input_region(outputs[i].surface, r);
                wl_region_destroy(r);
                wl_surface_commit(outputs[i].surface);
            }
            center_region_set = 1;
            LOG_INFO("Ring calibration region active (200x200 hollow, 2px)");
        }

        int prepared = 0;
        while (atomic_load(&running) && wl_display_prepare_read(display)!=0) {
            if (wl_display_dispatch_pending(display) < 0)
                atomic_store(&running, 0);
        }
        if (!atomic_load(&running)) break;
        prepared = 1;
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            atomic_store(&running, 0);
            break;
        }
        struct epoll_event events[32];
        int n = epoll_wait(epfd, events, 32, 10);
        if (n < 0 && errno == EINTR) {
            if (prepared) wl_display_cancel_read(display);
            continue;
        }
        if (n < 0) {
            if (prepared) wl_display_cancel_read(display);
            atomic_store(&running, 0);
            break;
        }
        int display_ready = 0;
        for (int i = 0; i < n; i++)
            if (events[i].data.fd == display_fd &&
                (events[i].events & (EPOLLIN|EPOLLERR|EPOLLHUP)))
                display_ready = 1;
        if (display_ready) {
            if (wl_display_read_events(display) < 0)
                atomic_store(&running, 0);
        } else {
            wl_display_cancel_read(display);
        }
        if (!atomic_load(&running)) break;
        if (wl_display_dispatch_pending(display) < 0) {
            atomic_store(&running, 0);
            break;
        }

        for (int i=0;i<n;i++) {
            if (events[i].data.fd == timer_fd) {
                uint64_t exp; if (read(timer_fd, &exp, sizeof(exp)) < 0) {}
                uint64_t now = get_time_ms();

                pthread_mutex_lock(&input_mutex);
                if (color_cycle_on) {
                    double t = fmod((double)(now-start_time_ms)/1000.0/cycle_speed, 1.0);
                    hsl_to_rgb(t,1.0,0.5,&trail.r,&trail.g,&trail.b);
                }
                int had_points = trail.count > 0;
                int alive = trail_cleanup(&trail, now);
                int redraw = atomic_exchange(&need_redraw, 0) || (had_points && !alive);
                pthread_mutex_unlock(&input_mutex);

                if (alive > 0 || redraw) render_all();
                for (int c = 0; c < MAX_CLIENTS; c++)
                    if (clients[c].fd >= 0 && now - clients[c].connected_ms > 2000)
                        finish_control_client(epfd, &clients[c], 0);
            } else if (ctrl_fd >= 0 && events[i].data.fd == ctrl_fd) {
                accept_control_clients(epfd);
            } else {
                for (int c = 0; c < MAX_CLIENTS; c++)
                    if (clients[c].fd == events[i].data.fd) {
                        read_control_client(epfd, &clients[c]);
                        break;
                    }
            }
        }
    }

    int should_restart = atomic_load(&restart_requested);
    LOG_INFO("Shutting down%s", should_restart ? " for restart" : "");
    /* Never block on a roundtrip while shutting down a stalled compositor. */
    pthread_cancel(input_thread); pthread_join(input_thread, NULL);
    if (num_kbd > 0) { pthread_cancel(kbd_thread); pthread_join(kbd_thread, NULL); }
    for (int i = 0; i < num_outputs; i++) {
        for (int j = 0; j < BUFFER_SLOTS; j++) destroy_buffer_slot(&outputs[i].buffers[j]);
        if (outputs[i].layer_surface) zwlr_layer_surface_v1_destroy(outputs[i].layer_surface);
        if (outputs[i].surface) wl_surface_destroy(outputs[i].surface);
        if (outputs[i].wl_output) wl_output_destroy(outputs[i].wl_output);
    }
    if(pointer) wl_pointer_destroy(pointer);
    if(seat) wl_seat_destroy(seat);
    if(compositor) wl_compositor_destroy(compositor);
    if(shm) wl_shm_destroy(shm);
    if(layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if(registry) wl_registry_destroy(registry);
    if(display) wl_display_disconnect(display);
    for(int m=0; m<num_mice; m++){ if(evdev[m]) libevdev_free(evdev[m]); if(input_fd[m] >= 0) close(input_fd[m]); }
    for (int k = 0; k < num_kbd; k++) { if (kbd_evdev[k]) libevdev_free(kbd_evdev[k]); if (kbd_fd[k] >= 0) close(kbd_fd[k]); }
    for (int c = 0; c < MAX_CLIENTS; c++)
        if (clients[c].fd >= 0) close_control_client(epfd, &clients[c]);
    if (ctrl_fd >= 0) { close(ctrl_fd); if (owns_control_socket) unlink(socket_path); }
    if(timer_fd >= 0) close(timer_fd);
    if(epfd >= 0) close(epfd);
    if(g_log_file && g_log_file != stderr) fclose(g_log_file);
    if (should_restart) {
        execvp(argv[0], argv);
        fprintf(stderr, "mouse-trail: restart failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
