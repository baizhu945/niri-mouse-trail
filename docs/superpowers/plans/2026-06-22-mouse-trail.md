# Mouse Trail Effect — Implementation Plan

> **For agentic workers:** Implement tasks sequentially. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Wayland layer-shell overlay that renders a fading, meteor-like mouse trail with configurable parameters and real-time control.

**Architecture:** Single C binary using wlr-layer-shell for transparent overlay, Cairo for rendering on wl_shm buffers, libevdev for relative mouse input, and a Unix domain socket for runtime control. EMA filter smooths input. Toggle via PID file pattern matching existing tools (showmethekey, lyrics).

**Tech Stack:** C99, Wayland client, wlr-layer-shell, Cairo, libevdev, POSIX

## Global Constraints

- Click passthrough: `wl_surface_set_input_region` with empty region
- Trail shape: `alpha = 1-t³`, `radius = max×(1-t)²` (meteor-like)
- Stationary: no movement above `--min-speed` for 2× trail length → fade
- Toggle: PID file `/tmp/mouse-trail.pid`, same pattern as showmethekey/lyrics
- Must work with relative input from `/dev/input/event2` (EV_REL events)
- Log format: `[TIMESTAMP] [LEVEL] file:line message`
- No home-manager switch needed for testing; compile with `gcc` directly

## File Structure

```
~/.config/home-manager/mouse-trail/
├── src/
│   ├── log.h                              # Logging macros
│   ├── trail.h                            # Trail state/API declarations
│   ├── trail.c                            # Trail logic + test main
│   ├── wlr-layer-shell-client-protocol.h  # Generated protocol header
│   ├── wlr-layer-shell-client-protocol.c  # Generated protocol code
│   └── main.c                             # Wayland + Cairo + input + control + CLI
├── mouse-trail.nix                        # Nix home-manager module
├── test-mouse-trail.sh                    # Build + log verification
├── docs/
│   ├── superpowers/plans/                 # This plan
│   └── 2026-06-22-mouse-trail-design.md   # Design spec
└── Makefile                               # Direct gcc compilation for testing
```

---

### Task 1: Log Header

**Files:**
- Create: `src/log.h`

**Interfaces:**
- Produces: `g_log_file` (FILE*), `g_log_level` (int), `LOG_DEBUG/INFO/WARN/ERROR` macros, `log_init()` function

- [ ] **Step 1: Write `src/log.h`**

```c
#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

extern FILE *g_log_file;
extern int g_log_level;

static inline void log_init(FILE *f, int level) {
    g_log_file = f;
    g_log_level = level;
}

#define LOG_FMT(level, fmt, ...) do { \
    if (g_log_file) { \
        struct timeval _tv; \
        gettimeofday(&_tv, NULL); \
        struct tm _tm; \
        localtime_r(&_tv.tv_sec, &_tm); \
        fprintf(g_log_file, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%-5s] %s:%d " fmt "\n", \
            _tm.tm_year + 1900, _tm.tm_mon + 1, _tm.tm_mday, \
            _tm.tm_hour, _tm.tm_min, _tm.tm_sec, (int)(_tv.tv_usec / 1000), \
            level, __FILE__, __LINE__, ##__VA_ARGS__); \
        fflush(g_log_file); \
    } \
} while(0)

#define LOG_DEBUG(fmt, ...) do { if (g_log_level <= 0) LOG_FMT("DEBUG", fmt, ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)  do { if (g_log_level <= 1) LOG_FMT("INFO",  fmt, ##__VA_ARGS__); } while(0)
#define LOG_WARN(fmt, ...)  do { if (g_log_level <= 2) LOG_FMT("WARN",  fmt, ##__VA_ARGS__); } while(0)
#define LOG_ERROR(fmt, ...) do { if (g_log_level <= 3) LOG_FMT("ERROR", fmt, ##__VA_ARGS__); } while(0)

#endif
```

- [ ] **Step 2: Verify it compiles**

```bash
echo '#include "src/log.h"
FILE *g_log_file = NULL;
int g_log_level = 0;
int main() { log_init(stderr, 0); LOG_INFO("test %d", 42); return 0; }' > /tmp/log_test.c
gcc -I/home/baizhu945/.config/home-manager/mouse-trail/src /tmp/log_test.c -o /tmp/log_test && /tmp/log_test
```

Expected: Output like `[2026-06-22 ...] [INFO ] log.h:NN test 42`

---

### Task 2: Trail Logic + Test

**Files:**
- Create: `src/trail.h`
- Create: `src/trail.c`

**Interfaces:**
- Produces: `trail_state_t`, `trail_init()`, `trail_feed()`, `trail_render()`, `trail_set_color()`, `trail_set_color_rgb()`

- [ ] **Step 1: Write `src/trail.h`**

```c
#ifndef TRAIL_H
#define TRAIL_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_TRAIL_POINTS 1024

typedef struct {
    double x, y;
    uint64_t timestamp_ms;
} trail_point_t;

typedef struct {
    trail_point_t points[MAX_TRAIL_POINTS];
    int head;
    int count;
    double filtered_x;
    double filtered_y;
    double max_radius;
    uint64_t max_age_ms;
    double min_speed;
    double smooth_factor;
    bool visible;
    uint64_t last_move_time;
    uint64_t stationary_start;
    double r, g, b, a;
    double last_raw_x, last_raw_y;
} trail_state_t;

void trail_init(trail_state_t *t, double width, uint64_t length_ms,
                double min_speed, double smooth_factor,
                double r, double g, double b, double a);

int trail_feed(trail_state_t *t, double rel_x, double rel_y, uint64_t now_ms);

typedef void (*trail_render_cb)(void *user, double x, double y,
                                 double radius, double alpha,
                                 double r, double g, double b);
void trail_render(trail_state_t *t, uint64_t now_ms,
                   trail_render_cb cb, void *user);

void trail_set_color(trail_state_t *t, double r, double g, double b, double a);
void trail_set_color_rgb(trail_state_t *t, double r, double g, double b);

#endif
```

- [ ] **Step 2: Write `src/trail.c`**

```c
#include "trail.h"
#include "log.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

void trail_init(trail_state_t *t, double width, uint64_t length_ms,
                double min_speed, double smooth_factor,
                double r, double g, double b, double a) {
    memset(t, 0, sizeof(*t));
    t->max_radius = width;
    t->max_age_ms = length_ms;
    t->min_speed = min_speed;
    t->smooth_factor = smooth_factor;
    t->r = r; t->g = g; t->b = b; t->a = a;
    t->visible = true;
    LOG_INFO("trail_init: width=%.2f length=%lu ms min_speed=%.2f smooth=%.2f color=#%02x%02x%02x%02x",
             width, (unsigned long)length_ms, min_speed, smooth_factor,
             (int)(r*255), (int)(g*255), (int)(b*255), (int)(a*255));
}

int trail_feed(trail_state_t *t, double rel_x, double rel_y, uint64_t now_ms) {
    if (!t->visible) return 0;

    double df = 1.0 - t->smooth_factor;
    t->filtered_x = t->filtered_x * t->smooth_factor + rel_x * df;
    t->filtered_y = t->filtered_y * t->smooth_factor + rel_y * df;

    double dist = sqrt(rel_x * rel_x + rel_y * rel_y);
    if (dist >= t->min_speed) {
        t->stationary_start = 0;
        t->last_move_time = now_ms;

        int idx = (t->head + t->count) % MAX_TRAIL_POINTS;
        t->points[idx].x = t->filtered_x;
        t->points[idx].y = t->filtered_y;
        t->points[idx].timestamp_ms = now_ms;

        if (t->count < MAX_TRAIL_POINTS) {
            t->count++;
        } else {
            t->head = (t->head + 1) % MAX_TRAIL_POINTS;
        }
        LOG_DEBUG("trail: point added x=%.1f y=%.1f count=%d dist=%.2f",
                  t->filtered_x, t->filtered_y, t->count, dist);
        return 1;
    } else {
        if (t->stationary_start == 0) {
            t->stationary_start = now_ms;
            LOG_DEBUG("trail: stationary detected");
        }
        return 0;
    }
}

void trail_render(trail_state_t *t, uint64_t now_ms,
                   trail_render_cb cb, void *user) {
    if (!t->visible) return;

    uint64_t stationary_age = (t->stationary_start > 0) ? (now_ms - t->stationary_start) : 0;

    int rendered = 0;
    for (int i = 0; i < t->count; i++) {
        int idx = (t->head + i) % MAX_TRAIL_POINTS;
        trail_point_t *pt = &t->points[idx];

        uint64_t age = now_ms - pt->timestamp_ms;

        if (age > t->max_age_ms) continue;
        if (stationary_age > t->max_age_ms * 2) continue;

        double t_norm = (double)age / (double)t->max_age_ms;
        if (t_norm < 0.0) t_norm = 0.0;
        if (t_norm > 1.0) t_norm = 1.0;

        double alpha = 1.0 - t_norm * t_norm * t_norm;
        double radius = t->max_radius * (1.0 - t_norm) * (1.0 - t_norm);

        double final_alpha = alpha * t->a;

        cb(user, pt->x, pt->y, radius, final_alpha, t->r, t->g, t->b);
        rendered++;
    }

    if (rendered > 0) {
        LOG_DEBUG("trail_render: rendered=%d points", rendered);
    }
}

void trail_set_color(trail_state_t *t, double r, double g, double b, double a) {
    t->r = r; t->g = g; t->b = b; t->a = a;
    LOG_INFO("trail_set_color: #%02x%02x%02x%02x",
             (int)(r*255), (int)(g*255), (int)(b*255), (int)(a*255));
}

void trail_set_color_rgb(trail_state_t *t, double r, double g, double b) {
    t->r = r; t->g = g; t->b = b;
    LOG_INFO("trail_set_color_rgb: #%02x%02x%02x",
             (int)(r*255), (int)(g*255), (int)(b*255));
}
```

- [ ] **Step 3: Write test program (append to `src/trail.c`)**

```c
/* ----- Built-in test ----- */
#ifdef TRAIL_TEST
#include <stdio.h>
#include <assert.h>

FILE *g_log_file = NULL;
int g_log_level = 0;

static int render_count = 0;

static void test_cb(void *user, double x, double y, double radius, double alpha,
                     double r, double g, double b) {
    (void)user;
    printf("  render: x=%.1f y=%.1f radius=%.2f alpha=%.3f color=#%02x%02x%02x\n",
           x, y, radius, alpha, (int)(r*255), (int)(g*255), (int)(b*255));
    assert(radius >= 0.0 && radius <= 8.0);
    assert(alpha >= 0.0 && alpha <= 1.0);
    render_count++;
}

int main(void) {
    log_init(stderr, 0);

    printf("=== Test 1: trail_init ===\n");
    trail_state_t t;
    trail_init(&t, 8.0, 500, 2.0, 0.6, 1.0, 0.0, 0.0, 1.0);
    assert(t.max_radius == 8.0);
    assert(t.max_age_ms == 500);
    assert(t.smooth_factor == 0.6);
    assert(t.r == 1.0 && t.g == 0.0 && t.b == 0.0);
    printf("PASS\n");

    printf("=== Test 2: trail_feed with movement ===\n");
    uint64_t t0 = 1000;
    int moved = trail_feed(&t, 10.0, 5.0, t0);
    assert(moved == 1);
    assert(t.count == 1);
    printf("PASS: count=%d filtered=(%.2f,%.2f)\n", t.count, t.filtered_x, t.filtered_y);

    printf("=== Test 3: trail_feed stationary ===\n");
    moved = trail_feed(&t, 0.0, 0.0, t0 + 10);
    assert(moved == 0);
    assert(t.stationary_start > 0);
    printf("PASS: stationary_start=%lu\n", (unsigned long)t.stationary_start);

    printf("=== Test 4: trail_render fresh ===\n");
    render_count = 0;
    trail_render(&t, t0 + 50, test_cb, NULL);
    assert(render_count == 1);
    printf("PASS: %d points rendered\n", render_count);

    printf("=== Test 5: trail_render after expiry ===\n");
    render_count = 0;
    trail_render(&t, t0 + 600, test_cb, NULL);
    assert(render_count == 0);
    printf("PASS: trail expired\n");

    printf("=== Test 6: trail_set_color ===\n");
    trail_set_color(&t, 0.0, 1.0, 0.0, 0.8);
    assert(t.r == 0.0 && t.g == 1.0 && t.b == 0.0 && t.a == 0.8);
    printf("PASS: color changed\n");

    printf("=== Test 7: EMA filter accumulation ===\n");
    trail_init(&t, 8.0, 500, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0);
    trail_feed(&t, 10.0, 0.0, 1000);
    trail_feed(&t, 10.0, 0.0, 1010);
    assert(t.filtered_x > 9.0); /* no smoothing → follows raw input */
    printf("PASS: filtered_x=%.2f (alpha=0, should equal 10)\n", t.filtered_x);

    printf("=== Test 8: EMA filter smoothing ===\n");
    trail_init(&t, 8.0, 500, 1.0, 0.9, 1.0, 1.0, 1.0, 1.0);
    trail_feed(&t, 10.0, 0.0, 1000);
    trail_feed(&t, 10.0, 0.0, 1010);
    trail_feed(&t, 10.0, 0.0, 1020);
    assert(t.filtered_x < 10.0); /* high smoothing → lag behind */
    printf("PASS: filtered_x=%.2f (alpha=0.9, should lag)\n", t.filtered_x);

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
#endif
```

- [ ] **Step 4: Compile and run tests**

```bash
gcc -DTRAIL_TEST -I/home/baizhu945/.config/home-manager/mouse-trail/src \
    /home/baizhu945/.config/home-manager/mouse-trail/src/trail.c \
    -o /tmp/trail_test -lm && /tmp/trail_test
```

Expected: All 8 tests pass with "ALL TESTS PASSED"

---

### Task 3: Write Makefile for manual compilation

**Files:**
- Create: `Makefile`

**Interfaces:**
- Consumes: `src/*.c`, `src/*.h`
- Produces: `mouse-trail` binary

- [ ] **Step 1: Write `Makefile`**

```makefile
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lm -lwayland-client -lcairo -levdev

SRC_DIR = src
BUILD_DIR = build

SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/trail.c $(SRC_DIR)/wlr-layer-shell-client-protocol.c
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
TARGET = mouse-trail

# Find wayland includes
WAYLAND_INCLUDE = $(shell pkg-config --cflags wayland-client 2>/dev/null || echo "-I/run/current-system/sw/include")
CAIRO_INCLUDE = $(shell pkg-config --cflags cairo 2>/dev/null || echo "-I/run/current-system/sw/include/cairo")
EVDEV_INCLUDE = $(shell pkg-config --cflags libevdev 2>/dev/null || echo "")
INCLUDES = -I$(SRC_DIR) $(WAYLAND_INCLUDE) $(CAIRO_INCLUDE) $(EVDEV_INCLUDE)

.PHONY: all clean test

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test:
	gcc -DTRAIL_TEST -I$(SRC_DIR) $(SRC_DIR)/trail.c -o /tmp/trail_test -lm
	/tmp/trail_test

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
```

---

### Task 4: Main Program (Wayland + Cairo + Input + Control)

**Files:**
- Create: `src/main.c`

**Interfaces:**
- Consumes: `log.h`, `trail.h`, `wlr-layer-shell-client-protocol.h`
- Produces: `mouse-trail` binary with CLI args, Wayland overlay, input reading, control socket

- [ ] **Step 1: Write `src/main.c`**

```c
#include "log.h"
#include "trail.h"
#include "wlr-layer-shell-client-protocol.h"
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <libevdev/libevdev.h>
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

/* ===== Globals ===== */
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

/* Frame timing */
static int timer_fd = -1;

/* Color cycle */
static int color_cycle_on = 0;
static double cycle_speed = 5.0;

/* ===== HSL to RGB for color cycling ===== */
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

/* ===== Wayland helpers ===== */
static struct wl_buffer *create_shm_buffer(int width, int height, void **data_out, int *stride_out) {
    int stride = width * 4;
    int size = stride * height;

    int fd = memfd_create("mouse-trail-shm", MFD_CLOEXEC);
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

/* ===== Trail render callback ===== */
static void draw_trail_point(void *user, double x, double y, double radius,
                              double alpha, double r, double g, double b) {
    cairo_t *cr = (cairo_t *)user;
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, alpha);
    cairo_arc(cr, x, y, radius, 0.0, 2.0 * 3.1415926535);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* ===== Render frame ===== */
static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

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

    /* Clear transparent */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Color cycling */
    if (color_cycle_on) {
        uint64_t now = get_time_ms();
        double t = fmod((double)(now - start_time_ms) / 1000.0 / cycle_speed, 1.0);
        double r, g, b;
        hsl_to_rgb(t, 1.0, 0.5, &r, &g, &b);
        trail_set_color_rgb(&trail, r, g, b);
        LOG_DEBUG("color_cycle: h=%.2f rgb=(%.2f,%.2f,%.2f)", t, r, g, b);
    }

    /* Translate cairo to center trail in middle of screen */
    /* Trail points are accumulated from filtered relative movements.
       They represent position relative to starting point.
       We center them in the middle of the surface. */
    cairo_translate(cr, surface_width / 2.0, surface_height / 2.0);

    trail_render(&trail, get_time_ms(), draw_trail_point, cr);

    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surf);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, surface_width, surface_height);
    wl_surface_commit(surface);

    munmap(data, stride * surface_height);
    wl_buffer_destroy(buffer);

    LOG_DEBUG("render_frame: %dx%d", surface_width, surface_height);
}

/* ===== Wayland registry handler ===== */
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

/* ===== Layer surface configure ===== */
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

/* ===== Control socket ===== */
static void handle_control_msg(const char *msg) {
    LOG_INFO("Control: received '%s'", msg);

    if (strncmp(msg, "color ", 6) == 0) {
        unsigned int r, g, b;
        if (sscanf(msg + 6, "#%02x%02x%02x", &r, &g, &b) == 3) {
            trail_set_color_rgb(&trail, r / 255.0, g / 255.0, b / 255.0);
            need_redraw = 1;
        } else if (sscanf(msg + 6, "#%02x%02x%02x%02x", &r, &g, &b, &r) >= 3) {
            trail_set_color_rgb(&trail, r / 255.0, g / 255.0, b / 255.0);
            need_redraw = 1;
        } else {
            LOG_WARN("Invalid color format: %s", msg + 6);
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
    }
}

static void setup_control_socket(const char *path) {
    unlink(path);
    ctrl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
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

/* ===== Input thread ===== */
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
            usleep(1000);
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

/* ===== Usage ===== */
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
        "  --socket PATH        Control socket path (default: $XDG_RUNTIME_DIR/mouse-trail.sock)\n"
        "  --log-level LEVEL    debug|info|warn|error (default: info)\n"
        "  --log-file PATH      Log file path (default: stderr)\n"
        "  --help               Show this help\n",
        prog);
}

/* ===== Main ===== */
int main(int argc, char *argv[]) {
    /* Defaults */
    const char *device_path = "/dev/input/event2";
    double r = 1.0, g = 1.0, b = 1.0, a = 1.0;
    double width = 8.0;
    uint64_t length_ms = 500;
    double min_speed = 2.0;
    double smooth_factor = 0.6;
    int log_level = 1;
    const char *log_path = NULL;
    const char *socket_path = NULL;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device_path = argv[++i];
        } else if (strcmp(argv[i], "--color") == 0 && i + 1 < argc) {
            const char *c = argv[++i];
            unsigned int ri, gi, bi;
            if (sscanf(c, "#%02x%02x%02x", &ri, &gi, &bi) == 3) {
                r = ri / 255.0; g = gi / 255.0; b = bi / 255.0;
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
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    /* Setup logging */
    if (log_path && strcmp(log_path, "-") != 0) {
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

    LOG_INFO("Starting mouse-trail v0.1.0");
    LOG_INFO("Config: device=%s width=%.1f length=%lums min_speed=%.1f smooth=%.2f",
             device_path, width, (unsigned long)length_ms, min_speed, smooth_factor);
    LOG_INFO("Color: #%02x%02x%02x cycle=%s speed=%.1fs",
             (int)(r*255), (int)(g*255), (int)(b*255),
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

    /* Init trail */
    trail_init(&trail, width, length_ms, min_speed, smooth_factor, r, g, b, a);

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

    /* Set empty input region for click passthrough */
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

    /* Wait for configure */
    wl_display_roundtrip(display);
    if (!configured) {
        LOG_ERROR("Layer surface not configured");
        return 1;
    }
    LOG_INFO("Layer surface ready: %dx%d", surface_width, surface_height);

    /* Setup control socket */
    if (!socket_path) {
        socket_path = getenv("XDG_RUNTIME_DIR");
        if (socket_path) {
            static char sock_buf[256];
            snprintf(sock_buf, sizeof(sock_buf), "%s/mouse-trail.sock", socket_path);
            socket_path = sock_buf;
        } else {
            socket_path = "/tmp/mouse-trail.sock";
        }
    }
    setup_control_socket(socket_path);

    /* Start input thread */
    start_time_ms = get_time_ms();
    pthread_create(&input_thread, NULL, input_thread_fn, NULL);

    /* Setup timer for rendering */
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec its = {
        .it_interval = { .tv_sec = 0, .tv_nsec = 16666667 }, /* ~60fps */
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

                /* Feed trail from input */
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

                /* Check stationary */
                if (trail.stationary_start > 0 && trail.count > 0) {
                    uint64_t idle = get_time_ms() - trail.stationary_start;
                    if (idle > trail.max_age_ms * 2 && need_redraw) {
                        need_redraw = 0;
                    }
                }

                if (need_redraw || trail.count > 0) {
                    render_frame();
                    need_redraw = 0;
                }
            } else if (events[i].data.fd == ctrl_fd) {
                int client = accept(ctrl_fd, NULL, NULL);
                if (client >= 0) {
                    char buf[256];
                    ssize_t nread = read(client, buf, sizeof(buf) - 1);
                    if (nread > 0) {
                        buf[nread] = '\0';
                        /* Strip trailing newline */
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
```

- [ ] **Step 2: Build with Nix for testing**

```bash
nix-shell -p gcc pkg-config wayland cairo libevdev --run 'cd /home/baizhu945/.config/home-manager/mouse-trail && make clean && make'
```

---

### Task 5: Test Script (log verification)

**Files:**
- Create: `test-mouse-trail.sh`

- [ ] **Step 1: Write `test-mouse-trail.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_FILE="/tmp/mouse-trail-test.log"
BINARY="$SCRIPT_DIR/mouse-trail"
PIDFILE="/tmp/mouse-trail-test.pid"
RET=0

cleanup() {
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null || true
        rm -f "$PIDFILE"
    fi
    rm -f /tmp/mouse-trail.sock
}
trap cleanup EXIT

echo "=== Test 1: Basic startup and log output ==="
"$BINARY" --log-file "$LOG_FILE" --log-level debug --socket /tmp/mouse-trail.sock &
PID=$!
echo "$PID" > "$PIDFILE"
sleep 2

if kill -0 "$PID" 2>/dev/null; then
    echo "PASS: Process running (PID=$PID)"
else
    echo "FAIL: Process died"
    cat "$LOG_FILE"
    exit 1
fi

echo "=== Test 2: Log file contains expected messages ==="
for pattern in \
    "Starting mouse-trail" \
    "Opened input device" \
    "Connected to Wayland" \
    "Input region set empty" \
    "Layer surface ready" \
    "Entering main loop"; do
    if grep -q "$pattern" "$LOG_FILE"; then
        echo "PASS: Found '$pattern'"
    else
        echo "FAIL: Missing '$pattern'"
        RET=1
    fi
done

echo "=== Test 3: Click passthrough configured ==="
if grep -q "Input region set empty.*passthrough" "$LOG_FILE"; then
    echo "PASS: Click passthrough configured"
else
    echo "FAIL: Click passthrough not found in logs"
    RET=1
fi

echo "=== Test 4: Graceful shutdown ==="
kill "$PID"
sleep 1
if ! kill -0 "$PID" 2>/dev/null; then
    echo "PASS: Process terminated cleanly"
else
    echo "FAIL: Process didn't die"
    kill -9 "$PID" 2>/dev/null || true
    RET=1
fi

echo "=== Test 5: Error on invalid device ==="
"$BINARY" --device /dev/input/nonexistent --log-file "$LOG_FILE" 2>/dev/null || true
if grep -q "Cannot open" "$LOG_FILE"; then
    echo "PASS: Proper error on invalid device"
else
    echo "FAIL: No error for invalid device"
    RET=1
fi

echo ""
if [ "$RET" -eq 0 ]; then
    echo "=== ALL TESTS PASSED ==="
else
    echo "=== SOME TESTS FAILED ==="
fi
echo "Full log: $LOG_FILE"
exit $RET
```

- [ ] **Step 2: Run test without compositor (tests error handling)**

```bash
chmod +x /home/baizhu945/.config/home-manager/mouse-trail/test-mouse-trail.sh
# When no Wayland compositor, should handle errors gracefully
bash /home/baizhu945/.config/home-manager/mouse-trail/test-mouse-trail.sh
```

---

### Task 6: Nix Module + Toggle/Ctl Scripts

**Files:**
- Create: `mouse-trail.nix`

- [ ] **Step 1: Write `mouse-trail.nix`**

```nix
{ pkgs, lib, config, ... }:

let
  src = ./.;

  mouse-trail-pkg = pkgs.stdenv.mkDerivation {
    pname = "mouse-trail";
    version = "0.1.0";

    src = src;

    nativeBuildInputs = with pkgs; [ pkg-config ];
    buildInputs = with pkgs; [ wayland cairo libevdev ];

    buildPhase = ''
      mkdir -p build
      gcc -Wall -Wextra -O2 -g \
        $NIX_CFLAGS_COMPILE \
        -I src \
        src/trail.c \
        src/wlr-layer-shell-client-protocol.c \
        src/main.c \
        -o mouse-trail \
        -lm -lwayland-client -lcairo -levdev \
        $NIX_LDFLAGS
    '';

    installPhase = ''
      mkdir -p $out/bin
      cp mouse-trail $out/bin/
    '';

    meta = with lib; {
      description = "Mouse trail overlay effect for Wayland/niri";
      license = licenses.mit;
      platforms = platforms.linux;
    };
  };

  toggle-script = pkgs.writeShellScriptBin "mouse-trail-toggle" ''
    #!/usr/bin/env bash
    PIDFILE="/tmp/mouse-trail.pid"
    SOCK="$XDG_RUNTIME_DIR/mouse-trail.sock"

    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE" "$SOCK"
        notify-send "Mouse Trail" "已关闭" 2>/dev/null || \
            ${pkgs.libnotify}/bin/notify-send "Mouse Trail" "已关闭" 2>/dev/null || true
    else
        rm -f "$PIDFILE" "$SOCK"
        ${mouse-trail-pkg}/bin/mouse-trail \
            --width 8 \
            --length 500 \
            --min-speed 2 \
            --smooth-factor 0.6 \
            --color "#80c8ff" \
            --log-level info \
            &
        echo $! > "$PIDFILE"
        sleep 0.1
        notify-send "Mouse Trail" "已开启" 2>/dev/null || \
            ${pkgs.libnotify}/bin/notify-send "Mouse Trail" "已开启" 2>/dev/null || true
    fi
  '';

  ctl-script = pkgs.writeShellScriptBin "mouse-trail-ctl" ''
    #!/usr/bin/env bash
    SOCK="$XDG_RUNTIME_DIR/mouse-trail.sock"
    if [ ! -S "$SOCK" ]; then
        echo "mouse-trail is not running (socket $SOCK not found)" >&2
        exit 1
    fi
    echo "$*" | nc -U -q 0 "$SOCK"
  '';

in
{
  home.packages = [
    mouse-trail-pkg
    toggle-script
    ctl-script
  ];
}
```

- [ ] **Step 2: Add import to `home.nix`**

In `/home/baizhu945/.config/home-manager/home.nix`, add to the `imports` list:

```nix
    ./mouse-trail/mouse-trail.nix
```

---

### Task 7: Final Integration & Verification

- [ ] **Step 1: Run trail unit tests**

```bash
cd /home/baizhu945/.config/home-manager/mouse-trail && nix-shell -p gcc --run 'make test'
```

- [ ] **Step 2: Build full binary in Nix env**

```bash
nix-shell -p gcc pkg-config wayland cairo libevdev --run 'cd /home/baizhu945/.config/home-manager/mouse-trail && make clean && make'
```

- [ ] **Step 3: Run log verification test**

```bash
bash /home/baizhu945/.config/home-manager/mouse-trail/test-mouse-trail.sh
```

- [ ] **Step 4: Verify toggle script logic**

```bash
# Not running under Wayland, so just check script syntax exists
bash -n /nix/store/.../bin/mouse-trail-toggle  # (after Nix build)
```

- [ ] **Step 5: Verify Nix module**

```bash
# Build the derivation (no switch needed)
nix-build -E 'let pkgs = import <nixpkgs> {}; in pkgs.callPackage /home/baizhu945/.config/home-manager/mouse-trail/mouse-trail.nix {}'
```

**Expect:** Successful build, binary at `result/bin/mouse-trail`
