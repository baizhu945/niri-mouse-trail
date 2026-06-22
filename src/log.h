#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <time.h>
#include <sys/time.h>

extern FILE *g_log_file;
extern int g_log_level;

static inline void log_init(FILE *f, int level) {
    g_log_file = f;
    g_log_level = level;
}

#define LOG_FMT(level_str, fmt, ...) do { \
    if (g_log_file) { \
        struct timeval _tv; \
        gettimeofday(&_tv, NULL); \
        struct tm _tm; \
        localtime_r(&_tv.tv_sec, &_tm); \
        fprintf(g_log_file, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%-5s] %s:%d " fmt "\n", \
            _tm.tm_year + 1900, _tm.tm_mon + 1, _tm.tm_mday, \
            _tm.tm_hour, _tm.tm_min, _tm.tm_sec, (int)(_tv.tv_usec / 1000), \
            level_str, __FILE__, __LINE__, ##__VA_ARGS__); \
        fflush(g_log_file); \
    } \
} while(0)

#define LOG_DEBUG(fmt, ...) do { if (g_log_level <= 0) LOG_FMT("DEBUG", fmt, ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)  do { if (g_log_level <= 1) LOG_FMT("INFO",  fmt, ##__VA_ARGS__); } while(0)
#define LOG_WARN(fmt, ...)  do { if (g_log_level <= 2) LOG_FMT("WARN",  fmt, ##__VA_ARGS__); } while(0)
#define LOG_ERROR(fmt, ...) do { if (g_log_level <= 3) LOG_FMT("ERROR", fmt, ##__VA_ARGS__); } while(0)

#endif
