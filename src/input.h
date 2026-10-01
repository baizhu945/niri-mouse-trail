#ifndef MOUSE_TRAIL_INPUT_H
#define MOUSE_TRAIL_INPUT_H

#include <linux/input.h>

#ifdef __cplusplus
extern "C" {
#endif

struct input_manager;
typedef struct { int super, shift, ctrl, alt; } input_modifiers_t;
typedef void (*input_motion_cb)(void *user, double dx, double dy, int normalized);
typedef void (*input_key_cb)(void *user, const struct input_event *event,
                             const input_modifiers_t *mods);

/* Paths are copied; NULL selects auto-discovery only. Explicit paths augment,
 * rather than replace, auto-discovery. Callbacks run on the input worker and
 * must not block or call start/destroy. Callback arguments are borrowed only
 * for the duration of the call. Consumers synchronize their own shared state.
 *
 * normalized=0: raw REL pixels; normalized=1: ABS delta / each valid axis range
 * (an invalid range contributes zero). ABS deltas are delivered at SYN_REPORT.
 * Key callbacks include presses, releases and repeats, including modifiers and
 * non-modifier keys. mods is the OR of all online keyboards' left/right keys,
 * after the event. SYN_DROPPED recovery updates state without synthesizing
 * shortcut callbacks or motion; ABS baselines restart after recovery.
 */
struct input_manager *input_manager_create(const char *mouse_path,
                                           const char *keyboard_path,
                                           input_motion_cb motion,
                                           input_key_cb key, void *user);
/* Returns 0 on thread creation, -1 with errno on failure. No devices is valid;
 * discovery/access failures are retried asynchronously. Calls to lifecycle
 * functions must be serialized by the caller; start may only succeed once. */
int input_manager_start(struct input_manager *manager);
/* NULL is allowed. Cooperatively wakes and joins the worker (no cancellation).
 * Do not call from a callback, or concurrently with another lifecycle call. */
void input_manager_destroy(struct input_manager *manager);

#ifdef __cplusplus
}
#endif
#endif
