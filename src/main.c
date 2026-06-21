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
#include <signal.h>
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

/* Wayland */
static struct wl_display *display = NULL;
static struct wl_registry *registry = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct zwlr_layer_shell_v1 *layer_shell = NULL;
static struct wl_surface *surface = NULL;
static struct zwlr_layer_surface_v1 *layer_surface = NULL;
static struct wl_output *wl_output = NULL;

static int surface_width = 0, surface_height = 0;
static int configured = 0;
static int running = 1;
static int need_redraw = 0;

/* Trail */
static trail_state_t trail;
static uint64_t start_time_ms = 0;

/* Input */
static struct libevdev *evdev = NULL;
static int input_fd = -1;
static pthread_t input_thread;
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static double pending_rel_x = 0, pending_rel_y = 0;

/* Control socket */
static int ctrl_fd = -1;

/* Frame timer */
static int timer_fd = -1;

/* Color cycle */
static int color_cycle_on = 0;
static double cycle_speed = 5.0;

/* HSL to RGB for color cycling */
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

static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Create a wl_shm buffer backed by memfd */
static struct wl_buffer *create_shm_buffer(int width, int height, void **data_out, int *stride_out) {
    int stride = width * 4;
    int size = stride * height;

    int fd = memfd_create("mouse-trail-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        LOG_ERROR("memfd_create failed: %s", strerror(errno));
        return NULL;
    }
    if (ftruncate(fd, size) < 0) {
        LOG_ERROR("ftruncate failed: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        LOG_ERROR("mmap failed: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    *data_out = data;
    *stride_out = stride;
    LOG_DEBUG("create_shm_buffer: %dx%d stride=%d size=%d", width, height, stride, size);
    return buffer;
}

/* Trail render callback */
static void draw_trail_point(void *user, double x, double y, double radius,
                              double alpha, double r, double g, double b) {
    cairo_t *cr = (cairo_t *)user;
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, alpha);
    cairo_arc(cr, x, y, radius, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* Render one frame */
static void render_frame(void) {
    if (surface_width <= 0 || surface_height <= 0) return;

    void *data;
    int stride;
    struct wl_buffer *buffer = create_shm_buffer(surface_width, surface_height, &data, &stride);
    if (!buffer) return;

    cairo_surface_t *cairo_surf = cairo_image_surface_create_for_data(
        (unsigned char *)data, CAIRO_FORMAT_ARGB32,
        surface_width, surface_height, stride);
    cairo_t *cr = cairo_create(cairo_surf);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (color_cycle_on) {
        uint64_t now = get_time_ms();
        double t = fmod((double)(now - start_time_ms) / 1000.0 / cycle_speed, 1.0);
        double cr_val, cg, cb_val;
        hsl_to_rgb(t, 1.0, 0.5, &cr_val, &cg, &cb_val);
        trail_set_color_rgb(&trail, cr_val, cg, cb_val);
    }

    cairo_translate(cr, surface_width / 2.0, surface_height / 2.0);

    trail_render(&trail, get_time_ms(), draw_trail_point, cr);

    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surf);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, surface_width, surface_height);
    wl_surface_commit(surface);

    munmap(data, stride * surface_height);
    wl_buffer_destroy(buffer);
}

/* Wayland registry */
static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                             const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
        LOG_INFO("Bound wl_compositor v4");
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        LOG_INFO("Bound wl_shm v1");
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 3);
        LOG_INFO("Bound zwlr_layer_shell_v1 v3");
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!wl_output) {
            wl_output = wl_registry_bind(reg, name, &wl_output_interface, 3);
            LOG_INFO("Bound wl_output v3");
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

/* Layer surface */
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                     uint32_t serial, uint32_t width, uint32_t height) {
    (void)data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    surface_width = (int)width;
    surface_height = (int)height;
    configured = 1;
    LOG_INFO("Layer surface configured: %dx%d", surface_width, surface_height);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)data;
    LOG_INFO("Layer surface closed");
    zwlr_layer_surface_v1_destroy(surface);
    running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* Control socket commands */
static void handle_control_msg(const char *msg) {
    LOG_INFO("Control: received '%s'", msg);

    if (strncmp(msg, "color ", 6) == 0) {
        unsigned int ri, gi, bi;
        if (sscanf(msg + 6, "#%02x%02x%02x", &ri, &gi, &bi) == 3) {
            trail_set_color_rgb(&trail, ri / 255.0, gi / 255.0, bi / 255.0);
            need_redraw = 1;
        } else {
            LOG_WARN("Invalid color format: %s (use #RRGGBB)", msg + 6);
        }
    } else if (strcmp(msg, "color-cycle on") == 0) {
        color_cycle_on = 1;
        need_redraw = 1;
        LOG_INFO("Color cycle enabled");
    } else if (strcmp(msg, "color-cycle off") == 0) {
        color_cycle_on = 0;
        need_redraw = 1;
        LOG_INFO("Color cycle disabled");
    } else if (strncmp(msg, "width ", 6) == 0) {
        trail.max_radius = atof(msg + 6);
        need_redraw = 1;
        LOG_INFO("Width set to %.2f", trail.max_radius);
    } else if (strncmp(msg, "speed ", 6) == 0) {
        trail.max_age_ms = (uint64_t)atoi(msg + 6);
        need_redraw = 1;
        LOG_INFO("Length set to %lu ms", (unsigned long)trail.max_age_ms);
    } else if (strcmp(msg, "show") == 0) {
        trail.visible = true;
        need_redraw = 1;
        LOG_INFO("Trail visible");
    } else if (strcmp(msg, "hide") == 0) {
        trail.visible = false;
        need_redraw = 1;
        LOG_INFO("Trail hidden");
    } else {
        LOG_WARN("Unknown control command: %s", msg);
    }
}

static void setup_control_socket(const char *path) {
    unlink(path);
    ctrl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ctrl_fd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(ctrl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind() failed: %s", strerror(errno));
        close(ctrl_fd);
        ctrl_fd = -1;
        return;
    }

    if (listen(ctrl_fd, 5) < 0) {
        LOG_ERROR("listen() failed: %s", strerror(errno));
        close(ctrl_fd);
        ctrl_fd = -1;
        return;
    }

    LOG_INFO("Control socket listening on %s", path);
}

/* Input thread */
static void *input_thread_fn(void *arg) {
    (void)arg;
    LOG_INFO("Input thread started");

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
        } else if (rc < 0) {
            if (rc != -ENODEV) {
                LOG_ERROR("libevdev_next_event error: %d", rc);
            }
            break;
        }
    }
    LOG_INFO("Input thread stopped");
    return NULL;
}

/* Control client mode */
static int send_control_cmd(const char *socket_path, const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket() failed: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to %s: %s\n", socket_path, strerror(errno));
        close(fd);
        return 1;
    }

    write(fd, cmd, strlen(cmd));
    close(fd);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "Options:\n"
        "  --device PATH        Input device (default: /dev/input/event2)\n"
        "  --color #RRGGBB      Trail color (default: #ffffff)\n"
        "  --width N            Trail head radius in px (default: 8)\n"
        "  --length N           Trail duration in ms (default: 500)\n"
        "  --min-speed N        Stationary threshold in px (default: 2)\n"
        "  --smooth-factor N    EMA filter 0-1 (default: 0.6)\n"
        "  --color-cycle on|off Auto color cycling (default: off)\n"
        "  --cycle-speed N      Color cycle period in seconds (default: 5)\n"
        "  --socket PATH        Control socket path\n"
        "  --log-level LEVEL    debug|info|warn|error (default: info)\n"
        "  --log-file PATH      Log file path (default: stderr)\n"
        "  --ctl \"COMMAND\"      Send command to running instance and exit\n"
        "  --help               Show this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    const char *device_path = "/dev/input/event2";
    double r = 1.0, g_val = 1.0, b_val = 1.0, a_val = 1.0;
    double width = 8.0;
    uint64_t length_ms = 500;
    double min_speed = 2.0;
    double smooth_factor = 0.6;
    int log_level = 1;
    const char *log_path = NULL;
    const char *socket_path = NULL;
    const char *ctl_cmd = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device_path = argv[++i];
        } else if (strcmp(argv[i], "--color") == 0 && i + 1 < argc) {
            const char *c = argv[++i];
            unsigned int ri, gi, bi;
            if (sscanf(c, "#%02x%02x%02x", &ri, &gi, &bi) == 3) {
                r = ri / 255.0; g_val = gi / 255.0; b_val = bi / 255.0;
            }
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atof(argv[++i]);
        } else if (strcmp(argv[i], "--length") == 0 && i + 1 < argc) {
            length_ms = (uint64_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--min-speed") == 0 && i + 1 < argc) {
            min_speed = atof(argv[++i]);
        } else if (strcmp(argv[i], "--smooth-factor") == 0 && i + 1 < argc) {
            smooth_factor = atof(argv[++i]);
            if (smooth_factor < 0.0) smooth_factor = 0.0;
            if (smooth_factor > 1.0) smooth_factor = 1.0;
        } else if (strcmp(argv[i], "--color-cycle") == 0 && i + 1 < argc) {
            color_cycle_on = (strcmp(argv[++i], "on") == 0);
        } else if (strcmp(argv[i], "--cycle-speed") == 0 && i + 1 < argc) {
            cycle_speed = atof(argv[++i]);
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            const char *l = argv[++i];
            if (strcmp(l, "debug") == 0) log_level = 0;
            else if (strcmp(l, "info") == 0) log_level = 1;
            else if (strcmp(l, "warn") == 0) log_level = 2;
            else if (strcmp(l, "error") == 0) log_level = 3;
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
            ctl_cmd = argv[++i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    /* Setup logging */
    if (log_path && strcmp(log_path, "-") != 0 && ctl_cmd == NULL) {
        FILE *f = fopen(log_path, "a");
        if (f) {
            log_init(f, log_level);
        } else {
            log_init(stderr, log_level);
            LOG_ERROR("Cannot open log file: %s, using stderr", log_path);
        }
    } else {
        log_init(stderr, log_level);
    }

    /* Determine socket path */
    if (!socket_path) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        static char sock_buf[256];
        if (xdg) {
            snprintf(sock_buf, sizeof(sock_buf), "%s/mouse-trail.sock", xdg);
        } else {
            snprintf(sock_buf, sizeof(sock_buf), "/tmp/mouse-trail.sock");
        }
        socket_path = sock_buf;
    }

    /* Control client mode */
    if (ctl_cmd) {
        return send_control_cmd(socket_path, ctl_cmd);
    }

    LOG_INFO("Starting mouse-trail v0.1.0");
    LOG_INFO("Config: device=%s width=%.1f length=%lums min_speed=%.1f smooth=%.2f",
             device_path, width, (unsigned long)length_ms, min_speed, smooth_factor);
    LOG_INFO("Color: #%02x%02x%02x cycle=%s speed=%.1fs",
             (int)(r*255), (int)(g_val*255), (int)(b_val*255),
             color_cycle_on ? "on" : "off", cycle_speed);

    /* Open input device */
    input_fd = open(device_path, O_RDONLY | O_NONBLOCK);
    if (input_fd < 0) {
        LOG_ERROR("Cannot open %s: %s", device_path, strerror(errno));
        return 1;
    }
    if (libevdev_new_from_fd(input_fd, &evdev) < 0) {
        LOG_ERROR("libevdev_new_from_fd failed");
        close(input_fd);
        return 1;
    }
    LOG_INFO("Opened input device: %s (%s)", libevdev_get_name(evdev), device_path);

    trail_init(&trail, width, length_ms, min_speed, smooth_factor, r, g_val, b_val, a_val);

    /* Connect to Wayland */
    display = wl_display_connect(NULL);
    if (!display) {
        LOG_ERROR("Cannot connect to Wayland display");
        libevdev_free(evdev);
        close(input_fd);
        return 1;
    }
    LOG_INFO("Connected to Wayland display");

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        LOG_ERROR("Missing Wayland globals: compositor=%p shm=%p layer_shell=%p",
                  (void*)compositor, (void*)shm, (void*)layer_shell);
        return 1;
    }

    /* Create surface */
    surface = wl_compositor_create_surface(compositor);
    if (!surface) {
        LOG_ERROR("Cannot create wl_surface");
        return 1;
    }

    /* Empty input region for click passthrough */
    struct wl_region *input_region = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(surface, input_region);
    wl_region_destroy(input_region);
    LOG_INFO("Input region set empty (click passthrough)");

    /* Create layer surface */
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface,
        wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mouse-trail");
    if (!layer_surface) {
        LOG_ERROR("Cannot create layer surface");
        return 1;
    }
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);

    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 0);
    wl_surface_commit(surface);

    wl_display_roundtrip(display);
    if (!configured) {
        LOG_ERROR("Layer surface not configured");
        return 1;
    }
    LOG_INFO("Layer surface ready: %dx%d", surface_width, surface_height);

    /* Setup control socket */
    setup_control_socket(socket_path);

    /* Start input thread */
    start_time_ms = get_time_ms();
    pthread_create(&input_thread, NULL, input_thread_fn, NULL);

    /* Render timer: ~60 Hz */
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec its = {
        .it_interval = { .tv_sec = 0, .tv_nsec = 16666667 },
        .it_value = { .tv_sec = 0, .tv_nsec = 1 },
    };
    timerfd_settime(timer_fd, 0, &its, NULL);

    /* Epoll for timer + control */
    int epfd = epoll_create1(0);
    struct epoll_event evt;
    evt.events = EPOLLIN;
    evt.data.fd = timer_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, timer_fd, &evt);

    if (ctrl_fd >= 0) {
        evt.events = EPOLLIN;
        evt.data.fd = ctrl_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, ctrl_fd, &evt);
    }

    LOG_INFO("Entering main loop");

    /* Main loop */
    while (running) {
        while (wl_display_prepare_read(display) != 0) {
            wl_display_dispatch_pending(display);
        }
        wl_display_flush(display);

        struct epoll_event events[8];
        int n = epoll_wait(epfd, events, 8, 10);
        wl_display_read_events(display);
        wl_display_dispatch_pending(display);

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == timer_fd) {
                uint64_t exp;
                read(timer_fd, &exp, sizeof(exp));

                pthread_mutex_lock(&input_mutex);
                double rx = pending_rel_x;
                double ry = pending_rel_y;
                pending_rel_x = 0;
                pending_rel_y = 0;
                pthread_mutex_unlock(&input_mutex);

                if (rx != 0.0 || ry != 0.0) {
                    int moved = trail_feed(&trail, rx, ry, get_time_ms());
                    if (moved) need_redraw = 1;
                }

                if (need_redraw) {
                    render_frame();
                    need_redraw = 0;
                }
            } else if (ctrl_fd >= 0 && events[i].data.fd == ctrl_fd) {
                int client = accept(ctrl_fd, NULL, NULL);
                if (client >= 0) {
                    char buf[256];
                    ssize_t nread = read(client, buf, sizeof(buf) - 1);
                    if (nread > 0) {
                        buf[nread] = '\0';
                        if (nread > 0 && buf[nread-1] == '\n') buf[nread-1] = '\0';
                        handle_control_msg(buf);
                    }
                    close(client);
                }
            }
        }
    }

    LOG_INFO("Shutting down");

    pthread_cancel(input_thread);
    pthread_join(input_thread, NULL);

    if (layer_surface) zwlr_layer_surface_v1_destroy(layer_surface);
    if (surface) wl_surface_destroy(surface);
    if (wl_output) wl_output_destroy(wl_output);
    if (compositor) wl_compositor_destroy(compositor);
    if (shm) wl_shm_destroy(shm);
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);
    if (evdev) libevdev_free(evdev);
    if (input_fd >= 0) close(input_fd);
    if (ctrl_fd >= 0) { close(ctrl_fd); unlink(socket_path); }
    if (timer_fd >= 0) close(timer_fd);
    if (g_log_file && g_log_file != stderr) fclose(g_log_file);

    LOG_INFO("mouse-trail exited");
    return 0;
}
