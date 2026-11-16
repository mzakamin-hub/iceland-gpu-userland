// Ours (MIT). The test picture of IcelandCamera: eight colour bars that move with the frame and the frame number as 32
// blocks of white and black, so a captured picture says by itself which frame it is and that it is fresh. Pure C, used
// when there is no kernel camera service, when ICELAND_CAMERA_PATTERN=1, or when /Library/Iceland/camera-pattern exists.
#ifndef ICELAND_CAMERA_PATTERN_H
#define ICELAND_CAMERA_PATTERN_H

#include <stddef.h>
#include <stdint.h>

// Fill one 8-bit BGRA picture of `width` x `height`, `stride` bytes per line. `frame` starts at 0 and increases by one
// per picture; the bars move and the counter block changes with it.
void iceland_camera_pattern_fill(uint8_t *bgra, size_t stride, int width, int height, uint32_t frame);

#endif
