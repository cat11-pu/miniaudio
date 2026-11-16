#ifndef MIXBENCH_H
#define MIXBENCH_H

#include <stdint.h>

#define MB_BLOCK_FRAMES   128
#define MB_OUT_RATE       48000

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const float* src;
    int          srcFrames;
    int          pos;
    float        frac;
    float        srcRate;
    float        gain;
    float        pan;
} mb_voice;

typedef struct {
    mb_voice* voices;
    int       count;
} mb_engine;

void mb_mix_naive(const mb_engine* engine, int frames, float* outL, float* outR);
void mb_mix_opt_scalar(const mb_engine* engine, int frames, float* outL, float* outR);
void mb_mix_opt_sse2(const mb_engine* engine, int frames, float* outL, float* outR);
void mb_mix_opt_avx512(const mb_engine* engine, int frames, float* outL, float* outR);

#ifdef __cplusplus
}
#endif

#endif
