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
#include <errno.h>
#include <pthread.h>
#include <linux/input.h>

FILE *g_log_file = NULL;
int g_log_level = 1;

#define MAX_OUTPUTS 8

typedef struct {
    struct wl_output *wl_output;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    int global_x, global_y;
    int width, height;
    int configured;
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

/* Cursor capture */
static double captured_cursor_x = 0, captured_cursor_y = 0;
static int cursor_captured = 0;

static trail_state_t trail;
static uint64_t start_time_ms = 0;

static struct libevdev *evdev = NULL;
static int input_fd = -1;
static pthread_t input_thread;
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static double pending_rel_x = 0, pending_rel_y = 0;

static int ctrl_fd = -1;
static int timer_fd = -1;
static int running = 1;
static int need_redraw = 0;

static int color_cycle_on = 0;
static double cycle_speed = 5.0;

static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static double hue2rgb(double p, double q, double t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0/2.0) return q;
    if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
    return p;
}

static void hsl_to_rgb(double h, double s, double l, double *r, double *g, double *b) {
    if (s == 0.0) { *r = *g = *b = l; return; }
    double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    double p = 2.0 * l - q;
    *r = hue2rgb(p, q, h + 1.0/3.0);
    *g = hue2rgb(p, q, h);
    *b = hue2rgb(p, q, h - 1.0/3.0);
}

static void output_geometry(void *data, struct wl_output *wl_output,
                             int32_t x, int32_t y,
                             int32_t pw, int32_t ph,
                             int32_t subpixel, const char *make,
                             const char *model, int32_t transform) {
    (void)data; (void)pw; (void)ph; (void)subpixel; (void)make; (void)model; (void)transform;
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].wl_output == wl_output) {
            outputs[i].global_x = x;
            outputs[i].global_y = y;
            LOG_INFO("Output %d geometry: x=%d y=%d", i, x, y);
            return;
        }
    }
}

static void output_mode(void *data, struct wl_output *wl_output,
                         uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)data; (void)refresh;
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].wl_output == wl_output) {
            outputs[i].width = width;
            outputs[i].height = height;
            LOG_INFO("Output %d mode: %dx%d", i, width, height);
            return;
        }
    }
}
static void output_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; (void)f; }
static void output_name(void *d, struct wl_output *o, const char *n) {
    (void)d; (void)o; LOG_INFO("Output name: %s", n);
}
static void output_desc(void *d, struct wl_output *o, const char *s) { (void)d; (void)o; (void)s; }

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_desc,
};

/* wl_pointer listener — capture cursor position from enter or motion */
static void ptr_enter(void *data, struct wl_pointer *p,
                       uint32_t serial, struct wl_surface *surface,
                       wl_fixed_t sx, wl_fixed_t sy) {
    (void)data; (void)p; (void)serial;
    if (cursor_captured) return;
    double psx = wl_fixed_to_double(sx);
    double psy = wl_fixed_to_double(sy);
    LOG_INFO("ptr_enter: surface=%p sx=%.0f sy=%.0f num_outputs=%d",
             (void*)surface, psx, psy, num_outputs);
    for (int i = 0; i < num_outputs; i++) {
        LOG_INFO("  output[%d] surface=%p", i, (void*)outputs[i].surface);
        if (outputs[i].surface == surface) {
            captured_cursor_x = outputs[i].global_x + psx;
            captured_cursor_y = outputs[i].global_y + psy;
            cursor_captured = 1;
            LOG_INFO("Cursor captured via enter: output=%d global=(%.0f,%.0f)",
                     i, captured_cursor_x, captured_cursor_y);
            return;
        }
    }
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf)
    { (void)d; (void)p; (void)s; (void)sf; }

static void ptr_motion(void *data, struct wl_pointer *p,
                        uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    (void)data; (void)p; (void)time;
    if (cursor_captured) return;
    /* Motion gives us the cursor position too — use as fallback */
    double psx = wl_fixed_to_double(sx);
    double psy = wl_fixed_to_double(sy);
    /* We don't know which surface the motion is on, 
       try to match any output that contains this coordinate */
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].configured &&
            psx >= 0 && psx < outputs[i].width &&
            psy >= 0 && psy < outputs[i].height) {
            captured_cursor_x = outputs[i].global_x + psx;
            captured_cursor_y = outputs[i].global_y + psy;
            cursor_captured = 1;
            LOG_INFO("Cursor captured via motion: output=%d global=(%.0f,%.0f)",
                     i, captured_cursor_x, captured_cursor_y);
            return;
        }
    }
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t,
                        uint32_t b, uint32_t st) { (void)d;(void)p;(void)s;(void)t;(void)b;(void)st; }
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
    { (void)d;(void)p;(void)t;(void)a;(void)v; }
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d;(void)p; }

static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                             const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
        LOG_INFO("Bound wl_compositor");
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        LOG_INFO("Bound wl_shm");
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 3);
        LOG_INFO("Bound zwlr_layer_shell_v1");
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (num_outputs < MAX_OUTPUTS) {
            struct wl_output *o = wl_registry_bind(reg, name, &wl_output_interface, 3);
            wl_output_add_listener(o, &output_listener, NULL);
            outputs[num_outputs].wl_output = o;
            outputs[num_outputs].surface = NULL;
            outputs[num_outputs].configured = 0;
            outputs[num_outputs].global_x = 0;
            outputs[num_outputs].global_y = 0;
            outputs[num_outputs].width = 0;
            outputs[num_outputs].height = 0;
            num_outputs++;
            LOG_INFO("Bound wl_output (#%d)", num_outputs - 1);
        }
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (!seat) {
            seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
            pointer = wl_seat_get_pointer(seat);
            LOG_INFO("Bound wl_seat + wl_pointer");
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                     uint32_t serial, uint32_t width, uint32_t height) {
    (void)data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].layer_surface == surface) {
            outputs[i].width = (int)width;
            outputs[i].height = (int)height;
            outputs[i].configured = 1;
            LOG_INFO("Layer surface %d configured: %dx%d", i, width, height);
            return;
        }
    }
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)data;
    zwlr_layer_surface_v1_destroy(surface);
    running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static struct wl_buffer *create_shm_buffer(int width, int height, void **data_out, int *stride_out) {
    int stride = width * 4;
    int size = stride * height;
    int fd = memfd_create("mouse-trail-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) { LOG_ERROR("memfd_create failed: %s", strerror(errno)); return NULL; }
    if (ftruncate(fd, size) < 0) { LOG_ERROR("ftruncate failed: %s", strerror(errno)); close(fd); return NULL; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { LOG_ERROR("mmap failed: %s", strerror(errno)); close(fd); return NULL; }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    *data_out = data;
    *stride_out = stride;
    return buffer;
}

static void draw_trail_point(void *user, double x, double y, double radius,
                              double alpha, double r, double g, double b) {
    cairo_t *cr = (cairo_t *)user;
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, alpha);
    cairo_arc(cr, x, y, radius, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void render_output(output_t *out) {
    if (out->width <= 0 || out->height <= 0 || !out->configured) return;

    void *data; int stride;
    struct wl_buffer *buffer = create_shm_buffer(out->width, out->height, &data, &stride);
    if (!buffer) return;

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        (unsigned char *)data, CAIRO_FORMAT_ARGB32, out->width, out->height, stride);
    cairo_t *cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Center origin at estimated cursor position on this output */
    cairo_translate(cr, trail.pos_x - out->global_x, trail.pos_y - out->global_y);

    trail_render(&trail, get_time_ms(), draw_trail_point, cr);

    cairo_destroy(cr);
    cairo_surface_destroy(cs);
    wl_surface_attach(out->surface, buffer, 0, 0);
    wl_surface_damage_buffer(out->surface, 0, 0, out->width, out->height);
    wl_surface_commit(out->surface);
    munmap(data, stride * out->height);
    wl_buffer_destroy(buffer);
}

static void render_all(void) {
    for (int i = 0; i < num_outputs; i++)
        render_output(&outputs[i]);
}

static void handle_control_msg(const char *msg) {
    LOG_INFO("Control: '%s'", msg);
    if (strncmp(msg, "color ", 6) == 0) {
        unsigned int ri, gi, bi;
        if (sscanf(msg + 6, "#%02x%02x%02x", &ri, &gi, &bi) == 3) {
            trail_set_color_rgb(&trail, ri/255.0, gi/255.0, bi/255.0);
            need_redraw = 1;
        }
    } else if (strcmp(msg, "color-cycle on") == 0) {
        color_cycle_on = 1; need_redraw = 1;
    } else if (strcmp(msg, "color-cycle off") == 0) {
        color_cycle_on = 0; need_redraw = 1;
    } else if (strncmp(msg, "width ", 6) == 0) {
        trail.max_radius = atof(msg+6); need_redraw = 1;
    } else if (strncmp(msg, "speed ", 6) == 0) {
        trail.max_age_ms = (uint64_t)atoi(msg+6); need_redraw = 1;
    } else if (strcmp(msg, "show") == 0) {
        trail.visible = true; need_redraw = 1;
    } else if (strcmp(msg, "hide") == 0) {
        trail.visible = false; need_redraw = 1;
    }
}

static void setup_control_socket(const char *path) {
    unlink(path);
    ctrl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ctrl_fd < 0) { LOG_ERROR("socket: %s", strerror(errno)); return; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(ctrl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind: %s", strerror(errno)); close(ctrl_fd); ctrl_fd = -1; return;
    }
    if (listen(ctrl_fd, 5) < 0) {
        LOG_ERROR("listen: %s", strerror(errno)); close(ctrl_fd); ctrl_fd = -1; return;
    }
    LOG_INFO("Control socket: %s", path);
}

static void *input_thread_fn(void *arg) {
    (void)arg;
    struct input_event ev;
    while (running) {
        int rc = libevdev_next_event(evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
            if (ev.type == EV_REL) {
                pthread_mutex_lock(&input_mutex);
                if (ev.code == REL_X) pending_rel_x += ev.value;
                if (ev.code == REL_Y) pending_rel_y += ev.value;
                pthread_mutex_unlock(&input_mutex);
            }
        } else if (rc == -EAGAIN) {
            usleep(500);
        } else if (rc < 0 && rc != -ENODEV) {
            LOG_ERROR("libevdev error: %d", rc);
            break;
        }
    }
    return NULL;
}

static int send_control_cmd(const char *sock, const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { fprintf(stderr, "socket: %s\n", strerror(errno)); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to %s: %s\n", sock, strerror(errno));
        close(fd); return 1;
    }
    if (write(fd, cmd, strlen(cmd)) < 0) {}
    close(fd); return 0;
}

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  --device PATH       Input device (default: /dev/input/event2)\n"
        "  --color #RRGGBB     Trail color (default: #ffffff)\n"
        "  --width N           Trail head radius px (default: 8)\n"
        "  --length N          Trail duration ms (default: 500)\n"
        "  --min-speed N       Stationary threshold px (default: 2)\n"
        "  --smooth-factor N   EMA filter 0-1 (default: 0.6)\n"
        "  --color-cycle on|off (default: off)\n"
        "  --cycle-speed N     Cycle period seconds (default: 5)\n"
        "  --socket PATH       Control socket path\n"
        "  --log-level debug|info|warn|error (default: info)\n"
        "  --log-file PATH     Log file (default: stderr)\n"
        "  --ctl \"CMD\"         Send command to running instance\n"
        "  --help\n", p);
}

int main(int argc, char *argv[]) {
    const char *device_path = "/dev/input/event2";
    double cr = 1.0, cg = 1.0, cb = 1.0, ca = 1.0;
    double width = 8.0;
    uint64_t length_ms = 500;
    double min_speed = 2.0;
    double smooth_factor = 0.6;
    int log_level = 1;
    const char *log_path = NULL;
    const char *socket_path = NULL;
    const char *ctl_cmd = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        else if (strcmp(argv[i], "--device") == 0 && i+1 < argc) device_path = argv[++i];
        else if (strcmp(argv[i], "--color") == 0 && i+1 < argc) {
            const char *c = argv[++i];
            unsigned int ri, gi, bi;
            if (sscanf(c, "#%02x%02x%02x", &ri, &gi, &bi) == 3) {
                cr = ri/255.0; cg = gi/255.0; cb = bi/255.0;
            }
        }
        else if (strcmp(argv[i], "--width") == 0 && i+1 < argc) width = atof(argv[++i]);
        else if (strcmp(argv[i], "--length") == 0 && i+1 < argc) length_ms = (uint64_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--min-speed") == 0 && i+1 < argc) min_speed = atof(argv[++i]);
        else if (strcmp(argv[i], "--smooth-factor") == 0 && i+1 < argc) {
            smooth_factor = atof(argv[++i]);
            if (smooth_factor < 0.0) smooth_factor = 0.0;
            if (smooth_factor > 1.0) smooth_factor = 1.0;
        }
        else if (strcmp(argv[i], "--color-cycle") == 0 && i+1 < argc)
            color_cycle_on = (strcmp(argv[++i], "on") == 0);
        else if (strcmp(argv[i], "--cycle-speed") == 0 && i+1 < argc) cycle_speed = atof(argv[++i]);
        else if (strcmp(argv[i], "--socket") == 0 && i+1 < argc) socket_path = argv[++i];
        else if (strcmp(argv[i], "--log-level") == 0 && i+1 < argc) {
            const char *l = argv[++i];
            if (strcmp(l, "debug") == 0) log_level = 0;
            else if (strcmp(l, "info") == 0) log_level = 1;
            else if (strcmp(l, "warn") == 0) log_level = 2;
            else if (strcmp(l, "error") == 0) log_level = 3;
        }
        else if (strcmp(argv[i], "--log-file") == 0 && i+1 < argc) log_path = argv[++i];
        else if (strcmp(argv[i], "--ctl") == 0 && i+1 < argc) ctl_cmd = argv[++i];
        else { fprintf(stderr, "Unknown: %s\n", argv[i]); usage(argv[0]); return 1; }
    }

    if (log_path && strcmp(log_path, "-") != 0 && ctl_cmd == NULL) {
        FILE *f = fopen(log_path, "a");
        if (f) log_init(f, log_level);
        else { log_init(stderr, log_level); LOG_ERROR("Cannot open: %s", log_path); }
    } else log_init(stderr, log_level);

    if (!socket_path) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        static char sbuf[256];
        snprintf(sbuf, sizeof(sbuf), "%s/mouse-trail.sock", xdg ? xdg : "/tmp");
        socket_path = sbuf;
    }
    if (ctl_cmd) return send_control_cmd(socket_path, ctl_cmd);

    LOG_INFO("mouse-trail v0.3.0 starting");
    LOG_INFO("device=%s width=%.1f length=%lums min_speed=%.1f smooth=%.2f",
             device_path, width, (unsigned long)length_ms, min_speed, smooth_factor);

    input_fd = open(device_path, O_RDONLY | O_NONBLOCK);
    if (input_fd < 0) { LOG_ERROR("Cannot open %s: %s", device_path, strerror(errno)); return 1; }
    if (libevdev_new_from_fd(input_fd, &evdev) < 0) { LOG_ERROR("libevdev failed"); close(input_fd); return 1; }
    LOG_INFO("Input: %s", libevdev_get_name(evdev));

    trail_init(&trail, width, length_ms, min_speed, smooth_factor, cr, cg, cb, ca);

    display = wl_display_connect(NULL);
    if (!display) { LOG_ERROR("Wayland connect failed"); libevdev_free(evdev); close(input_fd); return 1; }
    LOG_INFO("Wayland connected");

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        LOG_ERROR("Missing globals"); return 1;
    }
    if (num_outputs == 0) { LOG_ERROR("No outputs"); return 1; }

    /* Get output geometry */
    wl_display_roundtrip(display);

    /* Register pointer listener before creating surfaces */
    if (pointer) {
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }

    /* Estimate cursor position from screen geometry */
    double est_x = 0, est_y = 0;
    {
        int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
        for (int i = 0; i < num_outputs; i++) {
            output_t *o = &outputs[i];
            if (o->global_x < min_x) min_x = o->global_x;
            if (o->global_x + o->width > max_x) max_x = o->global_x + o->width;
            if (o->global_y < min_y) min_y = o->global_y;
            if (o->global_y + o->height > max_y) max_y = o->global_y + o->height;
        }
        est_x = (min_x + max_x) / 2.0;
        est_y = (min_y + max_y) / 2.0;
    }
    if (cursor_captured) {
        est_x = captured_cursor_x;
        est_y = captured_cursor_y;
    }

    /* Create layer surfaces */
    for (int i = 0; i < num_outputs; i++) {
        output_t *out = &outputs[i];
        out->surface = wl_compositor_create_surface(compositor);
        LOG_INFO("Output %d surface: %p", i, (void*)out->surface);
        out->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell, out->surface, out->wl_output,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mouse-trail");
        zwlr_layer_surface_v1_add_listener(out->layer_surface, &layer_surface_listener, NULL);
        zwlr_layer_surface_v1_set_anchor(out->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(out->layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(out->layer_surface, 0);
        wl_surface_commit(out->surface);
    }

    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    /* Map surfaces */
    for (int i = 0; i < num_outputs; i++) {
        output_t *o = &outputs[i];
        if (!o->configured || o->width <= 0) continue;
        int stride = o->width * 4;
        int size = stride * o->height;
        int fd = memfd_create("init", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0) continue;
        ftruncate(fd, size);
        void *d = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (d == MAP_FAILED) { close(fd); continue; }
        memset(d, 0, size);
        struct wl_shm_pool *p = wl_shm_create_pool(shm, fd, size);
        struct wl_buffer *b = wl_shm_pool_create_buffer(p, 0, o->width, o->height, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(p); close(fd);
        wl_surface_attach(o->surface, b, 0, 0);
        wl_surface_damage_buffer(o->surface, 0, 0, o->width, o->height);
        wl_surface_commit(o->surface);
        wl_buffer_destroy(b);
        munmap(d, size);
    }
    wl_display_roundtrip(display);

    /* Use initial estimate */
    trail_set_position(&trail, est_x, est_y);
    LOG_INFO("Position: (%.0f, %.0f)", est_x, est_y);

    setup_control_socket(socket_path);
    start_time_ms = get_time_ms();
    pthread_create(&input_thread, NULL, input_thread_fn, NULL);

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec its = {{0,16666667},{0,1}};
    timerfd_settime(timer_fd, 0, &its, NULL);

    int epfd = epoll_create1(0);
    struct epoll_event evt;
    evt.events = EPOLLIN; evt.data.fd = timer_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, timer_fd, &evt);
    if (ctrl_fd >= 0) {
        evt.events = EPOLLIN; evt.data.fd = ctrl_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, ctrl_fd, &evt);
    }

    /* Init phase: run main loop with normal input region until cursor captured */
    int passthrough_set = 0;
    LOG_INFO("Init phase: waiting for cursor (up to 0.5s)");

    while (running && !passthrough_set) {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);

        struct epoll_event events[4];
        int n = epoll_wait(epfd, events, 4, 50);
        wl_display_read_events(display);
        wl_display_dispatch_pending(display);

        /* Check if cursor captured via ptr_enter or ptr_motion */
        if (cursor_captured) {
            trail_set_position(&trail, captured_cursor_x, captured_cursor_y);
            LOG_INFO("Cursor captured: (%.0f, %.0f)", captured_cursor_x, captured_cursor_y);
        }

        /* Set passthrough after cursor captured or 3s timeout */
        if (cursor_captured || get_time_ms() - start_time_ms > 500) {
            for (int i = 0; i < num_outputs; i++) {
                struct wl_region *r = wl_compositor_create_region(compositor);
                wl_surface_set_input_region(outputs[i].surface, r);
                wl_region_destroy(r);
                wl_surface_commit(outputs[i].surface);
            }
            passthrough_set = 1;
            LOG_INFO("Passthrough set (%d outputs)", num_outputs);
            break;
        }
    }

    LOG_INFO("Main loop");

    while (running) {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);

        struct epoll_event events[8];
        int n = epoll_wait(epfd, events, 8, 10);
        wl_display_read_events(display);
        wl_display_dispatch_pending(display);

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == timer_fd) {
                uint64_t exp; read(timer_fd, &exp, sizeof(exp));
                uint64_t now = get_time_ms();

                pthread_mutex_lock(&input_mutex);
                double dx = pending_rel_x, dy = pending_rel_y;
                pending_rel_x = 0; pending_rel_y = 0;
                pthread_mutex_unlock(&input_mutex);

                if (dx != 0.0 || dy != 0.0)
                    trail_feed(&trail, dx, dy, now);

                if (color_cycle_on) {
                    double t = fmod((double)(now - start_time_ms)/1000.0/cycle_speed, 1.0);
                    double rr, gg, bb;
                    hsl_to_rgb(t, 1.0, 0.5, &rr, &gg, &bb);
                    trail_set_color_rgb(&trail, rr, gg, bb);
                }

                int alive = trail_cleanup(&trail, now);
                if (alive > 0 || need_redraw) {
                    render_all();
                    need_redraw = 0;
                }
            } else if (ctrl_fd >= 0 && events[i].data.fd == ctrl_fd) {
                int client = accept(ctrl_fd, NULL, NULL);
                if (client >= 0) {
                    char buf[256];
                    ssize_t nr = read(client, buf, sizeof(buf)-1);
                    if (nr > 0) {
                        buf[nr] = '\0';
                        if (buf[nr-1] == '\n') buf[nr-1] = '\0';
                        handle_control_msg(buf);
                    }
                    close(client);
                }
            }
        }
    }

    LOG_INFO("Shutting down");
    pthread_cancel(input_thread); pthread_join(input_thread, NULL);
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].layer_surface) zwlr_layer_surface_v1_destroy(outputs[i].layer_surface);
        if (outputs[i].surface) wl_surface_destroy(outputs[i].surface);
        if (outputs[i].wl_output) wl_output_destroy(outputs[i].wl_output);
    }
    if (compositor) wl_compositor_destroy(compositor);
    if (shm) wl_shm_destroy(shm);
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (pointer) wl_pointer_destroy(pointer);
    if (seat) wl_seat_destroy(seat);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);
    if (evdev) libevdev_free(evdev);
    if (input_fd >= 0) close(input_fd);
    if (ctrl_fd >= 0) { close(ctrl_fd); unlink(socket_path); }
    if (timer_fd >= 0) close(timer_fd);
    if (g_log_file && g_log_file != stderr) fclose(g_log_file);
    LOG_INFO("Exited");
    return 0;
}
