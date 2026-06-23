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
#include <signal.h>
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
    int width, height;       /* logical surface dimensions */
    int phys_w, phys_h;      /* physical pixel dimensions from mode */
    double scale;
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

static double captured_cursor_x = 0, captured_cursor_y = 0;
static int cursor_captured = 0;
static struct wl_surface *current_pointer_surface = NULL;

static trail_state_t trail;
static uint64_t start_time_ms = 0;

static struct libevdev *evdev = NULL;
static int input_fd = -1;
static struct libevdev *kbd_evdev = NULL;
static int kbd_fd = -1;
static pthread_t input_thread, kbd_thread;
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;

static int ctrl_fd = -1;
static int timer_fd = -1;
static int running = 1;
static int need_redraw = 0;
static int center_region_set = 0;

static int color_cycle_on = 0;
static double cycle_speed = 5.0;

/* Screen bounds for cursor clamping */
static double bounds_min_x = 0, bounds_min_y = 0;
static double bounds_max_x = 0, bounds_max_y = 0;

static uint64_t get_time_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static double hue2rgb(double p, double q, double t) {
    if (t < 0.0) t += 1.0; if (t > 1.0) t -= 1.0;
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
            LOG_DEBUG("Recalibrated via motion: output=%d global=(%.0f,%.0f)", i, captured_cursor_x, captured_cursor_y);
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

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
    const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 2);
    else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (num_outputs < MAX_OUTPUTS) {
            struct wl_output *o = wl_registry_bind(reg, name, &wl_output_interface, 3);
            wl_output_add_listener(o, &output_listener, NULL);
            memset(&outputs[num_outputs], 0, sizeof(output_t));
            outputs[num_outputs].wl_output = o;
            outputs[num_outputs].scale = 1.0;
            num_outputs++;
        }
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (!seat) { seat = wl_registry_bind(reg, name, &wl_seat_interface, 5); pointer = wl_seat_get_pointer(seat); }
    }
}
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) { (void)data;(void)reg;(void)name; }
static const struct wl_registry_listener registry_listener = { .global=registry_global, .global_remove=registry_global_remove };

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *s,
    uint32_t serial, uint32_t w, uint32_t h) {
    (void)data; zwlr_layer_surface_v1_ack_configure(s, serial);
    for (int i = 0; i < num_outputs; i++)
        if (outputs[i].layer_surface == s) {
            outputs[i].width=(int)w; outputs[i].height=(int)h; outputs[i].configured=1;
            if (outputs[i].phys_w > 0 && w > 0)
                outputs[i].scale = (double)outputs[i].phys_w / (double)w;
            LOG_INFO("Output %d: logical=%dx%d phys=%dx%d scale=%.2f",
                     i, (int)w, (int)h, outputs[i].phys_w, outputs[i].phys_h, outputs[i].scale);
            return;
        }
}
static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *s) { (void)data; zwlr_layer_surface_v1_destroy(s); running=0; }
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = { .configure=layer_surface_configure, .closed=layer_surface_closed };

static struct wl_buffer *create_shm_buffer(int w, int h, void **data_out, int *stride_out) {
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("mt", MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if (fd < 0) return NULL;
    ftruncate(fd, size);
    void *d = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (d == MAP_FAILED) { close(fd); return NULL; }
    struct wl_shm_pool *p = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(p, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(p); close(fd);
    *data_out = d; *stride_out = stride;
    return b;
}

static void draw_trail_point(void *user, double x, double y, double radius,
    double alpha, double r, double g, double b) {
    cairo_t *cr = (cairo_t*)user;
    cairo_save(cr); cairo_set_source_rgba(cr, r, g, b, alpha);
    cairo_arc(cr, x, y, radius, 0.0, 2.0*M_PI); cairo_fill(cr); cairo_restore(cr);
}

static void render_output(output_t *out) {
    if (out->width <= 0 || out->height <= 0 || !out->configured) return;
    void *data; int stride;
    struct wl_buffer *buf = create_shm_buffer(out->width, out->height, &data, &stride);
    if (!buf) return;
    cairo_surface_t *cs = cairo_image_surface_create_for_data((unsigned char*)data, CAIRO_FORMAT_ARGB32, out->width, out->height, stride);
    cairo_t *cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR); cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_translate(cr, -(double)out->global_x, -(double)out->global_y);
    trail_render(&trail, get_time_ms(), draw_trail_point, cr);
    cairo_destroy(cr); cairo_surface_destroy(cs);
    wl_surface_attach(out->surface, buf, 0, 0);
    wl_surface_damage_buffer(out->surface, 0, 0, out->width, out->height);
    wl_surface_commit(out->surface);
    munmap(data, stride * out->height);
    wl_buffer_destroy(buf);
}

static void render_all(void) {
    for (int i = 0; i < num_outputs; i++) render_output(&outputs[i]);
}

static void handle_control_msg(const char *msg) {
    if (strncmp(msg, "color ", 6)==0) {
        unsigned int ri,gi,bi;
        const char *c = msg + 6;
        if (*c == '#') c++;
        if (sscanf(c,"%02x%02x%02x",&ri,&gi,&bi)==3) { trail_set_color_rgb(&trail,ri/255.0,gi/255.0,bi/255.0); need_redraw=1; }
    } else if (strcmp(msg,"color-cycle on")==0) { color_cycle_on=1; need_redraw=1; }
    else if (strcmp(msg,"color-cycle off")==0) { color_cycle_on=0; need_redraw=1; }
    else if (strncmp(msg,"width ",6)==0) { trail.max_radius=atof(msg+6); need_redraw=1; }
    else if (strncmp(msg,"speed ",6)==0) { trail.max_age_ms=(uint64_t)atoi(msg+6); need_redraw=1; }
    else if (strncmp(msg,"alpha ",6)==0) { trail.a=atof(msg+6); need_redraw=1; }
    else if (strcmp(msg,"show")==0) { trail.visible=true; need_redraw=1; }
    else if (strcmp(msg,"hide")==0) { trail.visible=false; need_redraw=1; }
    else if (strcmp(msg,"warp")==0) {
        /* Trigger full-surface recapture: reset flags + timer */
        center_region_set = 0;
        cursor_captured = 0;
        start_time_ms = get_time_ms();  /* restart 5s timeout for this recapture */
        LOG_INFO("Warp: expanding to full region, waiting for cursor");
    }
}

static void setup_control_socket(const char *path) {
    unlink(path);
    ctrl_fd = socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK, 0);
    if (ctrl_fd < 0) return;
    struct sockaddr_un addr; memset(&addr,0,sizeof(addr)); addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);
    if (bind(ctrl_fd,(struct sockaddr*)&addr,sizeof(addr))<0) { close(ctrl_fd); ctrl_fd=-1; return; }
    if (listen(ctrl_fd,5)<0) { close(ctrl_fd); ctrl_fd=-1; return; }
}

static void *input_thread_fn(void *arg) {
    (void)arg;
    struct input_event ev;
    while (running) {
        int rc = libevdev_next_event(evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
            if (ev.type == EV_REL && (ev.code == REL_X || ev.code == REL_Y)) {
                pthread_mutex_lock(&input_mutex);
                double dx = (ev.code == REL_X) ? (double)ev.value : 0.0;
                double dy = (ev.code == REL_Y) ? (double)ev.value : 0.0;
                if (dx != 0.0 || dy != 0.0) {
                    /* Per-event clamping matches compositor behavior */
                    double new_x = trail.pos_x + dx;
                    double new_y = trail.pos_y + dy;
                    if (new_x < bounds_min_x) { dx = bounds_min_x - trail.pos_x; trail.pos_x = bounds_min_x; }
                    else if (new_x > bounds_max_x) { dx = bounds_max_x - trail.pos_x; trail.pos_x = bounds_max_x; }
                    else trail.pos_x = new_x;
                    if (new_y < bounds_min_y) { dy = bounds_min_y - trail.pos_y; trail.pos_y = bounds_min_y; }
                    else if (new_y > bounds_max_y) { dy = bounds_max_y - trail.pos_y; trail.pos_y = bounds_max_y; }
                    else trail.pos_y = new_y;
                    if (dx != 0.0 || dy != 0.0) {
                        int idx = (trail.head + trail.count) % MAX_TRAIL_POINTS;
                        trail.points[idx].x = trail.pos_x;
                        trail.points[idx].y = trail.pos_y;
                        trail.points[idx].timestamp_ms = get_time_ms();
                        if (trail.count < MAX_TRAIL_POINTS) trail.count++;
                        else trail.head = (trail.head + 1) % MAX_TRAIL_POINTS;
                        trail.stationary_start = 0;
                        need_redraw = 1;
                    }
                }
                pthread_mutex_unlock(&input_mutex);
            }
        } else if (rc == -EAGAIN) { usleep(200); }
        else if (rc < 0 && rc != -ENODEV) break;
    }
    return NULL;
}

/* Keyboard monitor: detect monitor-switch hotkeys and trigger warp */
static void *kbd_thread_fn(void *arg) {
    (void)arg;
    if (!kbd_evdev) return NULL;
    int super_down = 0, shift_down = 0, ctrl_down = 0;
    struct input_event ev;
    while (running) {
        int rc = libevdev_next_event(kbd_evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_KEY) {
            int pressed = (ev.value == 1);
            int released = (ev.value == 0);
            switch (ev.code) {
                case KEY_LEFTMETA: case KEY_RIGHTMETA:
                    if (pressed) super_down = 1; else if (released) super_down = 0; break;
                case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
                    if (pressed) shift_down = 1; else if (released) shift_down = 0; break;
                case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
                    if (pressed) ctrl_down = 1; else if (released) ctrl_down = 0; break;
                case KEY_LEFT:
                case KEY_RIGHT:
                    if (pressed && super_down && shift_down) {
                        LOG_INFO("Warp hotkey detected (Super+Shift+%s%s), triggering recalibration",
                                 ev.code == KEY_LEFT ? "Left" : "Right",
                                 ctrl_down ? "+Ctrl" : "");
                        pthread_mutex_lock(&input_mutex);
                        center_region_set = 0;
                        cursor_captured = 0;
                        start_time_ms = get_time_ms();
                        need_redraw = 1;
                        pthread_mutex_unlock(&input_mutex);
                    }
                    break;
            }
        } else if (rc == -EAGAIN) { usleep(500); }
        else if (rc < 0 && rc != -ENODEV) break;
    }
    return NULL;
}

static int send_control_cmd(const char *sock, const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    struct sockaddr_un addr; memset(&addr,0,sizeof(addr)); addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path)-1);
    if (connect(fd,(struct sockaddr*)&addr,sizeof(addr))<0) { close(fd); return 1; }
    if (write(fd, cmd, strlen(cmd)) < 0) {}
    close(fd); return 0;
}

/* Config file parser */
#define MAX_CONFIG_INCLUDES 8
static int config_include_count = 0;

static void parse_config(const char *path,
    double *cr, double *cg, double *cb, double *ca,
    double *width, uint64_t *length_ms, double *min_speed, double *smooth_factor,
    int *color_cycle_on, double *cycle_speed,
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

        if (strcmp(key, "import") == 0) { parse_config(val, cr, cg, cb, ca, width, length_ms, min_speed, smooth_factor, color_cycle_on, cycle_speed, device, kbd_device); }
        else if (strcmp(key, "color") == 0) { unsigned int ri,gi,bi; const char *cv = val; if (*cv=='#') cv++; if(sscanf(cv,"%02x%02x%02x",&ri,&gi,&bi)==3){ *cr=ri/255.0;*cg=gi/255.0;*cb=bi/255.0; } }
        else if (strcmp(key, "alpha") == 0) *ca = atof(val);
        else if (strcmp(key, "width") == 0) *width = atof(val);
        else if (strcmp(key, "length") == 0) *length_ms = (uint64_t)atoi(val);
        else if (strcmp(key, "min_speed") == 0) *min_speed = atof(val);
        else if (strcmp(key, "smooth_factor") == 0) *smooth_factor = atof(val);
        else if (strcmp(key, "color_cycle") == 0) *color_cycle_on = (strcmp(val, "on") == 0);
        else if (strcmp(key, "cycle_speed") == 0) *cycle_speed = atof(val);
        else if (strcmp(key, "device") == 0) { *device = strdup(val); }
        else if (strcmp(key, "kbd_device") == 0) { *kbd_device = strdup(val); }
    }
    fclose(f);
}

static void usage(const char *p) {
        fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  --config PATH       Config file (default: ~/.config/mouse-trail/config)\n"
        "  --device PATH       Input device (default: /dev/input/event2)\n"
        "  --kbd-device PATH    Keyboard for hotkey detection (default: /dev/input/event3)\n"
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
    const char *device_path = "/dev/input/event2";
    const char *kbd_device_path = NULL;
    double cr=1.0,cg=1.0,cb=1.0,ca=1.0, width=8.0;
    uint64_t length_ms=500; double min_speed=2.0, smooth_factor=0.6;
    int log_level=1; const char *log_path=NULL, *socket_path=NULL, *ctl_cmd=NULL;
    const char *config_path = NULL;

    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--help")==0) { usage(argv[0]); return 0; }
        else if (strcmp(argv[i],"--config")==0&&i+1<argc) config_path=argv[++i];
        else if (strcmp(argv[i],"--device")==0&&i+1<argc) device_path=argv[++i];
        else if (strcmp(argv[i],"--kbd-device")==0&&i+1<argc) kbd_device_path=argv[++i];
        else if (strcmp(argv[i],"--color")==0&&i+1<argc) {
            const char *c = argv[++i];
            unsigned int ri,gi,bi;
            if (*c == '#') c++;
            if (sscanf(c,"%02x%02x%02x",&ri,&gi,&bi)==3) { cr=ri/255.0;cg=gi/255.0;cb=bi/255.0; }
        }
        else if (strcmp(argv[i],"--alpha")==0&&i+1<argc) ca=atof(argv[++i]);
        else if (strcmp(argv[i],"--width")==0&&i+1<argc) width=atof(argv[++i]);
        else if (strcmp(argv[i],"--length")==0&&i+1<argc) length_ms=(uint64_t)atoi(argv[++i]);
        else if (strcmp(argv[i],"--min-speed")==0&&i+1<argc) min_speed=atof(argv[++i]);
        else if (strcmp(argv[i],"--smooth-factor")==0&&i+1<argc) { smooth_factor=atof(argv[++i]); if(smooth_factor<0)smooth_factor=0; if(smooth_factor>1)smooth_factor=1; }
        else if (strcmp(argv[i],"--color-cycle")==0&&i+1<argc) color_cycle_on=(strcmp(argv[++i],"on")==0);
        else if (strcmp(argv[i],"--cycle-speed")==0&&i+1<argc) cycle_speed=atof(argv[++i]);
        else if (strcmp(argv[i],"--socket")==0&&i+1<argc) socket_path=argv[++i];
        else if (strcmp(argv[i],"--log-level")==0&&i+1<argc) {
            const char *l=argv[++i];
            if(strcmp(l,"debug")==0)log_level=0; else if(strcmp(l,"info")==0)log_level=1;
            else if(strcmp(l,"warn")==0)log_level=2; else if(strcmp(l,"error")==0)log_level=3;
        }
        else if (strcmp(argv[i],"--log-file")==0&&i+1<argc) log_path=argv[++i];
        else if (strcmp(argv[i],"--ctl")==0&&i+1<argc) ctl_cmd=argv[++i];
        else { fprintf(stderr,"Unknown: %s\n",argv[i]); usage(argv[0]); return 1; }
    }

    /* Load config file (default: ~/.config/mouse-trail/config) */
    if (!config_path) {
        const char *home = getenv("HOME");
        static char def_cfg[512];
        if (home) snprintf(def_cfg, sizeof(def_cfg), "%s/.config/mouse-trail/config", home);
        else snprintf(def_cfg, sizeof(def_cfg), "/tmp/mouse-trail-config");
        config_path = def_cfg;
    }
    parse_config(config_path, &cr, &cg, &cb, &ca, &width, &length_ms, &min_speed, &smooth_factor, &color_cycle_on, &cycle_speed, &device_path, &kbd_device_path);

    /* Default keyboard device for hotkey detection */
    if (!kbd_device_path) kbd_device_path = "/dev/input/event5";

    if (log_path && strcmp(log_path,"-")!=0 && ctl_cmd==NULL) { FILE *f=fopen(log_path,"a"); if(f)log_init(f,log_level); else{log_init(stderr,log_level);LOG_ERROR("Cannot open: %s",log_path);} }
    else log_init(stderr, log_level);

    if (!socket_path) { const char *xdg=getenv("XDG_RUNTIME_DIR"); static char sbuf[256]; snprintf(sbuf,sizeof(sbuf),"%s/mouse-trail.sock",xdg?xdg:"/tmp"); socket_path=sbuf; }
    if (ctl_cmd) return send_control_cmd(socket_path, ctl_cmd);

    LOG_INFO("mouse-trail v0.11");

    input_fd = open(device_path, O_RDONLY|O_NONBLOCK);
    if (input_fd<0) { LOG_ERROR("Cannot open %s: %s",device_path,strerror(errno)); return 1; }
    if (libevdev_new_from_fd(input_fd, &evdev)<0) { LOG_ERROR("libevdev failed"); close(input_fd); return 1; }

    trail_init(&trail, width, length_ms, min_speed, smooth_factor, cr, cg, cb, ca);

    /* Open keyboard device for hotkey monitoring */
    kbd_fd = open(kbd_device_path, O_RDONLY|O_NONBLOCK);
    if (kbd_fd >= 0 && libevdev_new_from_fd(kbd_fd, &kbd_evdev) >= 0) {
        LOG_INFO("Keyboard: %s (%s)", libevdev_get_name(kbd_evdev), kbd_device_path);
    } else {
        if (kbd_fd >= 0) close(kbd_fd);
        kbd_fd = -1;
        kbd_evdev = NULL;
        LOG_WARN("Cannot open keyboard %s, warp hotkey detection disabled", kbd_device_path);
    }

    display = wl_display_connect(NULL);
    if (!display) { LOG_ERROR("Wayland connect failed"); libevdev_free(evdev); close(input_fd); return 1; }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor||!shm||!layer_shell) { LOG_ERROR("Missing globals"); return 1; }
    if (num_outputs==0) { LOG_ERROR("No outputs"); return 1; }

    if (pointer) wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    wl_display_roundtrip(display); /* output geometry/mode/scale */

    double est_x=0,est_y=0;
    { int mix=0,maxx=0,miy=0,may=0;
      for(int i=0;i<num_outputs;i++){ output_t*o=&outputs[i];
        if(o->global_x<mix)mix=o->global_x; if(o->global_x+o->phys_w>maxx)maxx=o->global_x+o->phys_w;
        if(o->global_y<miy)miy=o->global_y; if(o->global_y+o->phys_h>may)may=o->global_y+o->phys_h; }
      est_x=(mix+maxx)/2.0; est_y=(miy+may)/2.0; }

    for(int i=0;i<num_outputs;i++){ output_t*o=&outputs[i];
        o->surface=wl_compositor_create_surface(compositor);
        o->layer_surface=zwlr_layer_shell_v1_get_layer_surface(layer_shell,o->surface,o->wl_output,ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,"mouse-trail");
        zwlr_layer_surface_v1_add_listener(o->layer_surface,&layer_surface_listener,NULL);
        zwlr_layer_surface_v1_set_anchor(o->layer_surface,ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM|ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface,-1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer_surface,0);
        wl_surface_commit(o->surface); }
    wl_display_roundtrip(display); wl_display_roundtrip(display);

    /* Compute cursor bounds from LOGICAL surface dimensions */
    { bounds_min_x = bounds_max_x = bounds_min_y = bounds_max_y = 0;
      for(int i=0;i<num_outputs;i++){ output_t*o=&outputs[i];
        double l = o->global_x, r = o->global_x + (double)o->width;
        double t = o->global_y, b = o->global_y + (double)o->height;
        if(i==0 || l<bounds_min_x) bounds_min_x=l;
        if(i==0 || r>bounds_max_x) bounds_max_x=r;
        if(i==0 || t<bounds_min_y) bounds_min_y=t;
        if(i==0 || b>bounds_max_y) bounds_max_y=b; }
      LOG_INFO("Bounds: x=[%.0f,%.0f] y=[%.0f,%.0f]",
               bounds_min_x, bounds_max_x, bounds_min_y, bounds_max_y); }

    /* Map surfaces with transparent frames */
    for(int i=0;i<num_outputs;i++){ output_t*o=&outputs[i];
        if(!o->configured||o->width<=0)continue;
        int stride=o->width*4, size=stride*o->height;
        int fd=memfd_create("init",MFD_CLOEXEC|MFD_ALLOW_SEALING);
        if(fd<0)continue; ftruncate(fd,size);
        void*d=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(d==MAP_FAILED){close(fd);continue;} memset(d,0,size);
        struct wl_shm_pool*p=wl_shm_create_pool(shm,fd,size);
        struct wl_buffer*b=wl_shm_pool_create_buffer(p,0,o->width,o->height,stride,WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(p);close(fd);
        wl_surface_attach(o->surface,b,0,0);
        wl_surface_damage_buffer(o->surface,0,0,o->width,o->height);
        wl_surface_commit(o->surface); wl_buffer_destroy(b); munmap(d,size); }
    wl_display_roundtrip(display);

    /* Retry cursor capture with multiple roundtrips */
    for (int retry = 0; retry < 8 && pointer && !cursor_captured; retry++) {
        usleep(30000);
        wl_display_roundtrip(display);
        LOG_INFO("Capture retry %d: captured=%d", retry, cursor_captured);
    }

    if (cursor_captured) { est_x=captured_cursor_x; est_y=captured_cursor_y; }
    else {
        est_x = outputs[0].global_x + outputs[0].width / 2.0;
        est_y = outputs[0].global_y + outputs[0].height / 2.0;
        LOG_INFO("Cursor not captured, using output 0 center");
    }
    trail_set_position(&trail, est_x, est_y);

    LOG_INFO("Position: (%.0f,%.0f), %d outputs%s", est_x, est_y, num_outputs,
             cursor_captured ? " (captured)" : " (primary output center)");

    setup_control_socket(socket_path);
    start_time_ms = get_time_ms();
    pthread_create(&input_thread, NULL, input_thread_fn, NULL);
    if (kbd_evdev) pthread_create(&kbd_thread, NULL, kbd_thread_fn, NULL);

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec its = {{0,16666667},{0,1}};
    timerfd_settime(timer_fd, 0, &its, NULL);
    int epfd = epoll_create1(0);
    struct epoll_event evt; evt.events=EPOLLIN;
    evt.data.fd=timer_fd; epoll_ctl(epfd,EPOLL_CTL_ADD,timer_fd,&evt);
    if(ctrl_fd>=0){ evt.data.fd=ctrl_fd; epoll_ctl(epfd,EPOLL_CTL_ADD,ctrl_fd,&evt); }
    int display_fd = wl_display_get_fd(display);
    evt.data.fd = display_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, display_fd, &evt);

    LOG_INFO("Main loop");

    while (running) {
        /* After cursor captured or 5s, shrink to center region */
        if (!center_region_set && (cursor_captured || get_time_ms() - start_time_ms > 5000)) {
            for(int i=0;i<num_outputs;i++){
                struct wl_region *r = wl_compositor_create_region(compositor);
                int cx = outputs[i].width / 2, cy = outputs[i].height / 2;
                /* Bullseye: 10x10 center + cross arms (60px each way) */
                wl_region_add(r, cx - 5,  cy - 5,  10, 10);  /* center */
                wl_region_add(r, cx - 100, cy - 1,  200, 2); /* horizontal */
                wl_region_add(r, cx - 1,  cy - 100, 2,  200); /* vertical */
                wl_surface_set_input_region(outputs[i].surface, r);
                wl_region_destroy(r);
                wl_surface_commit(outputs[i].surface);
            }
            center_region_set = 1;
            LOG_INFO("Bullseye region active (10x10 center + cross)");
        }

        while (wl_display_prepare_read(display)!=0) wl_display_dispatch_pending(display);
        wl_display_flush(display);
        struct epoll_event events[8];
        int n = epoll_wait(epfd, events, 8, 10);
        wl_display_read_events(display);
        wl_display_dispatch_pending(display);

        for (int i=0;i<n;i++) {
            if (events[i].data.fd == timer_fd) {
                uint64_t exp; read(timer_fd, &exp, sizeof(exp));
                uint64_t now = get_time_ms();

                pthread_mutex_lock(&input_mutex);
                if (color_cycle_on) {
                    double t = fmod((double)(now-start_time_ms)/1000.0/cycle_speed, 1.0);
                    double rr,gg,bb; hsl_to_rgb(t,1.0,0.5,&rr,&gg,&bb);
                    trail_set_color_rgb(&trail, rr, gg, bb);
                }
                int alive = trail_cleanup(&trail, now);
                int redraw = need_redraw;
                need_redraw = 0;
                pthread_mutex_unlock(&input_mutex);

                if (alive > 0 || redraw) render_all();
            } else if (ctrl_fd>=0 && events[i].data.fd == ctrl_fd) {
                int client = accept(ctrl_fd, NULL, NULL);
                if (client >= 0) { char buf[256]; ssize_t nr = read(client,buf,sizeof(buf)-1);
                    if (nr>0) { buf[nr]='\0'; if(buf[nr-1]=='\n')buf[nr-1]='\0'; handle_control_msg(buf); }
                    close(client); }
            }
        }
    }

    LOG_INFO("Shutting down");
    pthread_cancel(input_thread); pthread_join(input_thread, NULL);
    if (kbd_evdev) { pthread_cancel(kbd_thread); pthread_join(kbd_thread, NULL); }
    for(int i=0;i<num_outputs;i++){ if(outputs[i].layer_surface)zwlr_layer_surface_v1_destroy(outputs[i].layer_surface); if(outputs[i].surface)wl_surface_destroy(outputs[i].surface); if(outputs[i].wl_output)wl_output_destroy(outputs[i].wl_output); }
    if(pointer)wl_pointer_destroy(pointer); if(seat)wl_seat_destroy(seat);
    if(compositor)wl_compositor_destroy(compositor); if(shm)wl_shm_destroy(shm); if(layer_shell)zwlr_layer_shell_v1_destroy(layer_shell);
    if(registry)wl_registry_destroy(registry); if(display)wl_display_disconnect(display);
    if(evdev)libevdev_free(evdev); if(input_fd>=0)close(input_fd);
    if(kbd_evdev)libevdev_free(kbd_evdev); if(kbd_fd>=0)close(kbd_fd);
    if(ctrl_fd>=0){close(ctrl_fd);unlink(socket_path);} if(timer_fd>=0)close(timer_fd);
    if(g_log_file&&g_log_file!=stderr)fclose(g_log_file);
    return 0;
}
