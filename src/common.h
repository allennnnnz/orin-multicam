/*
 * common.h - small shared helpers (timing, logging, error checks).
 */
#ifndef OMC_COMMON_H
#define OMC_COMMON_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_STREAMS 4

static inline uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

#define LOG(fmt, ...)  fprintf(stderr, "[omc] " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...)  fprintf(stderr, "[omc] ERROR %s:%d: " fmt "\n", \
                               __FILE__, __LINE__, ##__VA_ARGS__)
#define ERRNO(fmt, ...) fprintf(stderr, "[omc] ERROR %s:%d: " fmt ": %s\n", \
                               __FILE__, __LINE__, ##__VA_ARGS__, strerror(errno))

/* Jump to a cleanup label on failure; keeps error paths in C readable. */
#define TRY(cond, label, fmt, ...) do { \
        if (!(cond)) { ERR(fmt, ##__VA_ARGS__); goto label; } } while (0)

#endif
