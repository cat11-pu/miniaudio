#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#include "../extras/miniaudio_split/miniaudio.h"
#include <stdio.h>
int main(int argc, char** argv) {
    ma_decoder d; ma_result r;
    ma_uint64 total = 0, h = 1469598103934665603ULL;
    ma_uint8 buf[65536];
    r = ma_decoder_init_file(argv[1], NULL, &d);
    if (r != MA_SUCCESS) { printf("init_error=%d\n", r); return 2; }
    for (;;) {
        ma_uint64 n = 0, i;
        r = ma_decoder_read_pcm_frames(&d, buf, 4096, &n);
        ma_uint64 bpf = (ma_uint64)d.outputChannels * ma_get_bytes_per_sample(d.outputFormat);
        for (i = 0; i < n * bpf; ++i) { h ^= buf[i]; h *= 1099511628211ULL; }
        total += n;
        if (r != MA_SUCCESS) break;
    }
    printf("frames=%llu hash=%016llx\n", (unsigned long long)total, (unsigned long long)h);
    ma_decoder_uninit(&d);
    return 0;
}
