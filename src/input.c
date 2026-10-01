#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "input.h"
#include "log.h"
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define SCAN_MS 1500
#define MONITOR_BACKOFF_MAX_MS 12000
#define READ_BUDGET 128
#define UDEV_BUDGET 64
#define EXPLICIT_MOUSE 1u
#define EXPLICIT_KEYBOARD 2u

enum motion_kind { MOTION_NONE, MOTION_REL, MOTION_ABS };
struct input_node {
    struct input_node *next;
    char *path, *syspath;
    struct stat identity;
    struct libevdev *dev;
    int fd;
    unsigned explicit_roles;
    int seen, inspected, warned;
    uint64_t retry_at;
    int rel_xy, abs_xy, rel_mouse, auto_keyboard;
    int keyboard, suppressed, syncing, backlog;
    enum motion_kind motion;
    int abs_has[2];
    double abs_last[2], abs_pending[2];
#ifdef INPUT_TEST
    int (*test_next)(struct input_node *, unsigned, struct input_event *);
    void *test_data;
#endif
};

struct input_manager {
    char *mouse_path, *keyboard_path;
    input_motion_cb motion;
    input_key_cb key;
    void *user;
    pthread_t thread;
    int started, wake_fd;
    _Atomic int stopping;
    /* Everything below is owned exclusively by the worker after start. */
    struct input_node *nodes;
    struct udev *udev;
    struct udev_monitor *monitor;
    uint64_t scan_at, monitor_at;
    unsigned monitor_backoff;
    struct pollfd *pollfds;
    struct input_node **pollnodes;
    size_t pollcapacity;
#ifdef INPUT_TEST
    int test_offline, test_ready_fd;
    _Atomic unsigned test_polls;
#endif
};

#ifdef INPUT_TEST
static unsigned test_frees, test_closes;
#endif

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void reset_abs(struct input_node *n) {
    memset(n->abs_has, 0, sizeof(n->abs_has));
    memset(n->abs_last, 0, sizeof(n->abs_last));
    memset(n->abs_pending, 0, sizeof(n->abs_pending));
}

/* Idempotent: never leave dead descriptors or their key state in the poll set. */
static void release_device(struct input_node *n) {
    if (n->dev) {
        libevdev_free(n->dev);
        n->dev = NULL;
#ifdef INPUT_TEST
        test_frees++;
#endif
    }
    if (n->fd >= 0) {
        /* Do not retry close on EINTR: Linux has already released the fd. */
        close(n->fd);
        n->fd = -1;
#ifdef INPUT_TEST
        test_closes++;
#endif
    }
    n->keyboard = n->suppressed = n->syncing = n->backlog = 0;
    n->motion = MOTION_NONE;
    reset_abs(n);
}

static void forget_node(struct input_node **link) {
    struct input_node *n = *link;
    *link = n->next;
    release_device(n);
    free(n->path);
    free(n->syspath);
    free(n);
}

static int has_code(struct input_node *n, unsigned type, unsigned code) {
    return libevdev_has_event_code(n->dev, type, code);
}

static void classify(struct input_node *n) {
    if (!n->dev) return;
    n->rel_xy = has_code(n, EV_REL, REL_X) && has_code(n, EV_REL, REL_Y);
    n->abs_xy = has_code(n, EV_ABS, ABS_X) && has_code(n, EV_ABS, ABS_Y);
    n->rel_mouse = n->rel_xy && has_code(n, EV_KEY, BTN_LEFT);
    n->auto_keyboard = has_code(n, EV_KEY, KEY_A) && has_code(n, EV_KEY, KEY_ESC);
    n->keyboard = n->auto_keyboard || (n->explicit_roles & EXPLICIT_KEYBOARD);
    enum motion_kind old = n->motion;
    n->motion = n->rel_mouse || (n->rel_xy && (n->explicit_roles & EXPLICIT_MOUSE))
              ? MOTION_REL : n->abs_xy ? MOTION_ABS : MOTION_NONE;
    if (old != n->motion) reset_abs(n);
}

/* phys is a motion preference, NOT device identity. Keep suppressed REL nodes
 * open so removal of the ABS sibling immediately restores them. In particular
 * never discard a keyboard composite: only its motion stream is suppressed. */
static void update_preferences(struct input_manager *m) {
    for (struct input_node *n = m->nodes; n; n = n->next) {
        n->suppressed = 0;
        if (!n->dev || n->motion != MOTION_REL) continue;
        const char *phys = libevdev_get_phys(n->dev);
        if (!phys || !*phys) continue;
        for (struct input_node *a = m->nodes; a; a = a->next) {
            if (!a->dev || a->motion != MOTION_ABS || a->rel_xy || a->keyboard)
                continue;
            const char *other = libevdev_get_phys(a->dev);
            if (other && strcmp(phys, other) == 0) {
                n->suppressed = 1;
                break;
            }
        }
    }
}

static input_modifiers_t modifiers(struct input_manager *m) {
    input_modifiers_t mods = {0};
    for (struct input_node *n = m->nodes; n; n = n->next) {
        if (!n->dev || n->fd < 0 || !n->keyboard) continue;
#define DOWN(code) (libevdev_get_event_value(n->dev, EV_KEY, (code)) > 0)
        mods.super |= DOWN(KEY_LEFTMETA) || DOWN(KEY_RIGHTMETA);
        mods.shift |= DOWN(KEY_LEFTSHIFT) || DOWN(KEY_RIGHTSHIFT);
        mods.ctrl  |= DOWN(KEY_LEFTCTRL) || DOWN(KEY_RIGHTCTRL);
        mods.alt   |= DOWN(KEY_LEFTALT) || DOWN(KEY_RIGHTALT);
#undef DOWN
    }
    return mods;
}

static void process_event(struct input_manager *m, struct input_node *n,
                          const struct input_event *ev) {
    if (n->keyboard && ev->type == EV_KEY && m->key) {
        input_modifiers_t mods = modifiers(m);
        m->key(m->user, ev, &mods);
    }
    if (n->suppressed || n->motion == MOTION_NONE) return;
    if (n->motion == MOTION_REL) {
        if (ev->type == EV_REL && (ev->code == REL_X || ev->code == REL_Y) &&
            ev->value && m->motion)
            m->motion(m->user, ev->code == REL_X ? ev->value : 0,
                      ev->code == REL_Y ? ev->value : 0, 0);
        return;
    }
    if (ev->type == EV_ABS && (ev->code == ABS_X || ev->code == ABS_Y)) {
        unsigned axis = ev->code == ABS_Y;
        if (n->abs_has[axis]) n->abs_pending[axis] += ev->value - n->abs_last[axis];
        n->abs_last[axis] = ev->value;
        n->abs_has[axis] = 1;
    } else if (ev->type == EV_KEY && ev->code == BTN_TOUCH && !ev->value) {
        reset_abs(n);
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        double delta[2] = {0};
        for (unsigned axis = 0; axis < 2; axis++) {
            unsigned code = axis ? ABS_Y : ABS_X;
            /* Convert before subtracting, so extreme signed ranges cannot wrap. */
            double range = (double)libevdev_get_abs_maximum(n->dev, code) -
                           (double)libevdev_get_abs_minimum(n->dev, code);
            if (range > 0) delta[axis] = n->abs_pending[axis] / range;
            n->abs_pending[axis] = 0;
        }
        if (m->motion && (delta[0] || delta[1]))
            m->motion(m->user, delta[0], delta[1], 1);
    }
}

static int next_event(struct input_node *n, unsigned flags, struct input_event *ev) {
#ifdef INPUT_TEST
    if (n->test_next) return n->test_next(n, flags, ev);
#endif
    return libevdev_next_event(n->dev, flags, ev);
}

static void device_error(struct input_node *n, int error) {
    LOG_WARN("Input removed/unavailable: %s (%s)", n->path, strerror(error));
    release_device(n);
    n->inspected = 0;
    n->retry_at = now_ms() + SCAN_MS;
}

static void drain_device(struct input_manager *m, struct input_node *n) {
    n->backlog = 0;
    for (unsigned count = 0; count < READ_BUDGET; count++) {
        if (atomic_load(&m->stopping)) return;
        struct input_event ev;
        int rc = next_event(n, n->syncing ? LIBEVDEV_READ_FLAG_SYNC :
                                           LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            if (!n->syncing) {
                LOG_WARN("Input queue overrun: %s; resynchronizing", n->path);
                n->syncing = 1;
                reset_abs(n);
            }
            /* libevdev updates its per-device state. Synthetic sync events
             * must not move the cursor or fire monitor-switch shortcuts. */
            continue;
        }
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
            if (n->syncing) { n->syncing = 0; reset_abs(n); }
            process_event(m, n, &ev);
            continue;
        }
        if (rc == -EINTR) continue; /* Counts towards the fairness budget. */
        if (rc == -EAGAIN) {
            if (n->syncing) {
                n->syncing = 0;
                reset_abs(n);
                n->backlog = 1; /* A normal event may already be buffered. */
            }
            return;
        }
        device_error(n, rc < 0 ? -rc : EIO);
        return;
    }
    /* libevdev may have buffered events although the kernel fd is no longer
     * readable. Schedule one bounded turn, without waiting for another POLLIN. */
    n->backlog = 1;
}

static void handle_device_poll(struct input_manager *m, struct input_node *n,
                               short revents) {
    if (n->fd < 0 || !n->dev) return;
    if (revents & (POLLHUP | POLLERR | POLLNVAL)) {
        device_error(n, revents & POLLNVAL ? EBADF : ENODEV);
        return; /* Never read a descriptor reporting a fatal poll condition. */
    }
    if ((revents & POLLIN) || n->backlog) drain_device(m, n);
}

static int same_text(const char *a, const char *b) {
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

static int same_inode(const struct stat *a, const struct stat *b) {
    return a->st_rdev == b->st_rdev && a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

/* Return 0=unrelated, 1=same device/alias, 2=identity replaced. */
static int identity_relation(struct input_node *n, const char *path,
                             const char *syspath, const struct stat *st) {
    int same_path = same_text(n->path, path);
    int same_sys = syspath && same_text(n->syspath, syspath);
    if (!same_path && !same_sys && n->identity.st_rdev != st->st_rdev) return 0;
    if (n->identity.st_rdev != st->st_rdev || !same_text(n->syspath, syspath) ||
        (same_path && !same_inode(&n->identity, st))) return 2;
    return 1;
}

static void try_open(struct input_node *n, uint64_t now) {
    if (n->dev || n->inspected || now < n->retry_at) return;
    int fd = open(n->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int error = fd < 0 ? errno : 0;
    struct stat st;
    struct libevdev *dev = NULL;
    if (fd >= 0) {
        if (fstat(fd, &st) < 0) error = errno;
        else if (!same_inode(&st, &n->identity)) error = ENODEV;
        else {
            int rc = libevdev_new_from_fd(fd, &dev);
            if (rc < 0) error = -rc;
        }
    }
    if (error) {
        if (dev) libevdev_free(dev);
        if (fd >= 0) close(fd);
        if (!n->warned) {
            LOG_WARN("Cannot open input %s: %s (will retry)", n->path, strerror(error));
            n->warned = 1;
        }
        n->retry_at = now + SCAN_MS;
        return;
    }
    n->fd = fd;
    n->dev = dev;
    n->warned = 0;
    n->inspected = 1;
    classify(n);
    if (!n->keyboard && n->motion == MOTION_NONE && !n->explicit_roles) {
        /* Cache the inspected identity, not the descriptor: fallback scans do
         * not repeatedly open nonmatching switches/consumer-control nodes. */
        release_device(n);
        return;
    }
    LOG_INFO("Input online: %s (%s), motion=%s keyboard=%d",
             libevdev_get_name(dev) ? libevdev_get_name(dev) : "unnamed", n->path,
             n->motion == MOTION_REL ? "REL" : n->motion == MOTION_ABS ? "ABS" : "none",
             n->keyboard);
}

/* Only stat/udev lookups for known identities. Canonical udev devnodes also
 * deduplicate /dev/input/by-id links and alternate mknod aliases by rdev. */
static void discover_path(struct input_manager *m, const char *path, unsigned roles,
                          uint64_t now) {
    struct stat st;
    if (!path || stat(path, &st) < 0 || !S_ISCHR(st.st_mode)) return;
    struct udev_device *ud = m->udev ? udev_device_new_from_devnum(m->udev, 'c', st.st_rdev) : NULL;
    const char *devnode = ud ? udev_device_get_devnode(ud) : NULL;
    char *canonical = realpath(devnode ? devnode : path, NULL);
    const char *sys = ud ? udev_device_get_syspath(ud) : NULL;
    char syslink[80];
    if (!sys) {
        snprintf(syslink, sizeof(syslink), "/sys/dev/char/%u:%u", major(st.st_rdev), minor(st.st_rdev));
        sys = syslink;
    }
    char *syspath = realpath(sys, NULL);
    struct stat actual;
    if (!canonical || stat(canonical, &actual) < 0 || actual.st_rdev != st.st_rdev) goto out;
    st = actual;
    struct input_node *n = NULL;
    for (struct input_node **link = &m->nodes; *link;) {
        int relation = identity_relation(*link, canonical, syspath, &st);
        if (relation == 2) { forget_node(link); continue; }
        if (relation == 1) { n = *link; break; }
        link = &(*link)->next;
    }
    if (!n) {
        n = calloc(1, sizeof(*n));
        if (!n) goto out;
        n->fd = -1;
        n->path = canonical;
        n->syspath = syspath;
        canonical = syspath = NULL;
        n->identity = st;
        n->next = m->nodes;
        m->nodes = n;
    }
    n->seen = 1;
    if (roles & ~n->explicit_roles) {
        /* A previously rejected node can become an explicit device. */
        if (!n->dev) n->inspected = 0;
    }
    n->explicit_roles |= roles;
    try_open(n, now);
out:
    free(canonical);
    free(syspath);
    if (ud) udev_device_unref(ud);
}

static int is_event_name(const char *name) {
    if (!name || strncmp(name, "event", 5) || !name[5]) return 0;
    for (const char *p = name + 5; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static void rescan(struct input_manager *m, uint64_t now) {
    for (struct input_node *n = m->nodes; n; n = n->next) {
        n->seen = 0;
        n->explicit_roles = 0;
    }
    discover_path(m, m->mouse_path, EXPLICIT_MOUSE, now);
    discover_path(m, m->keyboard_path, EXPLICIT_KEYBOARD, now);
    int complete = 0;
    struct udev_enumerate *enumerate = m->udev ? udev_enumerate_new(m->udev) : NULL;
    if (enumerate && udev_enumerate_add_match_subsystem(enumerate, "input") >= 0 &&
        udev_enumerate_scan_devices(enumerate) >= 0) {
        complete = 1;
        struct udev_list_entry *entry;
        udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
            if (atomic_load(&m->stopping)) { complete = 0; break; }
            struct udev_device *ud = udev_device_new_from_syspath(m->udev, udev_list_entry_get_name(entry));
            if (!ud) { complete = 0; continue; }
            if (is_event_name(udev_device_get_sysname(ud)))
                discover_path(m, udev_device_get_devnode(ud), 0, now);
            udev_device_unref(ud);
        }
    }
    if (enumerate) udev_enumerate_unref(enumerate);
    for (struct input_node **link = &m->nodes; *link;) {
        struct input_node *n = *link;
        if (complete && !n->seen) { forget_node(link); continue; }
        classify(n);
        if (n->dev && !n->keyboard && n->motion == MOTION_NONE && !n->explicit_roles)
            release_device(n);
        link = &n->next;
    }
    update_preferences(m);
    m->scan_at = now_ms() + SCAN_MS;
}

static void monitor_lost(struct input_manager *m, uint64_t now) {
    if (m->monitor) {
        udev_monitor_unref(m->monitor);
        m->monitor = NULL;
    }
    m->monitor_at = now + m->monitor_backoff;
    if (m->monitor_backoff < MONITOR_BACKOFF_MAX_MS) m->monitor_backoff *= 2;
    if (m->monitor_backoff > MONITOR_BACKOFF_MAX_MS) m->monitor_backoff = MONITOR_BACKOFF_MAX_MS;
}

static void enable_monitor(struct input_manager *m, uint64_t now) {
    if (m->monitor || now < m->monitor_at) return;
    if (!m->udev) m->udev = udev_new();
    struct udev_monitor *mon = m->udev ? udev_monitor_new_from_netlink(m->udev, "udev") : NULL;
    if (!mon || udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL) < 0 ||
        udev_monitor_enable_receiving(mon) < 0) {
        if (mon) udev_monitor_unref(mon);
        LOG_WARN("Input udev monitor unavailable; enumeration/reconnect will retry");
        monitor_lost(m, now);
        return;
    }
    int fd = udev_monitor_get_fd(mon);
    int flags = fd >= 0 ? fcntl(fd, F_GETFL) : -1;
    int fdflags = fd >= 0 ? fcntl(fd, F_GETFD) : -1;
    if (flags < 0 || fdflags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) < 0) {
        udev_monitor_unref(mon);
        monitor_lost(m, now);
        return;
    }
    m->monitor = mon;
    m->monitor_backoff = SCAN_MS;
    /* Enable BEFORE enumerate, on initial startup and every reconnect. */
    m->scan_at = 0;
}

static void receive_udev(struct input_manager *m) {
    for (unsigned count = 0; count < UDEV_BUDGET && !atomic_load(&m->stopping); count++) {
        errno = 0;
        struct udev_device *ud = udev_monitor_receive_device(m->monitor);
        if (!ud) {
            if (errno == EINTR) continue;
            if (errno && errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_WARN("Input udev monitor lost: %s", strerror(errno));
                monitor_lost(m, now_ms());
            }
            update_preferences(m);
            return;
        }
        const char *action = udev_device_get_action(ud);
        const char *sys = udev_device_get_syspath(ud);
        if (action && sys && !strcmp(action, "remove")) {
            for (struct input_node **link = &m->nodes; *link;) {
                if (same_text((*link)->syspath, sys)) forget_node(link);
                else link = &(*link)->next;
            }
            m->scan_at = 0;
        } else if (action && is_event_name(udev_device_get_sysname(ud)) &&
                   (!strcmp(action, "add") || !strcmp(action, "change"))) {
            for (struct input_node *n = m->nodes; n; n = n->next) {
                if (same_text(n->syspath, sys) && !strcmp(action, "change")) {
                    release_device(n);
                    n->inspected = 0;
                    n->retry_at = 0;
                }
            }
            m->scan_at = 0;
        }
        udev_device_unref(ud);
    }
    update_preferences(m);
}

/* A poll snapshot is consumed before udev/scans can free its node pointers. */
static int build_pollset(struct input_manager *m, nfds_t *count, int *backlog) {
    size_t needed = 2;
    for (struct input_node *n = m->nodes; n; n = n->next) if (n->fd >= 0) needed++;
    if (needed > m->pollcapacity) {
        if (needed > SIZE_MAX / sizeof(*m->pollfds) || needed > SIZE_MAX / sizeof(*m->pollnodes)) {
            errno = ENOMEM;
            return -1;
        }
        struct pollfd *fds = malloc(needed * sizeof(*fds));
        struct input_node **nodes = malloc(needed * sizeof(*nodes));
        if (!fds || !nodes) { free(fds); free(nodes); return -1; }
        free(m->pollfds);
        free(m->pollnodes);
        m->pollfds = fds;
        m->pollnodes = nodes;
        m->pollcapacity = needed;
    }
    m->pollfds[0] = (struct pollfd){ .fd = m->wake_fd, .events = POLLIN };
    m->pollfds[1] = (struct pollfd){ .fd = m->monitor ? udev_monitor_get_fd(m->monitor) : -1,
                                  .events = POLLIN };
    *count = 2;
    *backlog = 0;
    for (struct input_node *n = m->nodes; n; n = n->next) {
        if (n->fd < 0) continue;
        m->pollfds[*count] = (struct pollfd){ .fd = n->fd, .events = POLLIN };
        m->pollnodes[*count] = n;
        (*count)++;
        *backlog |= n->backlog;
    }
    return 0;
}

static int poll_timeout(struct input_manager *m, uint64_t now, int backlog) {
    if (backlog) return 0;
#ifdef INPUT_TEST
    if (m->test_offline) return -1;
#endif
    uint64_t deadline = m->scan_at;
    if (!m->monitor && m->monitor_at < deadline) deadline = m->monitor_at;
    return deadline <= now ? 0 : deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
}

static void cleanup_worker(struct input_manager *m) {
    while (m->nodes) forget_node(&m->nodes);
    if (m->monitor) { udev_monitor_unref(m->monitor); m->monitor = NULL; }
    if (m->udev) { udev_unref(m->udev); m->udev = NULL; }
    free(m->pollfds);
    free(m->pollnodes);
    m->pollfds = NULL;
    m->pollnodes = NULL;
    m->pollcapacity = 0;
}

static void *input_worker(void *arg) {
    struct input_manager *m = arg;
    while (!atomic_load(&m->stopping)) {
        uint64_t now = now_ms();
#ifdef INPUT_TEST
        if (!m->test_offline) {
#endif
            enable_monitor(m, now);
            if (now >= m->scan_at) rescan(m, now);
#ifdef INPUT_TEST
        }
#endif
        if (atomic_load(&m->stopping)) break;
        nfds_t count;
        int backlog;
        if (build_pollset(m, &count, &backlog) < 0) {
            LOG_ERROR("Input poll allocation failed: %s", strerror(errno));
            break;
        }
        int timeout = poll_timeout(m, now_ms(), backlog);
#ifdef INPUT_TEST
        atomic_fetch_add(&m->test_polls, 1);
        if (m->test_ready_fd >= 0) {
            uint64_t one = 1;
            ssize_t written;
            do { written = write(m->test_ready_fd, &one, sizeof(one)); }
            while (written < 0 && errno == EINTR);
            if (written != (ssize_t)sizeof(one)) {
                LOG_ERROR("Input test notification failed");
                break;
            }
        }
#endif
        int rc = poll(m->pollfds, count, timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Input poll failed: %s", strerror(errno));
            break;
        }
        if (atomic_load(&m->stopping) || m->pollfds[0].revents) break;
        /* Remove every known-dead keyboard before invoking any callback. */
        for (nfds_t i = 2; i < count; i++)
            if (m->pollfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                handle_device_poll(m, m->pollnodes[i], m->pollfds[i].revents);
        update_preferences(m);
        for (nfds_t i = 2; i < count; i++)
            handle_device_poll(m, m->pollnodes[i], m->pollfds[i].revents);
        update_preferences(m);
        short monitor_events = m->pollfds[1].revents;
        if (monitor_events & (POLLHUP | POLLERR | POLLNVAL)) {
            LOG_WARN("Input udev monitor disconnected");
            monitor_lost(m, now_ms());
        } else if (m->monitor && (monitor_events & POLLIN)) receive_udev(m);
    }
    cleanup_worker(m);
    return NULL;
}

struct input_manager *input_manager_create(const char *mouse_path, const char *keyboard_path,
                                           input_motion_cb motion, input_key_cb key, void *user) {
    struct input_manager *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->wake_fd = -1;
    atomic_init(&m->stopping, 0);
    m->monitor_backoff = SCAN_MS;
#ifdef INPUT_TEST
    m->test_ready_fd = -1;
    atomic_init(&m->test_polls, 0);
#endif
    if (mouse_path && !(m->mouse_path = strdup(mouse_path))) goto fail;
    if (keyboard_path && !(m->keyboard_path = strdup(keyboard_path))) goto fail;
    m->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m->wake_fd < 0) goto fail;
    m->motion = motion;
    m->key = key;
    m->user = user;
    return m;
fail: {
    int saved = errno;
    input_manager_destroy(m);
    errno = saved;
    return NULL;
    }
}

int input_manager_start(struct input_manager *m) {
    if (!m) { errno = EINVAL; return -1; }
    if (m->started) { errno = EALREADY; return -1; }
    int rc = pthread_create(&m->thread, NULL, input_worker, m);
    if (rc) { errno = rc; return -1; }
    m->started = 1;
    return 0;
}

void input_manager_destroy(struct input_manager *m) {
    if (!m) return;
    if (m->started) {
        atomic_store(&m->stopping, 1);
        uint64_t one = 1;
        ssize_t rc;
        do { rc = write(m->wake_fd, &one, sizeof(one)); } while (rc < 0 && errno == EINTR);
        /* EAGAIN means an existing eventfd counter already wakes the worker. */
        pthread_join(m->thread, NULL);
    } else cleanup_worker(m);
    if (m->wake_fd >= 0) close(m->wake_fd);
    free(m->mouse_path);
    free(m->keyboard_path);
    free(m);
}

#ifdef INPUT_TEST
/* In-memory evdev state plus anonymous fds only: no /dev/input or uinput. */
#include <assert.h>
#include <math.h>
#include <sys/timerfd.h>
FILE *g_log_file = NULL;
int g_log_level = 3;

struct test_capture {
    unsigned motions, keys;
    double dx, dy;
    int normalized;
    input_modifiers_t mods;
    struct input_event key;
};

static void test_motion(void *user, double dx, double dy, int normalized) {
    struct test_capture *c = user;
    c->motions++;
    c->dx = dx;
    c->dy = dy;
    c->normalized = normalized;
}

static void test_key(void *user, const struct input_event *ev, const input_modifiers_t *mods) {
    struct test_capture *c = user;
    c->keys++;
    c->key = *ev;
    c->mods = *mods;
}

static struct input_node *test_node(struct input_manager *m, int keyboard, int rel, int abs) {
    struct input_node *n = calloc(1, sizeof(*n));
    assert(n);
    n->path = strdup("anonymous-test-device");
    n->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    n->dev = libevdev_new();
    assert(n->path && n->fd >= 0 && n->dev);
    libevdev_set_name(n->dev, "test");
    if (keyboard) {
        unsigned keys[] = {KEY_A, KEY_ESC, KEY_LEFTMETA, KEY_RIGHTMETA,
                           KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL,
                           KEY_LEFTALT, KEY_RIGHTALT};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            assert(libevdev_enable_event_code(n->dev, EV_KEY, keys[i], NULL) == 0);
    }
    if (rel) {
        assert(libevdev_enable_event_code(n->dev, EV_REL, REL_X, NULL) == 0);
        assert(libevdev_enable_event_code(n->dev, EV_REL, REL_Y, NULL) == 0);
        assert(libevdev_enable_event_code(n->dev, EV_KEY, BTN_LEFT, NULL) == 0);
    }
    if (abs) {
        struct input_absinfo x = {.minimum = 100, .maximum = 1100};
        struct input_absinfo y = {.minimum = -500, .maximum = 1500};
        assert(libevdev_enable_event_code(n->dev, EV_ABS, ABS_X, &x) == 0);
        assert(libevdev_enable_event_code(n->dev, EV_ABS, ABS_Y, &y) == 0);
        assert(libevdev_enable_event_code(n->dev, EV_KEY, BTN_TOUCH, NULL) == 0);
    }
    classify(n);
    n->next = m->nodes;
    m->nodes = n;
    return n;
}

static void test_send(struct input_manager *m, struct input_node *n,
                       unsigned type, unsigned code, int value) {
    struct input_event ev = {.type = type, .code = code, .value = value};
    if (type == EV_KEY || type == EV_ABS)
        assert(libevdev_set_event_value(n->dev, type, code, value) == 0);
    process_event(m, n, &ev);
}

struct test_step { int rc; unsigned type, code; int value; };
struct test_script { const struct test_step *steps; size_t count, pos; unsigned calls, sync_calls; int endless; };

static int test_next(struct input_node *n, unsigned flags, struct input_event *ev) {
    struct test_script *s = n->test_data;
    s->calls++;
    if (flags == LIBEVDEV_READ_FLAG_SYNC) s->sync_calls++;
    if (s->endless) {
        *ev = (struct input_event){.type = EV_REL, .code = REL_X, .value = 1};
        return LIBEVDEV_READ_STATUS_SUCCESS;
    }
    if (s->pos == s->count) return -EAGAIN;
    const struct test_step *step = &s->steps[s->pos++];
    *ev = (struct input_event){.type = step->type, .code = step->code, .value = step->value};
    if (step->rc >= 0 && (ev->type == EV_KEY || ev->type == EV_ABS))
        assert(libevdev_set_event_value(n->dev, ev->type, ev->code, ev->value) == 0);
    return step->rc;
}

static void test_fatal_fds(void) {
    const short conditions[] = {POLLHUP, POLLNVAL, POLLERR, 0};
    for (size_t i = 0; i < sizeof(conditions) / sizeof(conditions[0]); i++) {
        struct input_manager *m = input_manager_create(NULL, NULL, NULL, NULL, NULL);
        assert(m);
        struct input_node *n = test_node(m, 1, 0, 0);
        struct test_step step = {.rc = -ENODEV};
        struct test_script script = {.steps = &step, .count = 1};
        n->test_next = test_next;
        n->test_data = &script;
        int fd = n->fd;
        short revents = POLLIN | conditions[i];
        if (conditions[i] == POLLHUP) {
            close(n->fd);
            int p[2];
            assert(pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0);
            n->fd = fd = p[0];
            close(p[1]);
            struct pollfd f = {.fd = fd, .events = POLLIN};
            assert(poll(&f, 1, 0) == 1 && (f.revents & POLLHUP));
            revents |= f.revents;
        } else if (conditions[i] == POLLNVAL) {
            close(fd);
            struct pollfd f = {.fd = fd, .events = POLLIN};
            assert(poll(&f, 1, 0) == 1 && (f.revents & POLLNVAL));
            revents |= f.revents;
        }
        unsigned frees = test_frees, closes = test_closes;
        handle_device_poll(m, n, revents);
        assert(n->fd == -1 && !n->dev && !n->keyboard && !n->backlog);
        assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        assert(script.calls == (conditions[i] ? 0u : 1u));
        nfds_t count;
        int backlog;
        assert(build_pollset(m, &count, &backlog) == 0 && count == 2 && !backlog);
        handle_device_poll(m, n, POLLIN | POLLHUP);
        release_device(n);
        input_manager_destroy(m);
        assert(test_frees == frees + 1 && test_closes == closes + 1);
    }
    puts("PASS HUP/NVAL/ERR/ENODEV: removed, closed once, no bad-fd reads");
}

static void test_modifiers(void) {
    struct test_capture c = {0};
    struct input_manager *m = input_manager_create(NULL, NULL, NULL, test_key, &c);
    assert(m);
    struct input_node *a = test_node(m, 1, 0, 0), *b = test_node(m, 1, 0, 0);
    unsigned pairs[][2] = {{KEY_LEFTMETA, KEY_RIGHTMETA}, {KEY_LEFTSHIFT, KEY_RIGHTSHIFT},
                          {KEY_LEFTCTRL, KEY_RIGHTCTRL}, {KEY_LEFTALT, KEY_RIGHTALT}};
    for (unsigned i = 0; i < 4; i++) {
        test_send(m, a, EV_KEY, pairs[i][0], 1);
        test_send(m, a, EV_KEY, pairs[i][1], 1);
        test_send(m, b, EV_KEY, pairs[i][1], 1);
        test_send(m, a, EV_KEY, pairs[i][0], 0);
    }
    assert(c.mods.super && c.mods.shift && c.mods.ctrl && c.mods.alt);
    for (unsigned i = 0; i < 4; i++) test_send(m, a, EV_KEY, pairs[i][1], 0);
    test_send(m, a, EV_KEY, KEY_A, 1);
    assert(c.key.code == KEY_A && c.mods.super && c.mods.shift && c.mods.ctrl && c.mods.alt);
    test_send(m, a, EV_KEY, KEY_A, 2);
    assert(c.key.value == 2);
    release_device(b);
    test_send(m, a, EV_KEY, KEY_A, 0);
    assert(!c.mods.super && !c.mods.shift && !c.mods.ctrl && !c.mods.alt);
    input_manager_destroy(m);
    puts("PASS modifiers: two keyboards, both sides, nonmodifier/repeat/release, unplug reset");
}

static void test_abs_sync_and_budget(void) {
    struct test_capture c = {0};
    struct input_manager *m = input_manager_create(NULL, NULL, test_motion, test_key, &c);
    assert(m);
    struct input_node *a = test_node(m, 1, 0, 1);
    test_send(m, a, EV_ABS, ABS_X, 100);
    test_send(m, a, EV_SYN, SYN_REPORT, 0);
    assert(c.motions == 0);
    test_send(m, a, EV_ABS, ABS_X, 200);
    test_send(m, a, EV_ABS, ABS_Y, -300);
    test_send(m, a, EV_SYN, SYN_REPORT, 0);
    assert(c.motions == 1 && c.normalized && fabs(c.dx - .1) < 1e-9 && c.dy == 0);
    test_send(m, a, EV_ABS, ABS_Y, -100);
    test_send(m, a, EV_SYN, SYN_REPORT, 0);
    assert(c.motions == 2 && c.dx == 0 && fabs(c.dy - .1) < 1e-9);
    test_send(m, a, EV_KEY, BTN_TOUCH, 0);
    test_send(m, a, EV_ABS, ABS_X, 900);
    test_send(m, a, EV_SYN, SYN_REPORT, 0);
    assert(c.motions == 2 && !a->abs_has[1]);
    test_send(m, a, EV_ABS, ABS_X, 950); /* Pending motion must be discarded by sync. */
    const struct test_step steps[] = {
        {-EINTR, 0, 0, 0},
        {LIBEVDEV_READ_STATUS_SYNC, EV_SYN, SYN_DROPPED, 0},
        {LIBEVDEV_READ_STATUS_SYNC, EV_KEY, KEY_RIGHTSHIFT, 1},
        {LIBEVDEV_READ_STATUS_SYNC, EV_ABS, ABS_X, 1100},
        {-EAGAIN, 0, 0, 0},
        {LIBEVDEV_READ_STATUS_SUCCESS, EV_ABS, ABS_Y, 1000},
        {LIBEVDEV_READ_STATUS_SUCCESS, EV_ABS, ABS_X, 100},
        {LIBEVDEV_READ_STATUS_SUCCESS, EV_SYN, SYN_REPORT, 0},
        {LIBEVDEV_READ_STATUS_SUCCESS, EV_KEY, KEY_A, 1},
    };
    struct test_script s = {.steps = steps, .count = sizeof(steps) / sizeof(steps[0])};
    a->test_next = test_next;
    a->test_data = &s;
    unsigned keys = c.keys;
    drain_device(m, a);
    assert(!a->syncing && a->backlog && !a->abs_has[0] && !a->abs_has[1]);
    assert(c.motions == 2 && c.keys == keys && s.sync_calls == 3);
    handle_device_poll(m, a, 0); /* Buffered normal events, no new POLLIN. */
    assert(!a->backlog && c.motions == 2 && c.key.code == KEY_A && c.mods.shift);
    test_send(m, a, EV_ABS, ABS_Y, 1200);
    test_send(m, a, EV_SYN, SYN_REPORT, 0);
    assert(c.motions == 3 && c.dx == 0 && fabs(c.dy - .1) < 1e-9);
    struct input_absinfo zero_range = {.minimum = 0, .maximum = 0};
    libevdev_set_abs_info(a->dev, ABS_X, &zero_range);
    test_send(m, a, EV_ABS, ABS_X, 999);
    test_send(m, a, EV_SYN, SYN_REPORT, 0);
    assert(c.motions == 3);
    struct test_step long_sync[READ_BUDGET + 2];
    for (size_t i = 0; i < READ_BUDGET + 1; i++)
        long_sync[i] = (struct test_step){LIBEVDEV_READ_STATUS_SYNC, EV_KEY, KEY_RIGHTSHIFT, 1};
    long_sync[0] = (struct test_step){LIBEVDEV_READ_STATUS_SYNC, EV_SYN, SYN_DROPPED, 0};
    long_sync[READ_BUDGET].value = 0;
    long_sync[READ_BUDGET + 1] = (struct test_step){-EAGAIN, 0, 0, 0};
    s = (struct test_script){.steps = long_sync, .count = READ_BUDGET + 2};
    keys = c.keys;
    drain_device(m, a);
    assert(a->syncing && a->backlog && s.calls == READ_BUDGET);
    drain_device(m, a);
    assert(!a->syncing && a->backlog && !modifiers(m).shift && c.keys == keys);
    drain_device(m, a);
    assert(!a->backlog);
    struct input_node *r = test_node(m, 0, 1, 0);
    struct test_script endless = {.endless = 1};
    r->test_next = test_next;
    r->test_data = &endless;
    drain_device(m, r);
    assert(endless.calls == READ_BUDGET && r->backlog && !c.normalized);
    nfds_t count;
    int backlog;
    assert(build_pollset(m, &count, &backlog) == 0 && backlog && poll_timeout(m, now_ms(), backlog) == 0);
    input_manager_destroy(m);
    puts("PASS ABS/SYNC: independent axes, touch/recovery baselines, ranges, bounded buffered reads");
}

static void test_preferences_and_identity(void) {
    struct test_capture c = {0};
    struct input_manager *m = input_manager_create(NULL, NULL, test_motion, test_key, &c);
    assert(m);
    struct input_node *r = test_node(m, 0, 1, 0);
    struct input_node *a = test_node(m, 0, 0, 1);
    struct input_node *k = test_node(m, 1, 1, 1);
    libevdev_set_phys(r->dev, "same-phys");
    libevdev_set_phys(a->dev, "same-phys");
    libevdev_set_phys(k->dev, "same-phys");
    update_preferences(m);
    assert(r->suppressed && k->suppressed && k->keyboard && k->motion == MOTION_REL);
    assert(r->fd >= 0 && k->fd >= 0);
    test_send(m, k, EV_KEY, KEY_RIGHTMETA, 1);
    test_send(m, k, EV_KEY, KEY_A, 1);
    assert(c.keys == 2 && c.mods.super);
    test_send(m, k, EV_REL, REL_X, 1);
    assert(c.motions == 0); /* Motion preference must not discard keyboard events. */
    release_device(a);
    update_preferences(m);
    assert(!r->suppressed && !k->suppressed);
    test_send(m, k, EV_REL, REL_X, 1);
    assert(c.motions == 1 && !c.normalized);
    r->syspath = strdup("/sys/test/input1/event100");
    r->identity = (struct stat){.st_rdev = 123, .st_dev = 1, .st_ino = 10};
    struct stat st = r->identity;
    assert(identity_relation(r, r->path, r->syspath, &st) == 1);
    assert(identity_relation(r, "/dev/alias", r->syspath, &st) == 1);
    st.st_ino++;
    assert(identity_relation(r, r->path, r->syspath, &st) == 2);
    assert(identity_relation(r, r->path, "/sys/test/input2/event100", &st) == 2);
    st.st_rdev++;
    assert(identity_relation(r, "/dev/unrelated", "/sys/unrelated", &st) == 0);
    assert(is_event_name("event100") && !is_event_name("event") && !is_event_name("event1x"));
    input_manager_destroy(m);
    puts("PASS identity/phys: aliases, inode/syspath reuse, composite keyboard retained, ABS removal fallback");
}

static void test_empty_and_lifecycle(void) {
    char mouse[] = "copied-mouse", keyboard[] = "copied-keyboard";
    struct input_manager *m = input_manager_create(mouse, keyboard, NULL, NULL, NULL);
    assert(m);
    mouse[0] = keyboard[0] = 'X';
    assert(!strcmp(m->mouse_path, "copied-mouse") && !strcmp(m->keyboard_path, "copied-keyboard"));
    m->test_offline = 1; /* Explicitly prohibits enumeration or real device opens. */
    int ready = eventfd(0, EFD_CLOEXEC);
    assert(ready >= 0);
    m->test_ready_fd = ready;
    assert(input_manager_start(m) == 0);
    assert(input_manager_start(m) == -1 && errno == EALREADY);
    uint64_t value;
    assert(read(ready, &value, sizeof(value)) == sizeof(value));
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    assert(timer >= 0);
    struct itimerspec expiry = {.it_value = {.tv_nsec = 120000000}};
    uint64_t before = now_ms();
    assert(timerfd_settime(timer, 0, &expiry, NULL) == 0);
    assert(read(timer, &value, sizeof(value)) == sizeof(value));
    assert(now_ms() - before >= 100 && atomic_load(&m->test_polls) == 1);
    before = now_ms();
    input_manager_destroy(m);
    assert(now_ms() - before < 1000); /* eventfd wakes the indefinitely blocked poll. */
    close(timer);
    close(ready);
    m = input_manager_create(NULL, NULL, NULL, NULL, NULL);
    assert(m);
    nfds_t count;
    int backlog;
    m->scan_at = now_ms() + SCAN_MS;
    m->monitor_at = m->scan_at;
    assert(build_pollset(m, &count, &backlog) == 0 && count == 2 && !backlog);
    assert(poll_timeout(m, now_ms(), backlog) > 1000); /* Production empty set also blocks. */
    input_manager_destroy(m);
    input_manager_destroy(NULL);
    puts("PASS empty poll/lifecycle: no spin, copied paths, eventfd wake + join, destroy before start");
}

int main(void) {
    test_fatal_fds();
    test_modifiers();
    test_abs_sync_and_budget();
    test_preferences_and_identity();
    test_empty_and_lifecycle();
    puts("All INPUT_TEST tests passed (no real input devices accessed).");
    return 0;
}
#endif
