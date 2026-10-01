/* Read-only input-manager smoke test: no Wayland connection, no injected input.
 * The callbacks only count events delivered to this process's own evdev clients.
 * Usage: build/input_probe [seconds] (default: 3).
 */
#define _POSIX_C_SOURCE 200809L
#include "input.h"
#include "log.h"
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>

FILE *g_log_file = NULL;
int g_log_level = 1;
static _Atomic unsigned long motions, keys;

static void motion(void *user, double dx, double dy, int normalized) {
    (void)user; (void)dx; (void)dy; (void)normalized;
    atomic_fetch_add(&motions, 1);
}
static void key(void *user, const struct input_event *event,
                const input_modifiers_t *mods) {
    (void)user; (void)event; (void)mods;
    atomic_fetch_add(&keys, 1);
}
static int fd_count(void) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (entry->d_name[0] != '.') count++;
    closedir(dir);
    return count;
}
static double cpu_seconds(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.0;
}
int main(int argc, char **argv) {
    unsigned long seconds = 3;
    if (argc > 1) {
        char *end;
        errno = 0;
        seconds = strtoul(argv[1], &end, 10);
        if (errno || *end || seconds < 1 || seconds > 600) return 2;
    }
    log_init(stderr, 1);
    int before = fd_count();
    struct input_manager *manager = input_manager_create(NULL, NULL, motion, key, NULL);
    if (!manager || input_manager_start(manager) < 0) {
        input_manager_destroy(manager);
        return 1;
    }
    double cpu_start = cpu_seconds();
    struct timespec wait = { .tv_sec = (time_t)seconds };
    while (nanosleep(&wait, &wait) < 0 && errno == EINTR) {}
    input_manager_destroy(manager);
    double used = cpu_seconds() - cpu_start;
    int after = fd_count();
    printf("Input probe: %lu s, CPU %.4f s (%.2f%% of one core), callbacks motion=%lu key=%lu, FD before=%d after=%d\n",
           seconds, used, used * 100.0 / seconds, atomic_load(&motions),
           atomic_load(&keys), before, after);
    if (before < 0 || after != before) {
        fprintf(stderr, "Input manager leaked file descriptors\n");
        return 1;
    }
    return 0;
}
