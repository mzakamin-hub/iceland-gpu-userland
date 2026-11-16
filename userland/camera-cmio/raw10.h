// Ours (MIT). The RAW10 to BGRA converter of IcelandCamera: the front sensor's frames arrive MIPI-packed (every four
// pixels of a line in five bytes: the four pixels' top eight bits, then one byte with their four low bits), 1632 x 1224,
// Bayer BGGR, stride given by the kernel. One 2x2 Bayer quad becomes one 816 x 612 picture pixel. Pure C, no framework,
// so it can be unit-tested without a macOS SDK: iceland_raw10_selftest() is a self-contained check of the arithmetic.
//
// The kernel's frames are the top eight bits of the sensor's ten: the sensor's black level of 64 of 1023 is 16 here.
#ifndef ICELAND_RAW10_H
#define ICELAND_RAW10_H

#include <stddef.h>
#include <stdint.h>

// The sensor's geometry, as the kernel service reports it, and the picture's: two quads per four pixels, two lines per
// line pair.
#define ICELAND_RAW10_SENSOR_WIDTH 1632
#define ICELAND_RAW10_SENSOR_HEIGHT 1224
#define ICELAND_RAW10_OUTPUT_WIDTH 816
#define ICELAND_RAW10_OUTPUT_HEIGHT 612

// The four pixels of a group share five bytes; a picture pixel pair needs one group of each of the two lines.
#define ICELAND_RAW10_GROUP_BYTES 5

// The sensor's black level, 64 of 1023, in the top eight bits the kernel keeps.
#define ICELAND_RAW10_BLACK_OFFSET 16

// The white balance the sensor's neutral response asks for in this light, 8.8 fixed point.
#define ICELAND_RAW10_GAIN_RED 461
#define ICELAND_RAW10_GAIN_GREEN 256
#define ICELAND_RAW10_GAIN_BLUE 410

// Convert one RAW10 frame (BGGR, `raw_stride` bytes per line) into an 816 x 612 BGRA picture (`bgra_stride` bytes per
// line; the width is fixed, the stride need not be 3264). The converter keeps a 256-entry tone curve and is meant to run
// on one queue only.
void iceland_raw10_to_bgra(const uint8_t *raw, size_t raw_stride, uint8_t *bgra, size_t bgra_stride);

// 0 when the converter still does what this header says: two hand-computed pixels of a synthetic frame, alpha, and a
// uniform frame at the far end of the last line. Prints the first difference to stderr and returns 1 otherwise.
int iceland_raw10_selftest(void);

#endif
