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
    LOG_INFO("trail_init: width=%.2f length=%lu ms min_speed=%.2f smooth=%.2f",
             width, (unsigned long)length_ms, min_speed, smooth_factor);
}

void trail_set_position(trail_state_t *t, double x, double y) {
    t->pos_x = x;
    t->pos_y = y;
    LOG_INFO("trail_set_position: (%.1f, %.1f)", x, y);
}

int trail_feed(trail_state_t *t, double rel_x, double rel_y, uint64_t now_ms) {
    if (!t->visible) return 0;
    if (rel_x == 0.0 && rel_y == 0.0) return 0;

    /* Shift all existing points away from cursor by the movement delta */
    for (int i = 0; i < t->count; i++) {
        int idx = (t->head + i) % MAX_TRAIL_POINTS;
        t->points[idx].x -= rel_x;
        t->points[idx].y -= rel_y;
    }

    /* Add new point at (0,0) — current cursor position in trail-local coords */
    int idx = (t->head + t->count) % MAX_TRAIL_POINTS;
    t->points[idx].x = 0.0;
    t->points[idx].y = 0.0;
    t->points[idx].timestamp_ms = now_ms;

    if (t->count < MAX_TRAIL_POINTS) {
        t->count++;
    } else {
        t->head = (t->head + 1) % MAX_TRAIL_POINTS;
    }

    /* Update estimated cursor position */
    t->pos_x += rel_x;
    t->pos_y += rel_y;

    LOG_DEBUG("trail: point added pos=(%.1f,%.1f) count=%d",
              t->pos_x, t->pos_y, t->count);
    return 1;
}

int trail_cleanup(trail_state_t *t, uint64_t now_ms) {
    uint64_t stationary_age = (t->stationary_start > 0) ? (now_ms - t->stationary_start) : 0;
    int removed = 0;

    while (t->count > 0) {
        trail_point_t *pt = &t->points[t->head];
        uint64_t age = now_ms - pt->timestamp_ms;
        if (age <= t->max_age_ms && stationary_age <= t->max_age_ms * 2)
            break;
        t->head = (t->head + 1) % MAX_TRAIL_POINTS;
        t->count--;
        removed++;
    }

    if (removed > 0) {
        LOG_DEBUG("trail_cleanup: removed=%d remaining=%d stationary_age=%lums",
                  removed, t->count, (unsigned long)stationary_age);
    }
    return t->count;
}

int trail_render(trail_state_t *t, uint64_t now_ms,
                  trail_render_cb cb, void *user) {
    if (!t->visible) return 0;

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
        LOG_DEBUG("trail_render: rendered=%d points stationary_age=%lu",
                  rendered, (unsigned long)stationary_age);
    }
    return rendered;
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
int g_log_level = 3;

static int render_count = 0;

static void test_cb(void *user, double x, double y, double radius, double alpha,
                     double r, double g, double b) {
    (void)user; (void)r; (void)g; (void)b;
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
    printf("PASS\n");

    printf("=== Test 2: trail_set_position ===\n");
    trail_set_position(&t, 640.0, 480.0);
    assert(t.pos_x == 640.0 && t.pos_y == 480.0);
    printf("PASS\n");

    printf("=== Test 3: trail_feed adds point at (0,0) ===\n");
    uint64_t t0 = 1000;
    int moved = trail_feed(&t, 10.0, 5.0, t0);
    assert(moved == 1);
    assert(t.count == 1);
    assert(t.points[t.head].x == 0.0 && t.points[t.head].y == 0.0);
    assert(t.pos_x == 650.0 && t.pos_y == 485.0);
    printf("PASS: count=%d pos=(%.1f,%.1f)\n", t.count, t.pos_x, t.pos_y);

    printf("=== Test 4: second movement shifts old point away ===\n");
    trail_feed(&t, 20.0, 0.0, t0 + 10);
    assert(t.count == 2);
    /* Old point at head should be at (-20, 0) — shifted left */
    assert(t.points[t.head].x == -20.0 && t.points[t.head].y == 0.0);
    /* New point at tail should be at (0, 0) */
    int new_idx = (t.head + 1) % MAX_TRAIL_POINTS;
    assert(t.points[new_idx].x == 0.0 && t.points[new_idx].y == 0.0);
    assert(t.pos_x == 670.0);
    printf("PASS: old=(-20,0) new=(0,0) pos_x=%.1f\n", t.pos_x);

    printf("=== Test 5: trail_render relative coords ===\n");
    render_count = 0;
    int rc = trail_render(&t, t0 + 20, test_cb, NULL);
    assert(rc == 2);
    assert(render_count == 2);
    printf("PASS: %d points\n", rc);

    printf("=== Test 6: trail_render after expiry ===\n");
    render_count = 0;
    rc = trail_render(&t, t0 + 600, test_cb, NULL);
    assert(rc == 0);
    printf("PASS: expired\n");

    printf("=== Test 7: trail_cleanup removes expired ===\n");
    t.stationary_start = t0;
    trail_cleanup(&t, t0 + 2000);
    assert(t.count == 0);
    printf("PASS: all cleaned\n");

    printf("=== Test 8: zero delta returns 0 ===\n");
    trail_init(&t, 8.0, 500, 2.0, 0.6, 1.0, 1.0, 1.0, 1.0);
    trail_set_position(&t, 0.0, 0.0);
    moved = trail_feed(&t, 0.0, 0.0, 2000);
    assert(moved == 0);
    printf("PASS: zero delta skipped\n");

    printf("\n=== ALL TRAIL TESTS PASSED (8/8) ===\n");
    return 0;
}
#endif
