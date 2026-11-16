// Ours (MIT). The user client of the in-kernel camera driver: service "IcelandCamera", one connection, all calls scalar
// (IOConnectCallScalarMethod), all frame buffers one read-only mapping. The kernel side is the camera work's; this file is
// the only place that knows the numbers, so the stream source and tools/iceland-camera-probe can share them.
//
//   selector 0 start: 0 in, 1 out (0 = ok)
//   selector 1 stop:  0 in, 0 out
//   selector 2 wait:  2 in (last sequence, timeout ms), 8 out: sequence, buffer index, width, height, stride bytes,
//                     gain x128, exposure lines, mach_absolute_time of the frame; kIOReturnSuccess, kIOReturnTimeout or
//                     kIOReturnNotReady (the service is not streaming)
//   selector 3 info:  0 in, 8 out: state (0 idle, 1 starting, 2 streaming, 3 failed), width, height, stride, bytes per
//                     buffer, buffer count, the newest frame's sequence, clients
//   selector 4 note:  3 in (kind, a, b), 0 out: one line in the kernel log, "icecamsvc: note pid P: <kind> a b", so a
//                     boot log taken from the kernel alone tells what the userland did (the kinds are below)
//   memory type 0 (IOConnectMapMemory64, kIOMapAnywhere | kIOMapReadOnly): every buffer back to back, buffer i at
//                     i * bytes per buffer, each RAW10 MIPI-packed (see raw10.h)
//
// The ring: the driver writes its three buffers in turn. The buffer of frame S stays as it is until frame S + 2 is
// announced, so a reader copies (or converts) the frame and then asks selector 3: if the newest sequence is S + 2 or
// more, the copy may be torn and is dropped.
#ifndef ICELAND_CAMERA_KERNEL_H
#define ICELAND_CAMERA_KERNEL_H

#include <IOKit/IOKitLib.h>
#include <IOKit/IOTypes.h>

#include <stdint.h>

#define ICELAND_CAMERA_KERNEL_SERVICE "IcelandCamera"

enum {
    ICELAND_CAMERA_KERNEL_SELECTOR_START = 0,
    ICELAND_CAMERA_KERNEL_SELECTOR_STOP = 1,
    ICELAND_CAMERA_KERNEL_SELECTOR_WAIT = 2,
    ICELAND_CAMERA_KERNEL_SELECTOR_INFO = 3,
    ICELAND_CAMERA_KERNEL_SELECTOR_NOTE = 4,
};

// Selector 4's kinds: what the kernel log line says, and what a and b are.
enum {
    ICELAND_CAMERA_NOTE_PROVIDER_UP = 0,    // "cmio start": a = 1 pattern mode, 0 kernel mode
    ICELAND_CAMERA_NOTE_CLIENT = 1,         // "cmio client": a = the client's pid
    ICELAND_CAMERA_NOTE_STREAM_START = 2,   // "cmio stream start": a = 1 pattern mode, 0 kernel mode
    ICELAND_CAMERA_NOTE_FRAMES = 3,         // "cmio frames served": a = pictures sent, b = the kernel sequence
    ICELAND_CAMERA_NOTE_STREAM_STOP = 4,    // "cmio stream stop": a = pictures sent, b = frames dropped as torn
    ICELAND_CAMERA_NOTE_FAILURE = 5,        // "cmio failure": a = where (the stream source's own number), b = a status
    ICELAND_CAMERA_NOTE_HOST = 6,           // "host activation": a = 0 asked, 1 needs approval, 2 finished, 3 failed; b = code
    ICELAND_CAMERA_NOTE_PROBE = 7,          // "probe": a = CoreMediaIO devices listed, b = 1 when ours is among them
    ICELAND_CAMERA_NOTE_PROBE_FRAMES = 8,   // "probe frames": a = frames a CoreMediaIO client received, b = a status
};

// The eight scalars selector 3 answers with.
typedef struct {
    uint64_t state;
    uint64_t width;
    uint64_t height;
    uint64_t stride;
    uint64_t bytes_per_buffer;
    uint64_t buffer_count;
    uint64_t frames;                   // the newest frame's sequence
    uint64_t clients;
} iceland_camera_kernel_info;

// The eight scalars selector 2 answers with.
typedef struct {
    uint64_t sequence;
    uint64_t index;
    uint64_t width;
    uint64_t height;
    uint64_t stride;
    uint64_t gain;
    uint64_t exposure;
    uint64_t host_time;
} iceland_camera_kernel_frame;

typedef struct {
    io_connect_t connection;
    uint64_t buffers_address;          // the mapping, or 0
    uint64_t buffers_size;
    uint64_t bytes_per_buffer;
    uint64_t last_sequence;
} iceland_camera_kernel;

enum {
    ICELAND_CAMERA_KERNEL_FRAME = 0,     // *frame holds the picture
    ICELAND_CAMERA_KERNEL_TIMEOUT = 1,   // no picture within the timeout
    ICELAND_CAMERA_KERNEL_NOT_READY = 2, // the service is not streaming
    ICELAND_CAMERA_KERNEL_FAILED = 3,    // the call itself failed
};

// 1 when the service is in the IORegistry.
int iceland_camera_kernel_present(void);

// 0 on success; the connection is then open and must be closed. Returns 1 when the service is absent, 2 when it refuses
// the user client (a sandbox without the exception, the wrong class, or a client limit).
int iceland_camera_kernel_open(iceland_camera_kernel *kernel);
void iceland_camera_kernel_close(iceland_camera_kernel *kernel);

int iceland_camera_kernel_start(iceland_camera_kernel *kernel);      // 0 = streaming
void iceland_camera_kernel_stop(iceland_camera_kernel *kernel);
int iceland_camera_kernel_get_info(iceland_camera_kernel *kernel, iceland_camera_kernel_info *info);

// Map memory type 0 read-only: `buffer_count` buffers of `bytes_per_buffer` back to back. 0 on success.
int iceland_camera_kernel_map(iceland_camera_kernel *kernel, uint64_t bytes_per_buffer, uint64_t buffer_count);
void iceland_camera_kernel_unmap(iceland_camera_kernel *kernel);

// The RAW10 bytes of buffer `index`, or NULL when the index is outside the mapping.
const uint8_t *iceland_camera_kernel_buffer(const iceland_camera_kernel *kernel, uint64_t index);

// Wait for the next frame after the last one returned: 0 = a frame is in *frame, 1 = timeout, 2 = not streaming,
// 3 = the call failed. The sequence is remembered in the kernel struct.
int iceland_camera_kernel_wait(iceland_camera_kernel *kernel, uint32_t timeout_ms, iceland_camera_kernel_frame *frame);

// 1 when the buffer of `frame` may have been written again since the wait returned it (see "The ring" above), or when
// the kernel could not be asked.
int iceland_camera_kernel_torn(iceland_camera_kernel *kernel, const iceland_camera_kernel_frame *frame);

// One line in the kernel log (selector 4), through a connection of the process's own that is opened at the first call
// and kept. Does nothing when the service is absent or refuses the connection.
void iceland_camera_note(uint64_t kind, uint64_t a, uint64_t b);

#endif
