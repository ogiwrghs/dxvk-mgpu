#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

int main(int argc, char** argv) {
    int thresholdUs = (argc > 1) ? atoi(argv[1]) : 1000;
    int runSeconds  = (argc > 2) ? atoi(argv[2]) : 15;

    LARGE_INTEGER freq, start, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    prev = start;

    long long limit = (long long)runSeconds * freq.QuadPart;
    long long gaps = 0, worstUs = 0, worstAtMs = 0, iters = 0;
    long long buckets[5] = {0,0,0,0,0};

    printf("spin_mon: busy-spin, gaps over %d us, %d s\n", thresholdUs, runSeconds);
    fflush(stdout);

    for (;;) {
        QueryPerformanceCounter(&now);
        iters++;

        long long deltaUs = ((now.QuadPart - prev.QuadPart) * 1000000LL) / freq.QuadPart;

        if (deltaUs > thresholdUs) {
            long long atMs = ((now.QuadPart - start.QuadPart) * 1000LL) / freq.QuadPart;
            gaps++;

            if (deltaUs > worstUs) { worstUs = deltaUs; worstAtMs = atMs; }

            if      (deltaUs <  2000) buckets[0]++;
            else if (deltaUs <  5000) buckets[1]++;
            else if (deltaUs < 20000) buckets[2]++;
            else if (deltaUs < 50000) buckets[3]++;
            else                      buckets[4]++;

            if (deltaUs > 20000) {
                printf("[%8lld ms] GAP %lld us\n", atMs, deltaUs);
                fflush(stdout);
            }
        }

        prev = now;

        if (now.QuadPart - start.QuadPart > limit)
            break;
    }

    printf("\n--- %lld iterations, %lld gaps over %d us ---\n", iters, gaps, thresholdUs);
    printf("  1-2ms: %lld\n  2-5ms: %lld\n  5-20ms: %lld\n  20-50ms: %lld\n  50ms+: %lld\n",
           buckets[0], buckets[1], buckets[2], buckets[3], buckets[4]);
    printf("  worst: %lld us at %lld ms\n", worstUs, worstAtMs);
    return 0;
}
