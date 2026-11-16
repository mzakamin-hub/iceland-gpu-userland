// Ours (MIT). iceland-camera-probe: what the camera looks like from outside, so a boot log says whether the pieces are
// there. Public APIs only: IOKit for the in-kernel IcelandCamera service (../iceland-camera-kernel.h), CoreMediaIO's C
// API for the devices the system lists and for taking pictures the way any camera client does, ImageIO for the one
// picture --save keeps. Exit code 0 whenever it could report; the lines it prints ("camera-probe: ...", stderr) are the
// answer, and the results that matter are also sent to the kernel log (selector 4: "probe", "probe frames" and
// "probe saved").
//
//   iceland-camera-probe                  the service and its info scalars, then the CoreMediaIO devices
//   ... --frames N                        then N frames straight from the user client (start, wait, torn check, stop)
//   ... --wait S                          list the devices again every second until "Front Camera" is there, at most S s
//   ... --cmio-frames N                   then be a CoreMediaIO client of "Front Camera": start its stream, count N
//                                         pictures (at most 20 s), stop
//   ... --sessions K                      do the --frames part K times (the stream down and up again in between)
//   ... --say TEXT                        say "camera-probe: TEXT" and exit at once, nothing else: a shell job's own line
//                                         in the kernel log, which copies the stderr of a process named iceland-camera-*
//   ... --save PATH                       with --cmio-frames: keep one of those pictures (the 30th, or the last when
//                                         fewer arrive), write it as a JPEG to PATH, and say its size, its sha256 and
//                                         the means of B, G and R
//   ... --drop NODE                       with --save: also write that JPEG into NODE (/dev/icedrop, the node that sends
//                                         it to the PC: open O_WRONLY, retry, write in 64 KiB chunks, close)
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreMediaIO/CMIOHardware.h>
#include <CoreVideo/CoreVideo.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <ImageIO/ImageIO.h>
#include <mach/mach.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../iceland-camera-kernel.h"

static const char kOurDeviceName[] = "Front Camera";

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *format, ...)
{
    char text[512];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    fprintf(stderr, "camera-probe: %s\n", text);
    fflush(stderr);
}

static void copy_string(CFStringRef text, char *buffer, size_t length)
{
    buffer[0] = '\0';
    if (text)
        CFStringGetCString(text, buffer, (CFIndex)length, kCFStringEncodingUTF8);
    if (buffer[0] == '\0')
        snprintf(buffer, length, "?");
}

// ---- the kernel service, straight ---------------------------------------------------------------------------------

static void print_info(iceland_camera_kernel *kernel, const char *when)
{
    iceland_camera_kernel_info info;

    if (iceland_camera_kernel_get_info(kernel, &info) != 0) {
        say("IcelandCamera info failed (%s)", when);
        return;
    }
    say("IcelandCamera %s: state=%llu %llux%llu stride=%llu bytes=%llu buffers=%llu seq=%llu clients=%llu", when,
        (unsigned long long)info.state, (unsigned long long)info.width, (unsigned long long)info.height,
        (unsigned long long)info.stride, (unsigned long long)info.bytes_per_buffer, (unsigned long long)info.buffer_count,
        (unsigned long long)info.frames, (unsigned long long)info.clients);
}

static void run_frames(iceland_camera_kernel *kernel, long wanted, long session)
{
    iceland_camera_kernel_info info;
    long served = 0;
    long torn = 0;
    volatile uint64_t touched = 0;
    int attempt;
    int started;
    int why = 0;                       // how the loop ended: 0 all frames, 1 attempts used up, 2 + the wait's answer, 9 a bad index

    if (iceland_camera_kernel_get_info(kernel, &info) != 0 ||
        iceland_camera_kernel_map(kernel, info.bytes_per_buffer, info.buffer_count) != 0) {
        say("session %ld: the frame buffers are not mapped", session);
        return;
    }
    started = iceland_camera_kernel_start(kernel);
    if (started != 0) {
        say("session %ld: start refused (%d)", session, started);
        iceland_camera_kernel_unmap(kernel);
        return;
    }
    print_info(kernel, "after start");
    kernel->last_sequence = 0;
    for (attempt = 0; attempt < 600 && served < wanted; attempt++) {
        iceland_camera_kernel_frame frame;
        int waited = iceland_camera_kernel_wait(kernel, 500, &frame);
        const uint8_t *raw;
        uint64_t sum = 0;
        uint64_t i;
        uint64_t length;

        if (waited == ICELAND_CAMERA_KERNEL_TIMEOUT)
            continue;
        if (waited != ICELAND_CAMERA_KERNEL_FRAME) {
            say("session %ld: wait stopped (%d)", session, waited);
            why = 2 + waited;
            break;
        }
        raw = iceland_camera_kernel_buffer(kernel, frame.index);
        if (!raw) {
            say("session %ld: frame index %llu outside the mapping", session, (unsigned long long)frame.index);
            why = 9;
            break;
        }
        // The whole frame is read, as a converter would: the sum is of the middle line.
        length = frame.stride * frame.height;
        if (length > kernel->bytes_per_buffer)
            length = kernel->bytes_per_buffer;
        for (i = 0; i < length; i += 64)
            touched += raw[i];
        for (i = 0; i < frame.stride; i++)
            sum += raw[(frame.height / 2) * frame.stride + i];
        if (iceland_camera_kernel_torn(kernel, &frame)) {
            torn++;
            continue;
        }
        served++;
        if (served <= 3 || served == wanted) {
            say("session %ld: frame %ld seq=%llu index=%llu stride=%llu gain=%llu middle line mean=%llu.%02llu", session,
                served, (unsigned long long)frame.sequence, (unsigned long long)frame.index,
                (unsigned long long)frame.stride, (unsigned long long)frame.gain,
                (unsigned long long)(frame.stride ? sum / frame.stride : 0),
                (unsigned long long)(frame.stride ? (sum * 100 / frame.stride) % 100 : 0));
        }
    }
    if (why == 0 && served < wanted)
        why = 1;
    say("session %ld: %ld of %ld frames, %ld torn; the loop ended %d after %d waits", session, served, wanted, torn, why, attempt);
    // The same in the kernel log, which is all a tablet run keeps.
    iceland_camera_note(ICELAND_CAMERA_NOTE_PROBE_SESSION, (uint64_t)served, (uint64_t)torn);
    iceland_camera_note(ICELAND_CAMERA_NOTE_PROBE_SESSION_END, (uint64_t)why, (uint64_t)attempt);
    iceland_camera_kernel_stop(kernel);
    iceland_camera_kernel_unmap(kernel);
    print_info(kernel, "after stop");
}

// ---- CoreMediaIO ---------------------------------------------------------------------------------------------------

static const CMIOObjectPropertyAddress kDevicesAddress = {kCMIOHardwarePropertyDevices, kCMIOObjectPropertyScopeGlobal,
                                                          kCMIOObjectPropertyElementMain};

// The number of devices listed; *ours is the device named "Front Camera", or 0.
static unsigned list_devices(CMIOObjectID *ours, int print)
{
    UInt32 size = 0;
    UInt32 used = 0;
    UInt32 count;
    UInt32 i;
    CMIOObjectID *devices;
    OSStatus status = CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &kDevicesAddress, 0, NULL, &size);

    *ours = 0;
    if (status != kCMIOHardwareNoError) {
        if (print)
            say("no CoreMediaIO device list (status %d)", (int)status);
        return 0;
    }
    count = size / (UInt32)sizeof(CMIOObjectID);
    devices = calloc(count ? count : 1, sizeof(CMIOObjectID));
    if (!devices)
        return 0;
    if (count && CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &kDevicesAddress, 0, NULL, size, &used, devices) !=
                     kCMIOHardwareNoError) {
        if (print)
            say("CoreMediaIO device list not read");
        free(devices);
        return 0;
    }
    count = used / (UInt32)sizeof(CMIOObjectID);
    if (print)
        say("%u CoreMediaIO devices", (unsigned)count);
    for (i = 0; i < count; i++) {
        CMIOObjectPropertyAddress name_address = {kCMIOObjectPropertyName, kCMIOObjectPropertyScopeGlobal,
                                                  kCMIOObjectPropertyElementMain};
        CMIOObjectPropertyAddress uid_address = {kCMIODevicePropertyDeviceUID, kCMIOObjectPropertyScopeGlobal,
                                                 kCMIOObjectPropertyElementMain};
        CFStringRef name = NULL;
        CFStringRef uid = NULL;
        UInt32 name_size = 0;
        UInt32 uid_size = 0;
        char name_text[256];
        char uid_text[256];

        if (CMIOObjectGetPropertyData(devices[i], &name_address, 0, NULL, (UInt32)sizeof(name), &name_size, &name) !=
            kCMIOHardwareNoError)
            name = NULL;
        if (CMIOObjectGetPropertyData(devices[i], &uid_address, 0, NULL, (UInt32)sizeof(uid), &uid_size, &uid) !=
            kCMIOHardwareNoError)
            uid = NULL;
        copy_string(name, name_text, sizeof(name_text));
        copy_string(uid, uid_text, sizeof(uid_text));
        if (strcmp(name_text, kOurDeviceName) == 0)
            *ours = devices[i];
        if (print)
            say("device %u %s (%s)", (unsigned)devices[i], name_text, uid_text);
        if (name)
            CFRelease(name);
        if (uid)
            CFRelease(uid);
    }
    free(devices);
    return (unsigned)count;
}

static atomic_long cmio_pictures;
static atomic_long cmio_first_width;
static atomic_long cmio_first_height;
static CMSimpleQueueRef cmio_queue;

// --save keeps one picture: the thirtieth received, or the last one when fewer arrive. queue_altered runs on CoreMediaIO's
// queue, so what it keeps and what main() takes away are under this lock.
static const long kCmioKeptPicture = 30;

static pthread_mutex_t cmio_keep_lock = PTHREAD_MUTEX_INITIALIZER;
static CVImageBufferRef cmio_kept;      // the picture kept, or NULL
static long cmio_kept_number;           // its number: 1 for the first picture received
static const char *cmio_save_path;      // --save's PATH, or NULL

static void queue_altered(CMIOStreamID stream, void *token, void *context)
{
    CMSampleBufferRef sample;

    (void)stream;
    (void)token;
    (void)context;
    while (cmio_queue && (sample = (CMSampleBufferRef)CMSimpleQueueDequeue(cmio_queue)) != NULL) {
        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
        long number = atomic_fetch_add(&cmio_pictures, 1) + 1;

        if (image && number == 1) {
            atomic_store(&cmio_first_width, (long)CVPixelBufferGetWidth(image));
            atomic_store(&cmio_first_height, (long)CVPixelBufferGetHeight(image));
        }
        // Every picture up to the thirtieth replaces the one kept: 30 when they all arrive, the last one when fewer do.
        if (image && cmio_save_path && number <= kCmioKeptPicture) {
            CVImageBufferRef previous;

            CVPixelBufferRetain((CVPixelBufferRef)image);
            pthread_mutex_lock(&cmio_keep_lock);
            previous = cmio_kept;
            cmio_kept = image;
            cmio_kept_number = number;
            pthread_mutex_unlock(&cmio_keep_lock);
            if (previous)
                CVPixelBufferRelease((CVPixelBufferRef)previous);
        }
        CFRelease(sample);
    }
}

// ---- the kept picture, as a JPEG -----------------------------------------------------------------------------------

// Everything, in 64 KiB pieces, as userland/tools/iceland-audio-rec.c writes its WAV.
static int write_all(int fd, const uint8_t *data, size_t size)
{
    while (size) {
        ssize_t written = write(fd, data, size > 65536 ? 65536 : size);

        if (written <= 0)
            return 0;
        data += written;
        size -= (size_t)written;
    }
    return 1;
}

// The drop node (/dev/icedrop) takes one file at a time: try for half a minute, as the recorder's -d does.
static void write_into_node(const char *node, const uint8_t *data, size_t size)
{
    int written = 0;
    int tries;

    for (tries = 0; tries < 30 && !written; tries++) {
        int fd = open(node, O_WRONLY);

        if (fd >= 0) {
            written = write_all(fd, data, size);
            close(fd);
            break;
        }
        sleep(1);
    }
    say("%s: the JPEG was %s", node, written ? "written into it" : "NOT written");
}

// Take the kept picture, make a JPEG of it, write that to `path`, and then into `node` when one was given.
static void save_kept_picture(const char *path, const char *node)
{
    CVImageBufferRef image;
    long number;
    OSType format;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;
    const uint8_t *base = NULL;
    unsigned long long blue = 0;
    unsigned long long green = 0;
    unsigned long long red = 0;
    unsigned long long samples = 0;
    unsigned long long means[3];
    CGColorSpaceRef colorspace = NULL;
    CGContextRef context = NULL;
    CGImageRef picture = NULL;
    CFMutableDataRef jpeg = NULL;
    CGImageDestinationRef destination = NULL;
    const uint8_t *bytes;
    size_t length;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    char sum[2 * CC_SHA256_DIGEST_LENGTH + 1];
    int finalized = 0;
    int written = 0;
    int i;

    pthread_mutex_lock(&cmio_keep_lock);
    image = cmio_kept;
    number = cmio_kept_number;
    cmio_kept = NULL;
    pthread_mutex_unlock(&cmio_keep_lock);
    if (!image) {
        say("no picture was kept for %s", path);
        return;
    }

    format = CVPixelBufferGetPixelFormatType((CVPixelBufferRef)image);
    if (format != kCVPixelFormatType_32BGRA) {
        say("the kept picture %ld is not BGRA ('%c%c%c%c'), not saved", number, (char)((format >> 24) & 0xFF),
            (char)((format >> 16) & 0xFF), (char)((format >> 8) & 0xFF), (char)(format & 0xFF));
        CVPixelBufferRelease((CVPixelBufferRef)image);
        return;
    }
    if (CVPixelBufferLockBaseAddress((CVPixelBufferRef)image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        say("the kept picture %ld could not be locked, not saved", number);
        CVPixelBufferRelease((CVPixelBufferRef)image);
        return;
    }
    width = CVPixelBufferGetWidth((CVPixelBufferRef)image);
    height = CVPixelBufferGetHeight((CVPixelBufferRef)image);
    stride = CVPixelBufferGetBytesPerRow((CVPixelBufferRef)image);
    base = CVPixelBufferGetBaseAddress((CVPixelBufferRef)image);
    if (base && stride >= width * 4) {
        // The means, over every fourth pixel of every fourth line: B, G and R as the picture holds them.
        for (size_t y = 0; y < height; y += 4) {
            const uint8_t *row = base + y * stride;

            for (size_t x = 0; x < width; x += 4) {
                const uint8_t *pixel = row + x * 4;

                blue += pixel[0];
                green += pixel[1];
                red += pixel[2];
                samples++;
            }
        }
        // The context writes nothing: it is only the way a CGImage is made of the picture's own bytes.
        colorspace = CGColorSpaceCreateDeviceRGB();
        context = CGBitmapContextCreate((void *)base, width, height, 8, stride, colorspace,
                                        (CGBitmapInfo)(kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little));
        picture = context ? CGBitmapContextCreateImage(context) : NULL;
        if (context)
            CGContextRelease(context);
        if (colorspace)
            CGColorSpaceRelease(colorspace);
    }
    CVPixelBufferUnlockBaseAddress((CVPixelBufferRef)image, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease((CVPixelBufferRef)image);
    if (!picture) {
        say("the kept picture %ld (%zu x %zu, stride %zu) made no CGImage, not saved", number, width, height, stride);
        return;
    }
    means[0] = samples ? blue * 100 / samples : 0;
    means[1] = samples ? green * 100 / samples : 0;
    means[2] = samples ? red * 100 / samples : 0;

    jpeg = CFDataCreateMutable(kCFAllocatorDefault, 0);
    destination = jpeg ? CGImageDestinationCreateWithData(jpeg, CFSTR("public.jpeg"), 1, NULL) : NULL;
    if (destination) {
        float quality = 0.85f;
        CFNumberRef quality_value = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat32Type, &quality);

        if (quality_value) {
            const void *keys[1] = {kCGImageDestinationLossyCompressionQuality};
            const void *values[1] = {quality_value};
            CFDictionaryRef properties = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                            &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);

            CGImageDestinationAddImage(destination, picture, properties);
            if (properties)
                CFRelease(properties);
            CFRelease(quality_value);
        } else {
            CGImageDestinationAddImage(destination, picture, NULL);
        }
        finalized = CGImageDestinationFinalize(destination) ? 1 : 0;
        CFRelease(destination);
    }
    CGImageRelease(picture);
    if (!finalized || !jpeg || CFDataGetLength(jpeg) <= 0) {
        say("the kept picture %ld (%zu x %zu) made no JPEG, not saved", number, width, height);
        if (jpeg)
            CFRelease(jpeg);
        return;
    }

    bytes = CFDataGetBytePtr(jpeg);
    length = (size_t)CFDataGetLength(jpeg);
    CC_SHA256(bytes, (CC_LONG)length, digest);
    for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(sum + 2 * i, 3, "%02x", digest[i]);
    {
        FILE *file = fopen(path, "wb");

        if (file) {
            written = fwrite(bytes, 1, length, file) == length;
            fclose(file);
        }
    }
    say("saved picture %ld (%zu x %zu) as %s: %zu B JPEG, sha256 %s, mean B G R %llu.%02llu %llu.%02llu %llu.%02llu",
        number, width, height, path, length, sum, means[0] / 100, means[0] % 100, means[1] / 100, means[1] % 100,
        means[2] / 100, means[2] % 100);
    if (!written)
        say("the JPEG could not be written to %s", path);
    // The same in the kernel log: a = the JPEG's bytes, b = the picture's number.
    iceland_camera_note(ICELAND_CAMERA_NOTE_PROBE_SAVED, (uint64_t)length, (uint64_t)number);
    if (node)
        write_into_node(node, bytes, length);
    CFRelease(jpeg);
}

// The pictures a CoreMediaIO client gets from our device; *status is the first call that failed, or 0.
static long run_cmio_frames(CMIOObjectID device, long wanted, int *status_out)
{
    CMIOObjectPropertyAddress streams_address = {kCMIODevicePropertyStreams, kCMIOObjectPropertyScopeGlobal,
                                                 kCMIOObjectPropertyElementMain};
    CMIOStreamID streams[8];
    UInt32 used = 0;
    OSStatus status;
    int tenth;

    *status_out = 0;
    status = CMIOObjectGetPropertyData(device, &streams_address, 0, NULL, (UInt32)sizeof(streams), &used, streams);
    if (status != kCMIOHardwareNoError || used < sizeof(CMIOStreamID)) {
        say("cmio client: no stream on device %u (status %d)", (unsigned)device, (int)status);
        *status_out = status ? (int)status : -1;
        return 0;
    }
    status = CMIOStreamCopyBufferQueue(streams[0], queue_altered, NULL, &cmio_queue);
    if (status != kCMIOHardwareNoError || !cmio_queue) {
        say("cmio client: no buffer queue (status %d)", (int)status);
        *status_out = status ? (int)status : -2;
        return 0;
    }
    status = CMIODeviceStartStream(device, streams[0]);
    if (status != kCMIOHardwareNoError) {
        say("cmio client: the stream did not start (status %d)", (int)status);
        *status_out = (int)status;
        return 0;
    }
    say("cmio client: stream %u of device %u started, pid %d", (unsigned)streams[0], (unsigned)device, (int)getpid());
    for (tenth = 0; tenth < 200 && atomic_load(&cmio_pictures) < wanted; tenth++)
        usleep(100000);
    status = CMIODeviceStopStream(device, streams[0]);
    say("cmio client: %ld of %ld pictures in %d.%d s, the first %ld x %ld; stop status %d", atomic_load(&cmio_pictures),
        wanted, tenth / 10, tenth % 10, atomic_load(&cmio_first_width), atomic_load(&cmio_first_height), (int)status);
    return atomic_load(&cmio_pictures);
}

int main(int argc, char **argv)
{
    long frames = 0;
    long cmio_frames = 0;
    long wait_seconds = 0;
    long sessions = 1;
    const char *say_text = NULL;
    const char *save_path = NULL;
    const char *drop_node = NULL;
    iceland_camera_kernel kernel;
    CMIOObjectID ours = 0;
    unsigned listed;
    int opened;
    long i;

    for (i = 1; i < argc; i++) {
        long *value = NULL;

        if (strcmp(argv[i], "--frames") == 0)
            value = &frames;
        else if (strcmp(argv[i], "--cmio-frames") == 0)
            value = &cmio_frames;
        else if (strcmp(argv[i], "--wait") == 0)
            value = &wait_seconds;
        else if (strcmp(argv[i], "--sessions") == 0)
            value = &sessions;
        else if (i + 1 < argc) {
            if (strcmp(argv[i], "--say") == 0)
                say_text = argv[++i];
            else if (strcmp(argv[i], "--save") == 0)
                save_path = argv[++i];
            else if (strcmp(argv[i], "--drop") == 0)
                drop_node = argv[++i];
        }
        if (value && i + 1 < argc) {
            *value = strtol(argv[++i], NULL, 10);
            if (*value < 0)
                *value = 0;
        }
    }

    // --say is a shell job putting its own line into the kernel log (the kernel copies the stderr of a process named
    // iceland-camera-*): nothing else happens, and nothing else is touched.
    if (say_text) {
        say("%s", say_text);
        return 0;
    }

    opened = iceland_camera_kernel_open(&kernel);
    if (opened == 0) {
        say("IOKit service %s present, user client open", ICELAND_CAMERA_KERNEL_SERVICE);
        print_info(&kernel, "as found");
    } else {
        say("IOKit service %s %s", ICELAND_CAMERA_KERNEL_SERVICE, opened == 1 ? "absent" : "present, user client refused");
    }
    for (i = 1; opened == 0 && frames > 0 && i <= sessions; i++) {
        run_frames(&kernel, frames, i);
        if (i < sessions)
            sleep(2);
    }

    listed = list_devices(&ours, 1);
    for (i = 0; i < wait_seconds && !ours; i++) {
        sleep(1);
        listed = list_devices(&ours, 0);
    }
    if (wait_seconds > 0)
        listed = list_devices(&ours, 1);
    say("%s is %s the CoreMediaIO devices (%u listed, waited %ld s)", kOurDeviceName, ours ? "among" : "NOT among", listed, i);
    iceland_camera_note(ICELAND_CAMERA_NOTE_PROBE, listed, ours ? 1 : 0);

    if (cmio_frames > 0 && ours) {
        int status = 0;
        long got;

        // From here on queue_altered keeps a picture for --save.
        cmio_save_path = save_path;
        got = run_cmio_frames(ours, cmio_frames, &status);
        iceland_camera_note(ICELAND_CAMERA_NOTE_PROBE_FRAMES, (uint64_t)got, (uint64_t)(uint32_t)status);
        if (save_path)
            save_kept_picture(save_path, drop_node);
    } else if (save_path) {
        say("--save needs --cmio-frames and a device to run against: no picture was kept");
    }
    if (opened == 0)
        iceland_camera_kernel_close(&kernel);
    return 0;
}
