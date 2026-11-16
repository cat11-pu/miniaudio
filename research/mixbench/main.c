#define _POSIX_C_SOURCE 199309L

#include "mixbench.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_VOICES 64
#define BLOCKS     4096
#define REPEATS    7
#define SRC_SECS   12

typedef void (*mix_fn)(const mb_engine*, int, float*, float*);

typedef struct {
    float rate;
    int   frames;
    float* data;
} mb_source;

static uint32_t rng_state = 0x12345678u;

static uint32_t xrng(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static float frand(void)
{
    return (float)((int32_t)(xrng() >> 1) % 20001 - 10000) / 10000.0f;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u32(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

static uint32_t ulp_diff(float a, float b)
{
    uint32_t ua, ub;
    memcpy(&ua, &a, 4);
    memcpy(&ub, &b, 4);
    if ((ua >> 31) != (ub >> 31)) {
        return ua + ub + 1u;
    }
    return ua > ub ? ua - ub : ub - ua;
}

/* Returns number of mismatching float values. */
static long compare_buffers(const float* a, const float* b, long count,
                            uint32_t* maxUlp)
{
    long mismatches = 0;
    *maxUlp = 0;
    for (long i = 0; i < count; i++) {
        if (memcmp(&a[i], &b[i], 4) != 0) {
            mismatches++;
            uint32_t d = ulp_diff(a[i], b[i]);
            if (d > *maxUlp) {
                *maxUlp = d;
            }
        }
    }
    return mismatches;
}

static mb_source make_source(float rate, float tone)
{
    mb_source s;
    s.rate = rate;
    s.frames = (int)(rate * SRC_SECS);
    s.data = malloc(((size_t)s.frames + 2) * sizeof(float));
    for (int i = 0; i < s.frames; i++) {
        double t = (double)i / (double)rate;
        s.data[i] = (float)(0.6 * sin(2.0 * M_PI * tone * t)
                          + 0.3 * sin(2.0 * M_PI * tone * 2.7 * t)
                          + 0.1 * frand());
    }
    s.data[s.frames] = 0.0f;
    s.data[s.frames + 1] = 0.0f;
    return s;
}

static const float rates[4] = { 22050.0f, 32000.0f, 44100.0f, 48000.0f };

static void setup_voices(mb_voice* base, int count, const mb_source* srcs,
                         int shortSrc, int cursorOffset)
{
    for (int v = 0; v < count; v++) {
        const mb_source* s = &srcs[v % 4];
        base[v].src = s->data;
        base[v].srcFrames = shortSrc ? 180 + (v % 7) * 40 : s->frames;
        base[v].pos = (v * 37) % (base[v].srcFrames / 2);
        base[v].frac = 0.0f;
        base[v].srcRate = s->rate;
        base[v].gain = 0.1f + 0.9f * (float)((v * 131) % 97) / 97.0f;
        base[v].pan = (float)((v * 53) % 201 - 100) / 100.0f;
        (void)cursorOffset;
    }
}

static int run_correctness(const mb_source* srcs, int count, int blocks,
                           int shortSrc, const char* label)
{
    mb_voice* base = calloc(MAX_VOICES, sizeof(mb_voice));
    mb_voice (*work)[MAX_VOICES] = calloc(4, sizeof(mb_voice) * MAX_VOICES);
    setup_voices(base, count, srcs, shortSrc, 0);

    float out[4][2][MB_BLOCK_FRAMES] __attribute__((aligned(64)));
    const mix_fn fns[4] = { mb_mix_naive, mb_mix_opt_scalar, mb_mix_opt_sse2, mb_mix_opt_avx512 };
    long total = 0, bad12 = 0, bad13 = 0;
    long bad14 = 0;
    uint32_t maxUlp12 = 0, maxUlp13 = 0, maxUlp14 = 0;

    for (int b = 0; b < blocks; b++) {
        for (int k = 0; k < 4; k++) {
            memcpy(work[k], base, sizeof(mb_voice) * count);
            mb_engine e = { work[k], count };
            fns[k](&e, MB_BLOCK_FRAMES, out[k][0], out[k][1]);
        }
        for (int ch = 0; ch < 2; ch++) {
            uint32_t u;
            bad12 += compare_buffers(out[0][ch], out[1][ch], MB_BLOCK_FRAMES, &u);
            if (u > maxUlp12) maxUlp12 = u;
            bad13 += compare_buffers(out[0][ch], out[2][ch], MB_BLOCK_FRAMES, &u);
            if (u > maxUlp13) maxUlp13 = u;
            bad14 += compare_buffers(out[0][ch], out[3][ch], MB_BLOCK_FRAMES, &u);
            if (u > maxUlp14) maxUlp14 = u;
            total += MB_BLOCK_FRAMES;
        }
        /* Carry state forward from the naive run. */
        mb_engine e0 = { work[0], count };
        (void)e0;
        memcpy(base, work[0], sizeof(mb_voice) * count);
    }

    int ok = (bad12 == 0 && bad13 == 0 && bad14 == 0);
    printf("[correctness] %-28s voices=%2d blocks=%4d samples=%ld  "
           "scalar=%ld(%u) sse2=%ld(%u) avx512=%ld(%u) %s\n",
           label, count, blocks, total, bad12, maxUlp12, bad13, maxUlp13,
           bad14, maxUlp14,
           ok ? "BIT-IDENTICAL" : "FAIL");
    free(base);
    free(work);
    return ok;
}

static void print_sample_table(const mb_source* srcs, int count)
{
    mb_voice* base = calloc(MAX_VOICES, sizeof(mb_voice));
    mb_voice (*work)[MAX_VOICES] = calloc(2, sizeof(mb_voice) * MAX_VOICES);
    setup_voices(base, count, srcs, 0, 0);
    float outL[2][MB_BLOCK_FRAMES] __attribute__((aligned(64))), outR[2][MB_BLOCK_FRAMES] __attribute__((aligned(64)));

    for (int k = 0; k < 2; k++) {
        memcpy(work[k], base, sizeof(mb_voice) * count);
        mb_engine e = { work[k], count };
        if (k == 0) mb_mix_naive(&e, MB_BLOCK_FRAMES, outL[0], outR[0]);
        else        mb_mix_opt_sse2(&e, MB_BLOCK_FRAMES, outL[1], outR[1]);
    }

    printf("\n[sample comparison] %d voices, first block, first 8 frames\n", count);
    printf("%-5s %-17s %-17s %-6s  %-17s %-17s %-6s\n",
           "frame", "naive L", "sse2 L", "match", "naive R", "sse2 R", "match");
    for (int i = 0; i < 8; i++) {
        uint32_t u;
        int mL = compare_buffers(&outL[0][i], &outL[1][i], 1, &u) == 0;
        int mR = compare_buffers(&outR[0][i], &outR[1][i], 1, &u) == 0;
        printf("%-5d % .9g % .9g %-6s  % .9g % .9g %-6s\n",
               i, outL[0][i], outL[1][i], mL ? "yes" : "NO",
               outR[0][i], outR[1][i], mR ? "yes" : "NO");
    }
    free(base);
    free(work);
}

static double bench_one(mix_fn fn, const mb_voice* baseIn, int count,
                        uint64_t* perBlockOut)
{
    mb_voice* work = calloc(MAX_VOICES, sizeof(mb_voice));
    float outL[MB_BLOCK_FRAMES] __attribute__((aligned(64))), outR[MB_BLOCK_FRAMES] __attribute__((aligned(64)));
    uint64_t best = UINT64_MAX;

    for (int r = 0; r < REPEATS; r++) {
        memcpy(work, baseIn, sizeof(mb_voice) * count);
        mb_engine e = { work, count };
        uint64_t t0 = now_ns();
        for (int b = 0; b < BLOCKS; b++) {
            fn(&e, MB_BLOCK_FRAMES, outL, outR);
        }
        uint64_t dt = now_ns() - t0;
        perBlockOut[r] = dt / BLOCKS;
        if (dt < best) best = dt;
    }
    free(work);
    return (double)best / BLOCKS;
}

int main(void)
{
    mb_source srcs[4];
    for (int i = 0; i < 4; i++) {
        srcs[i] = make_source(rates[i], 110.0f * (1.0f + 0.37f * i));
    }
    printf("mixbench: 48kHz output, %d frames/block, %d blocks/run (%d samples)\n\n",
           MB_BLOCK_FRAMES, BLOCKS, BLOCKS * MB_BLOCK_FRAMES);

    int allOk = 1;
    const int poly[3] = { 16, 32, 64 };
    for (int i = 0; i < 3; i++) {
        allOk &= run_correctness(srcs, poly[i], 64, 0, "full-rate workload");
    }
    allOk &= run_correctness(srcs, 4, 32, 1, "voices ending mid-block");
    allOk &= run_correctness(srcs, 8, 8, 0, "pan sign coverage");

    print_sample_table(srcs, 64);

    printf("\n[timing] ns per 128-frame block, %d repeats (min/median), "
           "single thread\n", REPEATS);
    printf("%-8s %11s %11s %11s %11s %9s %9s %9s\n",
           "voices", "naive", "opt_scalar", "opt_avx2", "opt_avx512",
           "scalar Δ", "avx2 Δ", "avx512 Δ");

    for (int p = 0; p < 3; p++) {
        int count = poly[p];
        mb_voice* base = calloc(MAX_VOICES, sizeof(mb_voice));
        setup_voices(base, count, srcs, 0, 0);

        uint64_t stats[4][REPEATS];
        double t[4];
        t[0] = bench_one(mb_mix_naive, base, count, stats[0]);
        t[1] = bench_one(mb_mix_opt_scalar, base, count, stats[1]);
        t[2] = bench_one(mb_mix_opt_sse2, base, count, stats[2]);
        t[3] = bench_one(mb_mix_opt_avx512, base, count, stats[3]);
        for (int k = 0; k < 4; k++) {
            qsort(stats[k], REPEATS, sizeof(uint64_t), cmp_u32);
        }
        printf("%-8d %5.0f/%-5.0f %5.0f/%-5.0f %5.0f/%-5.0f %5.0f/%-5.0f %8.1f%% %8.1f%% %8.1f%%\n",
               count,
               t[0], (double)stats[0][REPEATS / 2],
               t[1], (double)stats[1][REPEATS / 2],
               t[2], (double)stats[2][REPEATS / 2],
               t[3], (double)stats[3][REPEATS / 2],
               100.0 * (1.0 - t[1] / t[0]),
               100.0 * (1.0 - t[2] / t[0]),
               100.0 * (1.0 - t[3] / t[0]));
        free(base);
    }

    printf("\nresult: %s\n", allOk ? "ALL OUTPUTS BIT-IDENTICAL" : "MISMATCH FOUND");
    for (int i = 0; i < 4; i++) free(srcs[i].data);
    return allOk ? 0 : 1;
}
