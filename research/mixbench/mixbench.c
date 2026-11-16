#include "mixbench.h"

#include <immintrin.h>
#include <string.h>

/* Naive baseline: per-sample pan branch, per-sample resampler carry
 * branch, per-sample end-of-voice branch, scalar FPU. The division is
 * hoisted by the compiler even at -O2, so the real cost is the serial
 * float phase dependency chain and data-dependent branches. */
void mb_mix_naive(const mb_engine* engine, int frames, float* outL, float* outR)
{
    for (int i = 0; i < frames; i++) {
        outL[i] = 0.0f;
        outR[i] = 0.0f;
    }

    for (int v = 0; v < engine->count; v++) {
        mb_voice vc = engine->voices[v];
        const float step = vc.srcRate / (float)MB_OUT_RATE;
        const int limit = vc.srcFrames - 2;

        for (int i = 0; i < frames; i++) {
            float gl, gr;
            if (vc.pan < 0.0f) {
                gl = 1.0f;
                gr = 1.0f + vc.pan;
            } else {
                gl = 1.0f - vc.pan;
                gr = 1.0f;
            }

            float sample = 0.0f;
            if (vc.pos < limit) {
                const float a = vc.src[vc.pos];
                const float b = vc.src[vc.pos + 1];
                sample = a + (b - a) * vc.frac;

                vc.frac += step;
                if (vc.frac >= 1.0f) {
                    vc.frac -= 1.0f;
                    vc.pos += 1;
                }
            }

            outL[i] = outL[i] + sample * vc.gain * gl;
            outR[i] = outR[i] + sample * vc.gain * gr;
        }

        engine->voices[v].pos = vc.pos;
        engine->voices[v].frac = vc.frac;
    }
}

/* Optimized scalar: branchless selects (cmov/setcc) replace the pan,
 * carry and end-of-voice branches; pan resolved once per block; one
 * division per block. Float recurrence, blend and gain association match
 * the baseline exactly, so output is bit-identical. */
void mb_mix_opt_scalar(const mb_engine* engine, int frames, float* outL, float* outR)
{
    for (int i = 0; i < frames; i++) {
        outL[i] = 0.0f;
        outR[i] = 0.0f;
    }

    for (int v = 0; v < engine->count; v++) {
        mb_voice* vcp = &engine->voices[v];
        mb_voice vc = *vcp;
        const float step = vc.srcRate / (float)MB_OUT_RATE;
        const int limit = vc.srcFrames - 2;

        const int negPan = vc.pan < 0.0f;
        const float gl = negPan ? 1.0f : 1.0f - vc.pan;
        const float gr = negPan ? 1.0f + vc.pan : 1.0f;

        int pos = vc.pos;
        float frac = vc.frac;

        for (int i = 0; i < frames; i++) {
            const float a = vc.src[pos];
            const float b = vc.src[pos + 1];
            float sample = a + (b - a) * frac;

            const int live = -(pos < limit);
            sample = live ? sample : 0.0f;

            const float gained = sample * vc.gain;
            outL[i] = outL[i] + gained * gl;
            outR[i] = outR[i] + gained * gr;

            frac += step;
            const int carry = frac >= 1.0f;
            frac = carry ? frac - 1.0f : frac;
            pos += carry;
            if (pos > limit) {
                pos = limit;
            }
        }

        vcp->pos = pos;
        vcp->frac = frac;
    }
}

/* Optimized AVX-512: same branchless scalar phase walk into a 64-byte
 * aligned staging buffer, then 16-wide AVX-512 gain/pan/stereo
 * accumulate. Identical per-lane float operations as the baseline.
 * MB_BLOCK_FRAMES (128) is a multiple of 16, so there is no tail. */
void mb_mix_opt_avx512(const mb_engine* engine, int frames, float* outL, float* outR)
{
    float tmp[MB_BLOCK_FRAMES] __attribute__((aligned(64)));

    for (int i = 0; i < frames; i++) {
        outL[i] = 0.0f;
        outR[i] = 0.0f;
    }

    for (int v = 0; v < engine->count; v++) {
        mb_voice* vcp = &engine->voices[v];
        mb_voice vc = *vcp;
        const float step = vc.srcRate / (float)MB_OUT_RATE;
        const int limit = vc.srcFrames - 2;

        const int negPan = vc.pan < 0.0f;
        const float gl = negPan ? 1.0f : 1.0f - vc.pan;
        const float gr = negPan ? 1.0f + vc.pan : 1.0f;

        int pos = vc.pos;
        float frac = vc.frac;

        for (int i = 0; i < frames; i++) {
            const float a = vc.src[pos];
            const float b = vc.src[pos + 1];
            float sample = a + (b - a) * frac;

            const int live = -(pos < limit);
            tmp[i] = live ? sample : 0.0f;

            frac += step;
            const int carry = frac >= 1.0f;
            frac = carry ? frac - 1.0f : frac;
            pos += carry;
            if (pos > limit) {
                pos = limit;
            }
        }

        const __m512 vgain = _mm512_set1_ps(vc.gain);
        const __m512 vgl = _mm512_set1_ps(gl);
        const __m512 vgr = _mm512_set1_ps(gr);
        for (int i = 0; i < frames; i += 16) {
            const __m512 s = _mm512_load_ps(tmp + i);
            const __m512 gained = _mm512_mul_ps(s, vgain);
            _mm512_store_ps(outL + i,
                _mm512_add_ps(_mm512_load_ps(outL + i),
                              _mm512_mul_ps(gained, vgl)));
            _mm512_store_ps(outR + i,
                _mm512_add_ps(_mm512_load_ps(outR + i),
                              _mm512_mul_ps(gained, vgr)));
        }

        vcp->pos = pos;
        vcp->frac = frac;
    }
}

/* Optimized AVX2: branchless scalar phase walk into an aligned staging
 * buffer (the position dependency is inherently serial), then an 8-wide
 * AVX2 pass applies gain, pan and stereo accumulation. One division per
 * block, no data-dependent branches in the steady state, one pass over the
 * output. Per-lane float operations match the baseline exactly. */
void mb_mix_opt_sse2(const mb_engine* engine, int frames, float* outL, float* outR)
{
    float tmp[MB_BLOCK_FRAMES] __attribute__((aligned(32)));

    for (int i = 0; i < frames; i++) {
        outL[i] = 0.0f;
        outR[i] = 0.0f;
    }

    for (int v = 0; v < engine->count; v++) {
        mb_voice* vcp = &engine->voices[v];
        mb_voice vc = *vcp;
        const float step = vc.srcRate / (float)MB_OUT_RATE;
        const int limit = vc.srcFrames - 2;

        const int negPan = vc.pan < 0.0f;
        const float gl = negPan ? 1.0f : 1.0f - vc.pan;
        const float gr = negPan ? 1.0f + vc.pan : 1.0f;

        int pos = vc.pos;
        float frac = vc.frac;

        for (int i = 0; i < frames; i++) {
            const float a = vc.src[pos];
            const float b = vc.src[pos + 1];
            float sample = a + (b - a) * frac;

            const int live = -(pos < limit);
            tmp[i] = live ? sample : 0.0f;

            frac += step;
            const int carry = frac >= 1.0f;
            frac = carry ? frac - 1.0f : frac;
            pos += carry;
            if (pos > limit) {
                pos = limit;
            }
        }

        const __m256 vgain = _mm256_set1_ps(vc.gain);
        const __m256 vgl = _mm256_set1_ps(gl);
        const __m256 vgr = _mm256_set1_ps(gr);
        for (int i = 0; i < frames; i += 8) {
            const __m256 s = _mm256_load_ps(tmp + i);
            const __m256 gained = _mm256_mul_ps(s, vgain);
            _mm256_store_ps(outL + i,
                _mm256_add_ps(_mm256_load_ps(outL + i),
                              _mm256_mul_ps(gained, vgl)));
            _mm256_store_ps(outR + i,
                _mm256_add_ps(_mm256_load_ps(outR + i),
                              _mm256_mul_ps(gained, vgr)));
        }

        vcp->pos = pos;
        vcp->frac = frac;
    }
}
