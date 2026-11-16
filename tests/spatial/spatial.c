#define MA_NO_DEVICE_IO
#include "../common/common.c"


static ma_bool32 spatial_nearly_equal(float a, float b, float tolerance)
{
    return ma_abs(a - b) <= tolerance;
}

static ma_result spatial_check_pan_case(ma_offline_spatializer* pSpatializer, const char* pName, ma_vec3f sourcePosition, ma_vec3f listenerPosition, ma_vec3f listenerForward, ma_vec3f listenerUp, float expectedPan, float expectedLeft, float expectedRight)
{
    ma_result result;
    float left;
    float right;

    result = ma_offline_spatializer_update(pSpatializer, sourcePosition, listenerPosition, listenerForward, listenerUp);
    if (result != MA_SUCCESS) {
        printf("    update failed: %s\n", pName);
        return result;
    }

    result = ma_offline_spatializer_get_target_channel_gains(pSpatializer, &left, &right);
    if (result != MA_SUCCESS) {
        printf("    get gains failed: %s\n", pName);
        return result;
    }

    printf("    %-12s pan=%+.6f left=%.8f right=%.8f\n", pName, ma_offline_spatializer_get_pan(pSpatializer), left, right);

    if (!spatial_nearly_equal(ma_offline_spatializer_get_pan(pSpatializer), expectedPan, 0.0001f) ||
        !spatial_nearly_equal(left, expectedLeft, 0.0001f) ||
        !spatial_nearly_equal(right, expectedRight, 0.0001f))
    {
        printf("    unexpected panning data: %s\n", pName);
        return MA_ERROR;
    }

    return MA_SUCCESS;
}

static ma_result test_spatial__panning(void)
{
    ma_result result;
    ma_offline_spatializer_config config;
    ma_offline_spatializer spatializer;
    ma_vec3f listenerPosition;
    ma_vec3f listenerForward;
    ma_vec3f listenerUp;
    const float centerGain = 0.70710678f;

    printf("  Panning table\n");

    config = ma_offline_spatializer_config_init(48000);
    config.smoothingTimeInSeconds = 0;
    config.maxDistance = 100;

    result = ma_offline_spatializer_init(&config, &spatializer);
    if (result != MA_SUCCESS) {
        return result;
    }

    listenerPosition = ma_vec3f_init_3f(0, 0, 0);
    listenerForward  = ma_vec3f_init_3f(0, 0, -1);
    listenerUp       = ma_vec3f_init_3f(0, 1, 0);

    result = spatial_check_pan_case(&spatializer, "front",      ma_vec3f_init_3f( 0.00000000f, 0, -1.00000000f), listenerPosition, listenerForward, listenerUp,  0.00000000f, centerGain, centerGain);
    if (result == MA_SUCCESS) {
        result = spatial_check_pan_case(&spatializer, "left",       ma_vec3f_init_3f(-1.00000000f, 0,  0.00000000f), listenerPosition, listenerForward, listenerUp, -1.00000000f, 1.00000000f, 0.00000000f);
    }
    if (result == MA_SUCCESS) {
        result = spatial_check_pan_case(&spatializer, "right",      ma_vec3f_init_3f( 1.00000000f, 0,  0.00000000f), listenerPosition, listenerForward, listenerUp, +1.00000000f, 0.00000000f, 1.00000000f);
    }
    if (result == MA_SUCCESS) {
        result = spatial_check_pan_case(&spatializer, "front-right", ma_vec3f_init_3f( 0.70710678f, 0, -0.70710678f), listenerPosition, listenerForward, listenerUp, +0.70710678f, 0.22801343f, 0.97365308f);
    }
    if (result == MA_SUCCESS) {
        result = spatial_check_pan_case(&spatializer, "back",       ma_vec3f_init_3f( 0.00000000f, 0,  1.00000000f), listenerPosition, listenerForward, listenerUp,  0.00000000f, centerGain, centerGain);
    }
    if (result == MA_SUCCESS) {
        result = spatial_check_pan_case(&spatializer, "back-right", ma_vec3f_init_3f( 0.70710678f, 0,  0.70710678f), listenerPosition, listenerForward, listenerUp, +0.70710678f, 0.22801343f, 0.97365308f);
    }

    /* Rotate the listener 90 degrees and verify that the right axis follows. */
    if (result == MA_SUCCESS) {
        result = spatial_check_pan_case(&spatializer, "rotated",    ma_vec3f_init_3f( 0.00000000f, 0,  1.00000000f), listenerPosition, ma_vec3f_init_3f(1, 0, 0), listenerUp, +1.00000000f, 0.00000000f, 1.00000000f);
    }

    ma_offline_spatializer_uninit(&spatializer);

    if (result == MA_SUCCESS) {
        config.handedness = ma_handedness_left;
        result = ma_offline_spatializer_init(&config, &spatializer);
        if (result == MA_SUCCESS) {
            result = spatial_check_pan_case(&spatializer, "left-handed", ma_vec3f_init_3f(1.00000000f, 0, 0), listenerPosition, ma_vec3f_init_3f(0, 0, 1), listenerUp, +1.00000000f, 0.00000000f, 1.00000000f);
            ma_offline_spatializer_uninit(&spatializer);
        }
    }

    return result;
}

static ma_result spatial_check_distance_case(ma_offline_spatializer* pSpatializer, const char* pName, float distance, float expectedGain)
{
    ma_result result;
    float gain;

    result = ma_offline_spatializer_update(pSpatializer, ma_vec3f_init_3f(0, 0, -distance), ma_vec3f_init_3f(0, 0, 0), ma_vec3f_init_3f(0, 0, -1), ma_vec3f_init_3f(0, 1, 0));
    if (result != MA_SUCCESS) {
        return result;
    }

    gain = ma_offline_spatializer_get_distance_gain(pSpatializer);
    printf("    %-14s distance=%6.3f gain=%.8f\n", pName, ma_offline_spatializer_get_distance(pSpatializer), gain);

    if (!spatial_nearly_equal(ma_offline_spatializer_get_distance(pSpatializer), distance, 0.0001f) ||
        !spatial_nearly_equal(gain, expectedGain, 0.0001f))
    {
        return MA_ERROR;
    }

    return MA_SUCCESS;
}

static ma_result spatial_test_distance_model(ma_attenuation_model attenuationModel, float minGain, float maxGain)
{
    ma_result result;
    ma_offline_spatializer_config config;
    ma_offline_spatializer spatializer;

    config = ma_offline_spatializer_config_init(48000);
    config.smoothingTimeInSeconds = 0;
    config.attenuationModel = attenuationModel;
    config.minDistance = 1;
    config.maxDistance = 10;
    config.rolloff = 1;
    config.minGain = minGain;
    config.maxGain = maxGain;

    result = ma_offline_spatializer_init(&config, &spatializer);
    if (result != MA_SUCCESS) {
        return result;
    }

    if (attenuationModel == ma_attenuation_model_inverse) {
        result = spatial_check_distance_case(&spatializer, "inverse 1",  1.0f, ma_clamp(1.00000000f, minGain, maxGain));
        if (result == MA_SUCCESS) {
            result = spatial_check_distance_case(&spatializer, "inverse 2",  2.0f, ma_clamp(0.50000000f, minGain, maxGain));
        }
        if (result == MA_SUCCESS) {
            result = spatial_check_distance_case(&spatializer, "inverse 10", 10.0f, ma_clamp(0.10000000f, minGain, maxGain));
        }
        if (result == MA_SUCCESS) {
            result = spatial_check_distance_case(&spatializer, "inverse 20", 20.0f, ma_clamp(0.10000000f, minGain, maxGain));
        }
    } else if (attenuationModel == ma_attenuation_model_linear) {
        result = spatial_check_distance_case(&spatializer, "linear 1",   1.0f, ma_clamp(1.00000000f, minGain, maxGain));
        if (result == MA_SUCCESS) {
            result = spatial_check_distance_case(&spatializer, "linear 5.5", 5.5f, ma_clamp(0.50000000f, minGain, maxGain));
        }
        if (result == MA_SUCCESS) {
            result = spatial_check_distance_case(&spatializer, "linear 10",  10.0f, ma_clamp(0.00000000f, minGain, maxGain));
        }
        if (result == MA_SUCCESS) {
            result = spatial_check_distance_case(&spatializer, "linear 20",  20.0f, ma_clamp(0.00000000f, minGain, maxGain));
        }
    } else if (attenuationModel == ma_attenuation_model_exponential) {
        result = spatial_check_distance_case(&spatializer, "exp 1",      1.0f, ma_clamp(1.00000000f, minGain, maxGain));
        if (result == MA_SUCCESS) {
            result = spatial_check_distance_case(&spatializer, "exp 2",      2.0f, ma_clamp(0.50000000f, minGain, maxGain));
        }
        if (result == MA_SUCCESS) {
            result = spatial_check_distance_case(&spatializer, "exp 10",     10.0f, ma_clamp(0.10000000f, minGain, maxGain));
        }
        if (result == MA_SUCCESS) {
            result = spatial_check_distance_case(&spatializer, "exp 20",     20.0f, ma_clamp(0.10000000f, minGain, maxGain));
        }
    } else {
        result = spatial_check_distance_case(&spatializer, "none 20",    20.0f, ma_clamp(1.00000000f, minGain, maxGain));
    }

    ma_offline_spatializer_uninit(&spatializer);
    return result;
}

static ma_result test_spatial__distance(void)
{
    ma_result result;
    ma_offline_spatializer_config config;
    ma_offline_spatializer spatializer;
    float left;
    float right;

    printf("  Distance models\n");

    result = spatial_test_distance_model(ma_attenuation_model_inverse, 0, 1);
    if (result == MA_SUCCESS) {
        result = spatial_test_distance_model(ma_attenuation_model_linear, 0, 1);
    }
    if (result == MA_SUCCESS) {
        result = spatial_test_distance_model(ma_attenuation_model_exponential, 0, 1);
    }
    if (result == MA_SUCCESS) {
        result = spatial_test_distance_model(ma_attenuation_model_none, 0, 1);
    }
    if (result == MA_SUCCESS) {
        result = spatial_test_distance_model(ma_attenuation_model_linear, 0.25f, 1.0f);
    }
    if (result == MA_SUCCESS) {
        result = spatial_test_distance_model(ma_attenuation_model_inverse, 0.0f, 0.8f);
    }

    if (result == MA_SUCCESS) {
        config = ma_offline_spatializer_config_init(48000);
        config.smoothingTimeInSeconds = 0;
        config.maxDistance = 10;
        result = ma_offline_spatializer_init(&config, &spatializer);
        if (result == MA_SUCCESS) {
            result = ma_offline_spatializer_update(&spatializer, ma_vec3f_init_3f(0, 0, 0), ma_vec3f_init_3f(0, 0, 0), ma_vec3f_init_3f(0, 0, -1), ma_vec3f_init_3f(0, 1, 0));
        }
        if (result == MA_SUCCESS) {
            result = ma_offline_spatializer_get_target_channel_gains(&spatializer, &left, &right);
        }
        if (result == MA_SUCCESS) {
            printf("    coincident     distance=%6.3f pan=%+.6f left=%.8f right=%.8f\n",
                   ma_offline_spatializer_get_distance(&spatializer),
                   ma_offline_spatializer_get_pan(&spatializer),
                   left,
                   right);
            if (!spatial_nearly_equal(ma_offline_spatializer_get_distance(&spatializer), 0, 0.0001f) ||
                !spatial_nearly_equal(ma_offline_spatializer_get_pan(&spatializer), 0, 0.0001f) ||
                !spatial_nearly_equal(left, 0.70710678f, 0.0001f) ||
                !spatial_nearly_equal(right, 0.70710678f, 0.0001f))
            {
                result = MA_ERROR;
            }
        }
        ma_offline_spatializer_uninit(&spatializer);
    }

    return result;
}

static ma_result test_spatial__smoothing(void)
{
    ma_result result;
    ma_offline_spatializer_config config;
    ma_offline_spatializer spatializer;
    ma_vec3f listenerPosition;
    ma_vec3f listenerForward;
    ma_vec3f listenerUp;
    float input[4];
    float output[8];
    float previousLeft;
    float previousRight;
    float maxDelta;
    ma_uint32 i;

    printf("  Smoothing continuity and update churn\n");

    config = ma_offline_spatializer_config_init(48000);
    config.smoothingTimeInSeconds = 0.01f;
    config.maxDistance = 100;

    result = ma_offline_spatializer_init(&config, &spatializer);
    if (result != MA_SUCCESS) {
        return result;
    }

    listenerPosition = ma_vec3f_init_3f(0, 0, 0);
    listenerForward  = ma_vec3f_init_3f(0, 0, -1);
    listenerUp       = ma_vec3f_init_3f(0, 1, 0);

    for (i = 0; i < 4; i += 1) {
        input[i] = 1;
    }

    result = ma_offline_spatializer_update(&spatializer, ma_vec3f_init_3f(-10, 0, 0), listenerPosition, listenerForward, listenerUp);
    if (result != MA_SUCCESS) {
        ma_offline_spatializer_uninit(&spatializer);
        return result;
    }

    for (i = 0; i < 1200; i += 1) {
        result = ma_offline_spatializer_process_pcm_frames(&spatializer, output, input, 4);
        if (result != MA_SUCCESS) {
            ma_offline_spatializer_uninit(&spatializer);
            return result;
        }
    }

    previousLeft  = output[6];
    previousRight = output[7];
    maxDelta = 0;

    for (i = 0; i < 10000; i += 1) {
        float x;
        ma_uint32 iFrame;

        x = ((i & 1) == 0) ? 10.0f : -10.0f;
        result = ma_offline_spatializer_update(&spatializer, ma_vec3f_init_3f(x, 0, 0), listenerPosition, listenerForward, listenerUp);
        if (result != MA_SUCCESS) {
            ma_offline_spatializer_uninit(&spatializer);
            return result;
        }

        result = ma_offline_spatializer_process_pcm_frames(&spatializer, output, input, 4);
        if (result != MA_SUCCESS) {
            ma_offline_spatializer_uninit(&spatializer);
            return result;
        }

        for (iFrame = 0; iFrame < 4; iFrame += 1) {
            float deltaLeft  = ma_abs(output[iFrame * 2 + 0] - previousLeft);
            float deltaRight = ma_abs(output[iFrame * 2 + 1] - previousRight);

            if (deltaLeft > maxDelta) {
                maxDelta = deltaLeft;
            }
            if (deltaRight > maxDelta) {
                maxDelta = deltaRight;
            }

            if (output[iFrame * 2 + 0] != output[iFrame * 2 + 0] ||
                output[iFrame * 2 + 1] != output[iFrame * 2 + 1] ||
                ma_abs(output[iFrame * 2 + 0]) > 0.1001f ||
                ma_abs(output[iFrame * 2 + 1]) > 0.1001f)
            {
                printf("    invalid output at update %u, frame %u\n", i, iFrame);
                ma_offline_spatializer_uninit(&spatializer);
                return MA_ERROR;
            }

            previousLeft  = output[iFrame * 2 + 0];
            previousRight = output[iFrame * 2 + 1];
        }
    }

    printf("    10000 updates, maximum adjacent-sample gain delta = %.8f\n", maxDelta);

    if (maxDelta > 0.02f) {
        printf("    smoothing discontinuity detected\n");
        result = MA_ERROR;
    }

    ma_offline_spatializer_uninit(&spatializer);
    return result;
}

static ma_result test_spatial__edge_cases(void)
{
    ma_result result;
    ma_offline_spatializer_config config;
    ma_offline_spatializer spatializer;
    ma_vec3f sourcePosition;
    ma_vec3f listenerPosition;
    ma_vec3f listenerForward;
    ma_vec3f listenerUp;
    float input[8];
    float output[16];
    ma_uint32 i;

    printf("  Edge cases\n");

    result = ma_offline_spatializer_init(NULL, &spatializer);
    if (result != MA_INVALID_ARGS) {
        return MA_ERROR;
    }

    config = ma_offline_spatializer_config_init(0);
    if (ma_offline_spatializer_init(&config, &spatializer) != MA_INVALID_ARGS) {
        return MA_ERROR;
    }

    config = ma_offline_spatializer_config_init(48000);
    config.minDistance = 0;
    if (ma_offline_spatializer_init(&config, &spatializer) != MA_INVALID_ARGS) {
        return MA_ERROR;
    }

    config = ma_offline_spatializer_config_init(48000);
    config.maxDistance = config.minDistance - 1;
    if (ma_offline_spatializer_init(&config, &spatializer) != MA_INVALID_ARGS) {
        return MA_ERROR;
    }

    config = ma_offline_spatializer_config_init(48000);
    config.minGain = 0.5f;
    config.maxGain = 0.25f;
    if (ma_offline_spatializer_init(&config, &spatializer) != MA_INVALID_ARGS) {
        return MA_ERROR;
    }

    config = ma_offline_spatializer_config_init(48000);
    config.smoothingTimeInSeconds = -1;
    if (ma_offline_spatializer_init(&config, &spatializer) != MA_INVALID_ARGS) {
        return MA_ERROR;
    }

    config = ma_offline_spatializer_config_init(48000);
    config.smoothingTimeInSeconds = 0;
    config.maxDistance = 5;
    result = ma_offline_spatializer_init(&config, &spatializer);
    if (result != MA_SUCCESS) {
        return result;
    }

    for (i = 0; i < 8; i += 1) {
        input[i] = 1;
    }

    if (ma_offline_spatializer_update(NULL, ma_vec3f_init_3f(0, 0, 0), ma_vec3f_init_3f(0, 0, 0), ma_vec3f_init_3f(0, 0, -1), ma_vec3f_init_3f(0, 1, 0)) != MA_INVALID_ARGS) {
        ma_offline_spatializer_uninit(&spatializer);
        return MA_ERROR;
    }

    if (ma_offline_spatializer_process_pcm_frames(NULL, output, input, 1) != MA_INVALID_ARGS) {
        ma_offline_spatializer_uninit(&spatializer);
        return MA_ERROR;
    }

    if (ma_offline_spatializer_process_pcm_frames(&spatializer, NULL, input, 1) != MA_INVALID_ARGS) {
        ma_offline_spatializer_uninit(&spatializer);
        return MA_ERROR;
    }

    if (ma_offline_spatializer_process_pcm_frames(&spatializer, output, NULL, 1) != MA_INVALID_ARGS) {
        ma_offline_spatializer_uninit(&spatializer);
        return MA_ERROR;
    }

    if (ma_offline_spatializer_process_pcm_frames(&spatializer, NULL, NULL, 0) != MA_SUCCESS) {
        ma_offline_spatializer_uninit(&spatializer);
        return MA_ERROR;
    }

    sourcePosition   = ma_vec3f_init_3f(0, 0, -2);
    listenerPosition = ma_vec3f_init_3f(0, 0, 0);
    listenerForward  = ma_vec3f_init_3f(0, 1, 0);
    listenerUp       = ma_vec3f_init_3f(0, 1, 0);

    result = ma_offline_spatializer_update(&spatializer, sourcePosition, listenerPosition, listenerForward, listenerUp);
    if (result != MA_SUCCESS) {
        ma_offline_spatializer_uninit(&spatializer);
        return result;
    }

    result = ma_offline_spatializer_process_pcm_frames(&spatializer, output, input, 8);
    if (result != MA_SUCCESS) {
        ma_offline_spatializer_uninit(&spatializer);
        return result;
    }

    for (i = 0; i < 16; i += 1) {
        if (output[i] != output[i] || ma_abs(output[i]) > 1.0f) {
            printf("    non-finite output with degenerate listener basis\n");
            ma_offline_spatializer_uninit(&spatializer);
            return MA_ERROR;
        }
    }

    if (ma_offline_spatializer_update(&spatializer, ma_vec3f_init_3f(MA_FLT_MAX, MA_FLT_MAX, MA_FLT_MAX), listenerPosition, listenerForward, listenerUp) != MA_SUCCESS) {
        ma_offline_spatializer_uninit(&spatializer);
        return MA_ERROR;
    }

    if (ma_offline_spatializer_update(&spatializer, ma_vec3f_init_3f(0, 0, 0), listenerPosition, ma_vec3f_init_3f(MA_FLT_MAX, MA_FLT_MAX, MA_FLT_MAX), listenerUp) != MA_SUCCESS) {
        ma_offline_spatializer_uninit(&spatializer);
        return MA_ERROR;
    }

    ma_offline_spatializer_uninit(&spatializer);
    return MA_SUCCESS;
}

static int test_entry__panning(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    return (test_spatial__panning() == MA_SUCCESS) ? 0 : -1;
}

static int test_entry__distance(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    return (test_spatial__distance() == MA_SUCCESS) ? 0 : -1;
}

static int test_entry__smoothing(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    return (test_spatial__smoothing() == MA_SUCCESS) ? 0 : -1;
}

static int test_entry__edge_cases(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    return (test_spatial__edge_cases() == MA_SUCCESS) ? 0 : -1;
}

int main(int argc, char** argv)
{
    ma_register_test("Panning",    test_entry__panning);
    ma_register_test("Distance",   test_entry__distance);
    ma_register_test("Smoothing",  test_entry__smoothing);
    ma_register_test("Edge Cases", test_entry__edge_cases);

    return ma_run_tests(argc, argv);
}
