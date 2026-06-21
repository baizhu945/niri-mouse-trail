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

/* ===== Built-in test ===== */
#ifdef TRAIL_TEST

#include <stdio.h>
#include <assert.h>

FILE *g_log_file = NULL;
int g_log_level = 0;

static int render_count = 0;

static void test_cb(void *user, double x, double y, double radius, double alpha,
                     double r, double g, double b) {
    (void)user; (void)x; (void)y; (void)r; (void)g; (void)b;
    assert(radius >= 0.0 && radius <= 8.0);
    assert(alpha >= 0.0 && alpha <= 1.0);
    render_count++;
}

int main(void) {
    log_init(stderr, 3);

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

    printf("=== Test 7: EMA filter (no smoothing) ===\n");
    trail_init(&t, 8.0, 500, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0);
    trail_feed(&t, 10.0, 0.0, 1000);
    trail_feed(&t, 10.0, 0.0, 1010);
    assert(t.filtered_x > 9.0);
    printf("PASS: filtered_x=%.2f (alpha=0)\n", t.filtered_x);

    printf("=== Test 8: EMA filter (heavy smoothing) ===\n");
    trail_init(&t, 8.0, 500, 1.0, 0.9, 1.0, 1.0, 1.0, 1.0);
    trail_feed(&t, 10.0, 0.0, 1000);
    trail_feed(&t, 10.0, 0.0, 1010);
    trail_feed(&t, 10.0, 0.0, 1020);
    assert(t.filtered_x < 10.0);
    printf("PASS: filtered_x=%.2f (alpha=0.9)\n", t.filtered_x);

    printf("\n=== ALL TRAIL TESTS PASSED ===\n");
    return 0;
}

#endif
