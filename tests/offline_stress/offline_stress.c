#define MA_NO_DEVICE_IO
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_THREADING
#define MA_NO_ENCODING
#define MA_NO_DECODING
#include "../../miniaudio.c"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CYCLE_COUNT 200
#define RENDER_FRAME_COUNT  384
#define PERIOD_FRAME_COUNT  128
#define TRACE_ENTRY_COUNT   64
#define TRACE_ENTRY_SIZE    192

typedef union test_alloc_header test_alloc_header;
union test_alloc_header
{
    struct
    {
        size_t size;
        ma_uint32 magic;
        test_alloc_header* pNext;
    } h;
    long double alignment;
};

#define TEST_ALLOC_MAGIC 0x4D414C4C

typedef struct
{
    size_t liveCount;
    size_t peakCount;
    size_t totalCount;
    size_t badFreeCount;
    test_alloc_header* pHead;
} test_allocator;

static test_allocator g_Allocator;
static char g_Trace[TRACE_ENTRY_COUNT][TRACE_ENTRY_SIZE];
static unsigned g_TraceCount;
static unsigned g_FailureCount;
static unsigned g_LeakCount;

typedef enum
{
    test_fault_format_conversion,
    test_fault_buffer_underrun,
    test_fault_oversized_frame,
    test_fault_channel_count
} test_fault;

typedef struct
{
    ma_data_source_base ds;
    ma_format format;
    ma_uint32 channels;
    ma_uint32 sampleRate;
    ma_uint64 cursor;
    ma_uint32 readCallCount;
    ma_uint32 formatCallCount;
    ma_bool32 faultActive;
    ma_bool32 faultSeen;
    test_fault fault;
    ma_uint32 faultVariant;
} test_source;

static const char* g_FaultNames[] =
{
    "format_conversion",
    "buffer_underrun",
    "oversized_frame",
    "channel_count"
};

static unsigned g_FaultCounts[4];
static unsigned g_VariantCounts[4][2];
static unsigned g_FaultObservedCount;
static unsigned g_RecoveryCount;

static void trace_reset(void)
{
    g_TraceCount = 0;
}

static void trace_add(const char* pFormat, ...)
{
    va_list args;

    if (g_TraceCount >= TRACE_ENTRY_COUNT) {
        return;
    }

    va_start(args, pFormat);
    vsprintf(g_Trace[g_TraceCount], pFormat, args);
    va_end(args);

    g_TraceCount += 1;
}

static void trace_print(void)
{
    unsigned i;

    printf("minimal operation sequence:\n");
    for (i = 0; i < g_TraceCount; i += 1) {
        printf("  %u. %s\n", i + 1, g_Trace[i]);
    }
}

static const char* result_name(ma_result result)
{
    switch (result)
    {
        case MA_SUCCESS:           return "MA_SUCCESS";
        case MA_ERROR:             return "MA_ERROR";
        case MA_INVALID_ARGS:      return "MA_INVALID_ARGS";
        case MA_INVALID_OPERATION: return "MA_INVALID_OPERATION";
        case MA_OUT_OF_MEMORY:     return "MA_OUT_OF_MEMORY";
        case MA_AT_END:            return "MA_AT_END";
        case MA_BUSY:              return "MA_BUSY";
        case MA_INVALID_DATA:      return "MA_INVALID_DATA";
        case MA_NOT_IMPLEMENTED:   return "MA_NOT_IMPLEMENTED";
        default:                   return "MA_OTHER";
    }
}

static const char* variant_name(test_fault fault, ma_uint32 variant)
{
    if (fault == test_fault_format_conversion) {
        return (variant == 0) ? "unknown-format" : "format-query-error";
    }
    if (fault == test_fault_buffer_underrun) {
        return (variant == 0) ? "busy" : "at-end";
    }
    if (fault == test_fault_oversized_frame) {
        return (variant == 0) ? "request+1" : "request+4096";
    }

    return (variant == 0) ? "channels=0" : "channels=255";
}

static void fail_cycle(unsigned cycle, test_fault fault, ma_uint32 variant, const char* pReason)
{
    printf("\n[FAIL] cycle=%u fault=%s variant=%s reason=%s\n",
        cycle, g_FaultNames[fault], variant_name(fault, variant), pReason);
    trace_print();
    g_FailureCount += 1;
}

static void allocator_register(test_alloc_header* pHeader)
{
    pHeader->h.pNext = g_Allocator.pHead;
    g_Allocator.pHead = pHeader;

    g_Allocator.liveCount += 1;
    g_Allocator.totalCount += 1;
    if (g_Allocator.liveCount > g_Allocator.peakCount) {
        g_Allocator.peakCount = g_Allocator.liveCount;
    }
}

static ma_bool32 allocator_unregister(test_alloc_header* pHeader)
{
    test_alloc_header** ppCurrent;

    if (pHeader->h.magic != TEST_ALLOC_MAGIC) {
        g_Allocator.badFreeCount += 1;
        return MA_FALSE;
    }

    ppCurrent = &g_Allocator.pHead;
    while (*ppCurrent != NULL) {
        if (*ppCurrent == pHeader) {
            *ppCurrent = pHeader->h.pNext;
            pHeader->h.pNext = NULL;
            pHeader->h.magic = 0;
            g_Allocator.liveCount -= 1;
            return MA_TRUE;
        }

        ppCurrent = &(*ppCurrent)->h.pNext;
    }

    g_Allocator.badFreeCount += 1;
    return MA_FALSE;
}

static void* test_malloc(size_t size, void* pUserData)
{
    test_alloc_header* pHeader;

    (void)pUserData;

    pHeader = (test_alloc_header*)malloc(sizeof(*pHeader) + size);
    if (pHeader == NULL) {
        return NULL;
    }

    pHeader->h.size = size;
    pHeader->h.magic = TEST_ALLOC_MAGIC;
    allocator_register(pHeader);

    return pHeader + 1;
}

static void test_free(void* p, void* pUserData)
{
    test_alloc_header* pHeader;

    (void)pUserData;

    if (p == NULL) {
        return;
    }

    pHeader = ((test_alloc_header*)p) - 1;
    if (allocator_unregister(pHeader)) {
        free(pHeader);
    }
}

static void* test_realloc(void* p, size_t size, void* pUserData)
{
    test_alloc_header* pHeader;
    test_alloc_header* pNewHeader;

    (void)pUserData;

    if (p == NULL) {
        return test_malloc(size, pUserData);
    }
    if (size == 0) {
        test_free(p, pUserData);
        return NULL;
    }

    pHeader = ((test_alloc_header*)p) - 1;
    if (!allocator_unregister(pHeader)) {
        return NULL;
    }

    pNewHeader = (test_alloc_header*)realloc(pHeader, sizeof(*pNewHeader) + size);
    if (pNewHeader == NULL) {
        allocator_register(pHeader);
        return NULL;
    }

    pNewHeader->h.size = size;
    pNewHeader->h.magic = TEST_ALLOC_MAGIC;
    allocator_register(pNewHeader);

    return pNewHeader + 1;
}

static ma_result source_read(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
{
    test_source* pSource = (test_source*)pDataSource;
    ma_uint64 i;
    float* pFrames = (float*)pFramesOut;

    pSource->readCallCount += 1;
    *pFramesRead = 0;

    if (pSource->faultActive) {
        pSource->faultSeen = MA_TRUE;

        if (pSource->fault == test_fault_buffer_underrun) {
            return (pSource->faultVariant == 0) ? MA_BUSY : MA_AT_END;
        }

        if (pSource->fault == test_fault_oversized_frame) {
            if (pFramesOut != NULL) {
                for (i = 0; i < frameCount * pSource->channels; i += 1) {
                    pFrames[i] = 0.25f;
                }
            }

            pSource->cursor += frameCount;
            *pFramesRead = frameCount + ((pSource->faultVariant == 0) ? 1 : 4096);
            return MA_SUCCESS;
        }
    }

    if (pFramesOut != NULL) {
        for (i = 0; i < frameCount * pSource->channels; i += 1) {
            pFrames[i] = 0.25f;
        }
    }

    pSource->cursor += frameCount;
    *pFramesRead = frameCount;
    return MA_SUCCESS;
}

static ma_result source_seek(ma_data_source* pDataSource, ma_uint64 frameIndex)
{
    test_source* pSource = (test_source*)pDataSource;
    pSource->cursor = frameIndex;
    return MA_SUCCESS;
}

static ma_result source_get_data_format(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap)
{
    test_source* pSource = (test_source*)pDataSource;

    pSource->formatCallCount += 1;

    if (pSource->faultActive) {
        pSource->faultSeen = MA_TRUE;

        if (pSource->fault == test_fault_format_conversion) {
            if (pSource->faultVariant == 0) {
                *pFormat = ma_format_unknown;
                *pChannels = pSource->channels;
                *pSampleRate = pSource->sampleRate;
                return MA_SUCCESS;
            }

            return MA_INVALID_DATA;
        }

        if (pSource->fault == test_fault_channel_count) {
            *pFormat = pSource->format;
            *pChannels = (pSource->faultVariant == 0) ? 0 : (MA_MAX_CHANNELS + 1);
            *pSampleRate = pSource->sampleRate;
            return MA_SUCCESS;
        }
    }

    *pFormat = pSource->format;
    *pChannels = pSource->channels;
    *pSampleRate = pSource->sampleRate;

    if (pChannelMap != NULL && channelMapCap >= pSource->channels) {
        pChannelMap[0] = MA_CHANNEL_FRONT_LEFT;
        pChannelMap[1] = MA_CHANNEL_FRONT_RIGHT;
    }

    return MA_SUCCESS;
}

static ma_result source_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor)
{
    test_source* pSource = (test_source*)pDataSource;
    *pCursor = pSource->cursor;
    return MA_SUCCESS;
}

static ma_result source_get_length(ma_data_source* pDataSource, ma_uint64* pLength)
{
    (void)pDataSource;
    *pLength = 0;
    return MA_NOT_IMPLEMENTED;
}

static ma_result source_set_looping(ma_data_source* pDataSource, ma_bool32 isLooping)
{
    (void)pDataSource;
    (void)isLooping;
    return MA_SUCCESS;
}

static ma_data_source_vtable g_SourceVTable =
{
    source_read,
    source_seek,
    source_get_data_format,
    source_get_cursor,
    source_get_length,
    source_set_looping,
    0
};

static ma_bool32 output_is_silent(const float* pFrames, ma_uint64 frameCount, ma_uint32 channels)
{
    ma_uint64 i;

    for (i = 0; i < frameCount * channels; i += 1) {
        if (pFrames[i] != 0.0f) {
            return MA_FALSE;
        }
    }

    return MA_TRUE;
}

static ma_bool32 output_is_expected(const float* pFrames, ma_uint64 frameCount, ma_uint32 channels)
{
    ma_uint64 i;

    for (i = 0; i < frameCount * channels; i += 1) {
        if (!(pFrames[i] > 0.249999f && pFrames[i] < 0.250001f)) {
            return MA_FALSE;
        }
    }

    return MA_TRUE;
}

static ma_bool32 source_init(test_source* pSource)
{
    ma_data_source_config config;

    memset(pSource, 0, sizeof(*pSource));
    pSource->format = ma_format_f32;
    pSource->channels = 2;
    pSource->sampleRate = 48000;

    config = ma_data_source_config_init();
    config.vtable = &g_SourceVTable;

    return ma_data_source_init(&config, pSource) == MA_SUCCESS;
}

static int run_cycle(unsigned cycle)
{
    ma_result result;
    ma_engine engine;
    ma_engine_config engineConfig;
    ma_sound sound;
    ma_sound_config soundConfig;
    test_source source;
    float output[RENDER_FRAME_COUNT * 2];
    ma_uint64 framesRead;
    size_t liveBefore;
    ma_bool32 engineInitialized = MA_FALSE;
    ma_bool32 soundInitialized = MA_FALSE;
    test_fault fault;
    ma_uint32 variant;
    unsigned cycleFailures;

    fault = (test_fault)(cycle % 4);
    variant = (cycle / 4) % 2;
    cycleFailures = g_FailureCount;

    trace_reset();

    if (!source_init(&source)) {
        fail_cycle(cycle, fault, variant, "test data source initialization failed");
        return 1;
    }
    trace_add("ma_data_source_init(format=f32, channels=2, sampleRate=48000) -> MA_SUCCESS");

    liveBefore = g_Allocator.liveCount;

    engineConfig = ma_engine_config_init();
    engineConfig.noDevice = MA_TRUE;
    engineConfig.noAutoStart = MA_TRUE;
    engineConfig.channels = 2;
    engineConfig.sampleRate = 48000;
    engineConfig.periodSizeInFrames = PERIOD_FRAME_COUNT;
    engineConfig.allocationCallbacks.pUserData = NULL;
    engineConfig.allocationCallbacks.onMalloc = test_malloc;
    engineConfig.allocationCallbacks.onRealloc = test_realloc;
    engineConfig.allocationCallbacks.onFree = test_free;

    result = ma_engine_init(&engineConfig, &engine);
    trace_add("ma_engine_init(noDevice=1, channels=2, sampleRate=48000, periodSizeInFrames=128) -> %s", result_name(result));
    if (result != MA_SUCCESS) {
        fail_cycle(cycle, fault, variant, "engine initialization failed");
        ma_data_source_uninit(&source);
        return 1;
    }
    engineInitialized = MA_TRUE;

    soundConfig = ma_sound_config_init_2(&engine);
    soundConfig.pDataSource = &source;
    soundConfig.flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;

    result = ma_sound_init_ex(&engine, &soundConfig, &sound);
    trace_add("ma_sound_init_from_data_source(flags=NO_SPATIALIZATION|NO_PITCH) -> %s", result_name(result));
    if (result != MA_SUCCESS) {
        fail_cycle(cycle, fault, variant, "sound initialization failed");
        goto cycle_done;
    }
    soundInitialized = MA_TRUE;

    result = ma_sound_start(&sound);
    trace_add("ma_sound_start() -> %s", result_name(result));
    if (result != MA_SUCCESS) {
        fail_cycle(cycle, fault, variant, "sound start failed");
        goto cycle_done;
    }

    source.fault = fault;
    source.faultVariant = variant;
    source.faultActive = MA_TRUE;
    source.faultSeen = MA_FALSE;

    trace_add("inject(%s, %s)", g_FaultNames[fault], variant_name(fault, variant));

    memset(output, 0xA5, sizeof(output));
    result = ma_engine_read_pcm_frames(&engine, output, RENDER_FRAME_COUNT, &framesRead);
    trace_add("ma_engine_read_pcm_frames(%u) -> %s, framesRead=%u", RENDER_FRAME_COUNT, result_name(result), (unsigned)framesRead);

    if (!source.faultSeen) {
        fail_cycle(cycle, fault, variant, "fault was not observed by the data source");
        goto cycle_done;
    }
    g_FaultObservedCount += 1;

    if (framesRead > RENDER_FRAME_COUNT) {
        fail_cycle(cycle, fault, variant, "renderer reported more frames than requested");
        goto cycle_done;
    }
    if (!output_is_silent(output, RENDER_FRAME_COUNT, 2)) {
        fail_cycle(cycle, fault, variant, "fault output was not fully silenced");
        goto cycle_done;
    }

    source.faultActive = MA_FALSE;
    trace_add("clear injected fault");

    if (!ma_sound_is_playing(&sound)) {
        result = ma_sound_start(&sound);
        trace_add("ma_sound_start(recovery) -> %s", result_name(result));
        if (result != MA_SUCCESS) {
            fail_cycle(cycle, fault, variant, "sound state was not recoverable");
            goto cycle_done;
        }
    }

    memset(output, 0xA5, sizeof(output));
    result = ma_engine_read_pcm_frames(&engine, output, RENDER_FRAME_COUNT, &framesRead);
    trace_add("ma_engine_read_pcm_frames(%u, recovery) -> %s, framesRead=%u", RENDER_FRAME_COUNT, result_name(result), (unsigned)framesRead);

    if (result != MA_SUCCESS || framesRead != RENDER_FRAME_COUNT) {
        fail_cycle(cycle, fault, variant, "recovery render did not complete");
        goto cycle_done;
    }
    if (!output_is_expected(output, RENDER_FRAME_COUNT, 2)) {
        fail_cycle(cycle, fault, variant, "recovery render produced unexpected samples");
        goto cycle_done;
    }
    if (!ma_sound_is_playing(&sound)) {
        fail_cycle(cycle, fault, variant, "sound did not return to the playing state");
        goto cycle_done;
    }

    g_RecoveryCount += 1;
    g_FaultCounts[fault] += 1;
    g_VariantCounts[fault][variant] += 1;

cycle_done:
    source.faultActive = MA_FALSE;

    if (soundInitialized) {
        trace_add("ma_sound_uninit()");
        ma_sound_uninit(&sound);
    }
    if (engineInitialized) {
        trace_add("ma_engine_uninit()");
        ma_engine_uninit(&engine);
    }

    trace_add("ma_data_source_uninit()");
    ma_data_source_uninit(&source);

    if (g_Allocator.liveCount != liveBefore) {
        printf("\n[FAIL] cycle=%u fault=%s variant=%s reason=leak, live=%u expected=%u\n",
            cycle, g_FaultNames[fault], variant_name(fault, variant),
            (unsigned)g_Allocator.liveCount, (unsigned)liveBefore);
        trace_print();
        g_FailureCount += 1;
        g_LeakCount += 1;
    }

    if (g_Allocator.badFreeCount != 0) {
        fail_cycle(cycle, fault, variant, "allocator observed an invalid or double free");
    }

    return (g_FailureCount == cycleFailures) ? 0 : 1;
}

int main(int argc, char** argv)
{
    unsigned cycle;
    unsigned cycleCount = DEFAULT_CYCLE_COUNT;
    unsigned result = 0;
    unsigned i;
    unsigned j;

    if (argc > 1) {
        cycleCount = (unsigned)atoi(argv[1]);
        if (cycleCount == 0) {
            printf("cycle count must be greater than 0\n");
            return 1;
        }
    }

    memset(&g_Allocator, 0, sizeof(g_Allocator));

    printf("offline render fault injection: cycles=%u, frames=%u, period=%u\n",
        cycleCount, RENDER_FRAME_COUNT, PERIOD_FRAME_COUNT);

    for (cycle = 0; cycle < cycleCount; cycle += 1) {
        result |= run_cycle(cycle);
        if (g_FailureCount > 0) {
            break;
        }
    }

    printf("\nsummary\n");
    printf("  cycles requested: %u\n", cycleCount);
    printf("  cycles completed: %u\n", cycle);
    printf("  faults observed:  %u\n", g_FaultObservedCount);
    printf("  recoveries:       %u\n", g_RecoveryCount);
    printf("  failures:         %u\n", g_FailureCount);
    printf("  leaks:            %u\n", g_LeakCount);
    printf("  allocations:      live=%u peak=%u total=%u badFree=%u\n",
        (unsigned)g_Allocator.liveCount,
        (unsigned)g_Allocator.peakCount,
        (unsigned)g_Allocator.totalCount,
        (unsigned)g_Allocator.badFreeCount);

    for (i = 0; i < 4; i += 1) {
        printf("  %-17s %u", g_FaultNames[i], g_FaultCounts[i]);
        for (j = 0; j < 2; j += 1) {
            printf(" [%s=%u]", variant_name((test_fault)i, j), g_VariantCounts[i][j]);
        }
        printf("\n");
    }

    if (cycle != cycleCount) {
        result = 1;
    }
    if (g_Allocator.liveCount != 0 || g_Allocator.badFreeCount != 0) {
        result = 1;
    }

    printf("  result:           %s\n", (result == 0) ? "PASS" : "FAIL");
    return (result == 0) ? 0 : 1;
}
