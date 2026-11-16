// Ours (MIT). See iceland-camera-pattern.h. No framework, no allocation: the picture is written straight into the pixel
// buffer the pool handed out.
#include "iceland-camera-pattern.h"

static void iceland_camera_pattern_rect(uint8_t *bgra, size_t stride, int width, int height, int x0, int y0, int w, int h,
                                        uint8_t blue, uint8_t green, uint8_t red)
{
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= height)
            continue;
        uint8_t *row = bgra + (size_t)y * stride;
        for (int x = x0; x < x0 + w; x++) {
            if (x < 0 || x >= width)
                continue;
            uint8_t *pixel = row + (size_t)x * 4;
            pixel[0] = blue;
            pixel[1] = green;
            pixel[2] = red;
            pixel[3] = 0xFF;
        }
    }
}

void iceland_camera_pattern_fill(uint8_t *bgra, size_t stride, int width, int height, uint32_t frame)
{
    // White, yellow, cyan, green, magenta, red, blue, black: 75 % bars, the colours a camera test chart starts with.
    static const uint8_t bars[8][3] = {
        {235, 235, 235}, { 41, 235, 235}, {235, 235,  41}, { 41, 235,  41},
        {235,  41, 235}, { 41,  41, 235}, {235,  41,  41}, { 16,  16,  16},
    };
    if (!bgra || width <= 0 || height <= 0)
        return;

    const int shift = (int)((frame * 3u) % (uint32_t)width);
    for (int y = 0; y < height; y++) {
        uint8_t *row = bgra + (size_t)y * stride;
        for (int x = 0; x < width; x++) {
            int moved = x + shift;
            if (moved >= width)
                moved -= width;
            int index = (moved * 8) / width;
            if (index > 7)
                index = 7;
            uint8_t *pixel = row + (size_t)x * 4;
            pixel[0] = bars[index][0];
            pixel[1] = bars[index][1];
            pixel[2] = bars[index][2];
            pixel[3] = 0xFF;
        }
    }

    // The frame number: bit 0 leftmost of the top row, bit 31 rightmost of the bottom row, a white block for one and a
    // black block for zero, on a black plate.
    {
        enum { cell = 12, gap = 2 };
        const int plate_width = 16 * (cell + gap) + gap;
        const int plate_height = 2 * (cell + gap) + gap;
        iceland_camera_pattern_rect(bgra, stride, width, height, 8, 8, plate_width, plate_height, 0, 0, 0);
        for (int bit = 0; bit < 32; bit++) {
            uint8_t value = ((frame >> bit) & 1u) ? 0xFF : 0x00;
            int x = 8 + gap + (bit % 16) * (cell + gap);
            int y = 8 + gap + (bit / 16) * (cell + gap);
            iceland_camera_pattern_rect(bgra, stride, width, height, x, y, cell, cell, value, value, value);
        }
    }
}
