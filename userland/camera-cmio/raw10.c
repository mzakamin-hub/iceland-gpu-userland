// Ours (MIT). See raw10.h: MIPI-packed BGGR RAW10 to BGRA, the arithmetic the camera's picture path is built on, with a
// self-test so a wrong table or a wrong group offset is caught without a camera. No framework, no allocation per frame.
#include "raw10.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// sqrt(v * 255) over 0...255: the sensor's response is close enough to linear in light that a square root is what makes
// the picture look like a picture; the table is built once, on the first frame.
static uint8_t iceland_raw10_tone[256];
static int iceland_raw10_tone_ready;

static void iceland_raw10_build_tone(void)
{
    if (iceland_raw10_tone_ready)
        return;
    for (int value = 0; value < 256; value++) {
        double level = sqrt((double)value * 255.0);
        if (level > 255.0)
            level = 255.0;
        iceland_raw10_tone[value] = (uint8_t)level;
    }
    iceland_raw10_tone_ready = 1;
}

// Black level, white balance, tone curve: one channel of one pixel.
static uint8_t iceland_raw10_channel(int sample, int gain)
{
    int level = sample - ICELAND_RAW10_BLACK_OFFSET;
    if (level < 0)
        level = 0;
    level = (level * gain) >> 8;
    if (level > 255)
        level = 255;
    return iceland_raw10_tone[level];
}

void iceland_raw10_to_bgra(const uint8_t *raw, size_t raw_stride, uint8_t *bgra, size_t bgra_stride)
{
    iceland_raw10_build_tone();
    for (size_t y = 0; y < ICELAND_RAW10_OUTPUT_HEIGHT; y++) {
        const uint8_t *row0 = raw + (2 * y) * raw_stride;      // the quad's B G B G line
        const uint8_t *row1 = row0 + raw_stride;               // the quad's G R G R line
        uint8_t *out = bgra + y * bgra_stride;
        for (size_t x = 0; x < ICELAND_RAW10_OUTPUT_WIDTH; x += 2) {
            const uint8_t *group0 = row0 + (x / 2) * ICELAND_RAW10_GROUP_BYTES;
            const uint8_t *group1 = row1 + (x / 2) * ICELAND_RAW10_GROUP_BYTES;
            for (size_t k = 0; k < 2; k++) {
                uint8_t *pixel = out + (x + k) * 4;
                int blue = group0[2 * k];
                int green = (group0[2 * k + 1] + group1[2 * k]) / 2;
                int red = group1[2 * k + 1];
                pixel[0] = iceland_raw10_channel(blue, ICELAND_RAW10_GAIN_BLUE);
                pixel[1] = iceland_raw10_channel(green, ICELAND_RAW10_GAIN_GREEN);
                pixel[2] = iceland_raw10_channel(red, ICELAND_RAW10_GAIN_RED);
                pixel[3] = 0xFF;
            }
        }
    }
}

static int iceland_raw10_check(const char *what, int got, int want)
{
    if (got == want)
        return 0;
    fprintf(stderr, "iceland_raw10_selftest: %s is %d, expected %d\n", what, got, want);
    return 1;
}

int iceland_raw10_selftest(void)
{
    enum { kStride = 2048 };
    const size_t raw_size = (size_t)kStride * ICELAND_RAW10_SENSOR_HEIGHT;
    const size_t bgra_stride = (size_t)ICELAND_RAW10_OUTPUT_WIDTH * 4;
    const size_t bgra_size = bgra_stride * ICELAND_RAW10_OUTPUT_HEIGHT;
    uint8_t *raw = calloc(1, raw_size);
    uint8_t *bgra = calloc(1, bgra_size);
    int failures = 0;

    if (!raw || !bgra) {
        free(raw);
        free(bgra);
        return 1;
    }

    // The first four pixels of the first two lines: B G B G over G R G R, five bytes per four pixels, the fifth byte the
    // four low bits the picture path ignores.
    raw[0] = 100; raw[1] = 120; raw[2] = 130; raw[3] = 140;
    raw[kStride + 0] = 110; raw[kStride + 1] = 125; raw[kStride + 2] = 150; raw[kStride + 3] = 160;

    iceland_raw10_to_bgra(raw, kStride, bgra, bgra_stride);
    // (0,0): B 100-16=84 -> *410>>8 = 134 -> sqrt(134*255) = 184; G (120+110)/2-16 = 99 -> 158; R 125-16=109 -> 196 ->
    // 223.
    failures += iceland_raw10_check("pixel (0,0) B", bgra[0], 184);
    failures += iceland_raw10_check("pixel (0,0) G", bgra[1], 158);
    failures += iceland_raw10_check("pixel (0,0) R", bgra[2], 223);
    failures += iceland_raw10_check("pixel (0,0) A", bgra[3], 255);
    // (1,0): B 130-16=114 -> 182 -> 215; G (140+150)/2-16 = 129 -> 181; R 160-16=144 -> *461>>8 = 259, clamped -> 255.
    failures += iceland_raw10_check("pixel (1,0) B", bgra[4], 215);
    failures += iceland_raw10_check("pixel (1,0) G", bgra[5], 181);
    failures += iceland_raw10_check("pixel (1,0) R", bgra[6], 255);
    failures += iceland_raw10_check("pixel (1,0) A", bgra[7], 255);

    for (size_t i = 0; i < (size_t)ICELAND_RAW10_OUTPUT_WIDTH * ICELAND_RAW10_OUTPUT_HEIGHT && failures == 0; i++) {
        if (bgra[i * 4 + 3] != 0xFF) {
            fprintf(stderr, "iceland_raw10_selftest: pixel %zu has alpha %d\n", i, bgra[i * 4 + 3]);
            failures++;
        }
    }

    // A uniform frame at the far end: the highest sample 255 minus black 16 keeps every channel bright, and the last line
    // of the picture is the last line pair of the frame only if the line pair loop and the strides are right.
    memset(raw, 0xFF, raw_size);
    iceland_raw10_to_bgra(raw, kStride, bgra, bgra_stride);
    {
        size_t last = ((size_t)ICELAND_RAW10_OUTPUT_HEIGHT - 1) * bgra_stride + ((size_t)ICELAND_RAW10_OUTPUT_WIDTH - 1) * 4;
        failures += iceland_raw10_check("uniform B", bgra[last], 255);
        failures += iceland_raw10_check("uniform G", bgra[last + 1], 246);
        failures += iceland_raw10_check("uniform R", bgra[last + 2], 255);
        failures += iceland_raw10_check("uniform A", bgra[last + 3], 255);
    }

    free(raw);
    free(bgra);
    if (failures) {
        fprintf(stderr, "iceland_raw10_selftest: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
