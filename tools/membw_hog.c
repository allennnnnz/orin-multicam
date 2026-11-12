/*
 * membw_hog.c - background DRAM-bandwidth load to test latency under
 * contention. On Tegra the CPU, GPU, DLA, VIC and codecs share one LPDDR5
 * memory controller (EMC), so a CPU memcpy storm slows every engine.
 *
 *   usage: membw_hog <threads> <seconds> [MB per buffer]
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double g_end;
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *hog(void *arg)
{
    size_t sz = *(size_t *)arg;
    char *a = malloc(sz), *b = malloc(sz);
    double bytes = 0, t0 = now_s();
    memset(a, 1, sz);
    memset(b, 2, sz);
    while (now_s() < g_end) {
        memcpy(b, a, sz);
        memcpy(a, b, sz);
        bytes += 4.0 * sz;          /* 2 copies x (read + write) */
    }
    double *gbps = malloc(sizeof(double));
    *gbps = bytes / (now_s() - t0) / 1e9;
    free(a);
    free(b);
    return gbps;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <threads> <seconds> [MB]\n", argv[0]); return 2; }
    int n = atoi(argv[1]);
    size_t sz = (size_t)(argc > 3 ? atoi(argv[3]) : 64) << 20;
    g_end = now_s() + atof(argv[2]);
    pthread_t th[64];
    if (n < 1 || n > 64) return 2;
    for (int i = 0; i < n; i++) pthread_create(&th[i], NULL, hog, &sz);
    double total = 0;
    for (int i = 0; i < n; i++) {
        double *g;
        pthread_join(th[i], (void **)&g);
        total += *g;
        free(g);
    }
    printf("membw_hog: %d threads, %.1f GB/s aggregate\n", n, total);
    return 0;
}
