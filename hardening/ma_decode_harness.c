/*
Decode harness for malformed-input hardening.

Decodes a file completely with ma_decoder (WAV/MP3/FLAC auto-detected),
reads every frame, and prints a stable FNV-1a hash of the decoded PCM so
bit-exactness of valid files can be verified before/after patches.

Exit codes:
    0  decoded OK
    2  decoder returned an error (expected for malformed files)
    anything else = crash/sanitizer abort (what we are hunting)
*/
#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MINIAUDIO_IMPLEMENTATION
#include "../miniaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ma_uint64 fnv1a_update(ma_uint64 h, const void* data, size_t size)
{
    const ma_uint8* p = (const ma_uint8*)data;
    size_t i;
    for (i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int main(int argc, char** argv)
{
    ma_decoder decoder;
    ma_decoder_config config;
    ma_result result;
    ma_uint64 hash = 1469598103934665603ULL;
    ma_uint64 totalFrames = 0;
    ma_uint8* frames = NULL;
    size_t framesCap = 0;
    void* pFileData = NULL;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <file> [mem]\n", argv[0]);
        return 1;
    }

    config = ma_decoder_config_init_default();

    if (argc > 2 && strcmp(argv[2], "mem") == 0) {
        /* Decode from memory: exercises the memory-backed read/seek paths. */
        FILE* f = fopen(argv[1], "rb");
        long size;
        if (f == NULL) return 1;
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        pFileData = malloc((size_t)size + 1);
        if (pFileData == NULL || fread(pFileData, 1, (size_t)size, f) != (size_t)size) {
            fclose(f);
            free(pFileData);
            return 1;
        }
        fclose(f);
        result = ma_decoder_init_memory(pFileData, (size_t)size, &config, &decoder);
    } else {
        result = ma_decoder_init_file(argv[1], &config, &decoder);
    }
    if (result != MA_SUCCESS) {
        printf("init_error=%d\n", (int)result);
        free(pFileData);
        return 2;
    }

    /* Size the buffer from the decoder's actual output format. */
    {
        ma_uint64 bpf = (ma_uint64)decoder.outputChannels * ma_get_bytes_per_sample(decoder.outputFormat);
        if (bpf == 0 || bpf > 1u << 20) {
            printf("init_error=bad_output_format channels=%u format=%d\n",
                decoder.outputChannels, (int)decoder.outputFormat);
            ma_decoder_uninit(&decoder);
            free(pFileData);
            return 2;
        }
        framesCap = (size_t)bpf * 4096;
        frames = (ma_uint8*)malloc(framesCap);
        if (frames == NULL) {
            ma_decoder_uninit(&decoder);
            free(pFileData);
            return 1;
        }
    }

    if (argc > 2 && strcmp(argv[2], "seek") == 0) {
        /* Exercise seek paths: bounce around the stream, reading after each seek. */
        ma_uint64 targets[] = {0, 1, 100, 4096, 100000, 0xFFFFFFFFFFFFFFFFULL, 44100, 0};
        int i;
        for (i = 0; i < (int)(sizeof(targets)/sizeof(targets[0])); ++i) {
            ma_uint64 framesRead = 0;
            ma_decoder_seek_to_pcm_frame(&decoder, targets[i]);
            result = ma_decoder_read_pcm_frames(&decoder, frames, 4096, &framesRead);
            if (framesRead > 0) {
                hash = fnv1a_update(hash, frames,
                    (size_t)framesRead * decoder.outputChannels * ma_get_bytes_per_sample(decoder.outputFormat));
                totalFrames += framesRead;
            }
        }
        printf("seek_result frames=%llu hash=%016llx\n",
            (unsigned long long)totalFrames, (unsigned long long)hash);
        ma_decoder_uninit(&decoder);
        free(frames);
        free(pFileData);
        return 0;
    }

    for (;;) {
        ma_uint64 framesRead = 0;
        result = ma_decoder_read_pcm_frames(&decoder, frames, 4096, &framesRead);
        if (framesRead > 0) {
            hash = fnv1a_update(hash, frames,
                (size_t)framesRead * decoder.outputChannels * ma_get_bytes_per_sample(decoder.outputFormat));
            totalFrames += framesRead;
        }
        if (result != MA_SUCCESS) {
            break; /* MA_AT_END or an error; both terminate the loop. */
        }
    }

    printf("result=%d frames=%llu hash=%016llx\n",
        (result == MA_AT_END) ? 0 : (int)result,
        (unsigned long long)totalFrames,
        (unsigned long long)hash);

    ma_decoder_uninit(&decoder);
    free(frames);
    free(pFileData);
    return (result == MA_AT_END) ? 0 : 2;
}
