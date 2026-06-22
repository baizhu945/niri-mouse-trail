#ifndef TRAIL_H
#define TRAIL_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_TRAIL_POINTS 1024

typedef struct {
    double x, y;          // offset from current cursor position
    uint64_t timestamp_ms;
} trail_point_t;

typedef struct {
    trail_point_t points[MAX_TRAIL_POINTS];
    int head;
    int count;
    double pos_x;          // estimated cursor global position
    double pos_y;
    double max_radius;
    uint64_t max_age_ms;
    double min_speed;
    double smooth_factor;
    bool visible;
    uint64_t stationary_start;
    double r, g, b, a;
} trail_state_t;

void trail_init(trail_state_t *t, double width, uint64_t length_ms,
                double min_speed, double smooth_factor,
                double r, double g, double b, double a);

void trail_set_position(trail_state_t *t, double x, double y);

int trail_feed(trail_state_t *t, double rel_x, double rel_y, uint64_t now_ms);

int trail_cleanup(trail_state_t *t, uint64_t now_ms);

typedef void (*trail_render_cb)(void *user, double x, double y,
                                 double radius, double alpha,
                                 double r, double g, double b);
int trail_render(trail_state_t *t, uint64_t now_ms,
                  trail_render_cb cb, void *user);

void trail_set_color(trail_state_t *t, double r, double g, double b, double a);
void trail_set_color_rgb(trail_state_t *t, double r, double g, double b);

#endif
