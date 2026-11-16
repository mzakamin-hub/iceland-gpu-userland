// Ours (MIT). iceland-audio-rec: record from an input device through CoreAudio as an app would (an AudioQueue input
// queue on the device, coreaudiod in between) and write a 16-bit mono WAV, so a boot with nobody at the tablet can
// show that macOS itself hears the microphone. Public CoreAudio / AudioToolbox calls only.
//
//   iceland-audio-rec [-s seconds] [-o file.wav] [-u device-UID] [-w seconds] [-d node]
//     -s  how long to record (default 25, at most 120)
//     -o  the WAV file (default /private/tmp/iceland-mic.wav)
//     -u  the input device's UID (default: "Mic", the kernel's IcelandAudio microphone; else a device whose name has
//         "icrophone" or "Mic"; else the system's default input)
//     -w  first wait up to this many seconds for sysctl kern.icemic_live = 1 (the kernel's capture service is up)
//     -d  also write the finished WAV into this node (/dev/icedrop: the kernel sends it to the PC) and close it
//
// Every line starts "audio-rec: " (the kernel copies a program's output into its log). The last line is
// "audio-rec: DONE ..." with the frames, the peak and RMS level and the WAV's SHA-256, or "audio-rec: FAIL ...".
// Exit 0: frames were recorded and the WAV written; 1: a call failed; 2: it ran but every sample was zero (the audio
// server gives silence to a program that may not use the microphone).
#include <AudioToolbox/AudioToolbox.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

struct recording {
    pthread_mutex_t lock;
    int16_t *samples;
    size_t frames, capacity;
    unsigned callbacks;
    bool running;
};

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("audio-rec: ", stdout);
    vfprintf(stdout, format, arguments);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(arguments);
}

static void take(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer, const AudioTimeStamp *start,
                 UInt32 packets, const AudioStreamPacketDescription *descriptions)
{
    struct recording *r = context;
    (void)start;
    (void)packets;
    (void)descriptions;
    pthread_mutex_lock(&r->lock);
    size_t frames = buffer->mAudioDataByteSize / sizeof(int16_t);
    if (frames > r->capacity - r->frames)
        frames = r->capacity - r->frames;
    memcpy(r->samples + r->frames, buffer->mAudioData, frames * sizeof(int16_t));
    r->frames += frames;
    r->callbacks++;
    bool again = r->running;
    pthread_mutex_unlock(&r->lock);
    if (again)
        AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

static UInt32 input_channels(AudioObjectID device)
{
    AudioObjectPropertyAddress address = {kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    UInt32 size = 0, total = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, NULL, &size) != kAudioHardwareNoError || size == 0)
        return 0;
    AudioBufferList *list = malloc(size);
    if (list && AudioObjectGetPropertyData(device, &address, 0, NULL, &size, list) == kAudioHardwareNoError)
        for (UInt32 i = 0; i < list->mNumberBuffers; i++)
            total += list->mBuffers[i].mNumberChannels;
    free(list);
    return total;
}

static bool text_of(AudioObjectID device, AudioObjectPropertySelector selector, char *text, size_t room)
{
    AudioObjectPropertyAddress address = {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef string = NULL;
    UInt32 size = sizeof(string);
    text[0] = 0;
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &string) != kAudioHardwareNoError || !string)
        return false;
    bool ok = CFStringGetCString(string, text, (CFIndex)room, kCFStringEncodingUTF8);
    CFRelease(string);
    return ok;
}

// The input device to record from: its UID into `uid`, its nominal rate into `rate`. Prints every input device.
static bool choose(const char *wanted, char *uid, size_t room, Float64 *rate)
{
    AudioObjectPropertyAddress address = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    AudioObjectID chosen = kAudioObjectUnknown, fallback = kAudioObjectUnknown, standard = kAudioObjectUnknown;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != kAudioHardwareNoError) {
        say("no device list");
        return false;
    }
    UInt32 count = size / (UInt32)sizeof(AudioObjectID);
    AudioObjectID *devices = calloc(count ? count : 1, sizeof(AudioObjectID));
    if (!devices || (count && AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, devices) != kAudioHardwareNoError)) {
        say("device list not read");
        free(devices);
        return false;
    }
    AudioObjectPropertyAddress standard_address = {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    size = sizeof(standard);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &standard_address, 0, NULL, &size, &standard) != kAudioHardwareNoError)
        standard = kAudioObjectUnknown;
    for (UInt32 i = 0; i < count; i++) {
        char name[256], id[256];
        UInt32 channels = input_channels(devices[i]);
        text_of(devices[i], kAudioObjectPropertyName, name, sizeof(name));
        text_of(devices[i], kAudioDevicePropertyDeviceUID, id, sizeof(id));
        say("device %u: in=%u uid \"%s\" name \"%s\"%s", (unsigned)devices[i], (unsigned)channels, id, name,
            devices[i] == standard ? " (the default input)" : "");
        if (channels == 0)
            continue;
        if (strcmp(id, wanted) == 0)
            chosen = devices[i];
        else if (fallback == kAudioObjectUnknown && (strstr(name, "icrophone") || strstr(name, "Mic") || strstr(id, "Mic")))
            fallback = devices[i];
    }
    free(devices);
    if (chosen == kAudioObjectUnknown)
        chosen = fallback != kAudioObjectUnknown ? fallback : standard;
    if (chosen == kAudioObjectUnknown || input_channels(chosen) == 0) {
        say("no input device (wanted uid \"%s\")", wanted);
        return false;
    }
    if (!text_of(chosen, kAudioDevicePropertyDeviceUID, uid, room)) {
        say("the chosen device %u has no UID", (unsigned)chosen);
        return false;
    }
    AudioObjectPropertyAddress rate_address = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    size = sizeof(*rate);
    if (AudioObjectGetPropertyData(chosen, &rate_address, 0, NULL, &size, rate) != kAudioHardwareNoError || *rate < 8000 || *rate > 192000)
        *rate = 48000;
    say("recording from device %u, uid \"%s\", %u Hz", (unsigned)chosen, uid, (unsigned)*rate);
    return true;
}

static void put32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static bool write_all(int fd, const uint8_t *data, size_t size)
{
    while (size) {
        ssize_t n = write(fd, data, size > 65536 ? 65536 : size);
        if (n <= 0)
            return false;
        data += n;
        size -= (size_t)n;
    }
    return true;
}

int main(int argc, char **argv)
{
    const char *out = "/private/tmp/iceland-mic.wav", *wanted = "Mic", *drop = NULL;
    int seconds = 25, wait_live = 0, option;
    while ((option = getopt(argc, argv, "s:o:u:w:d:")) != -1) {
        switch (option) {
        case 's': seconds = atoi(optarg); break;
        case 'o': out = optarg; break;
        case 'u': wanted = optarg; break;
        case 'w': wait_live = atoi(optarg); break;
        case 'd': drop = optarg; break;
        default:
            say("FAIL usage: iceland-audio-rec [-s seconds] [-o file.wav] [-u device-UID] [-w seconds] [-d node]");
            return 1;
        }
    }
    if (seconds < 1 || seconds > 120)
        seconds = 25;
    say("pid %d uid %d: %d s to %s%s%s", (int)getpid(), (int)getuid(), seconds, out, drop ? ", then into " : "", drop ? drop : "");
    for (int waited = 0; waited < wait_live; waited++) {
        int live = 0;
        size_t size = sizeof(live);
        if (sysctlbyname("kern.icemic_live", &live, &size, NULL, 0) != 0) {
            say("no sysctl kern.icemic_live: not waiting");
            break;
        }
        if (live) {
            say("kern.icemic_live = 1 after %d s", waited);
            break;
        }
        if (waited + 1 == wait_live)
            say("kern.icemic_live still 0 after %d s: recording anyway", wait_live);
        sleep(1);
    }

    char uid[256];
    Float64 rate = 48000;
    if (!choose(wanted, uid, sizeof(uid), &rate)) {
        say("FAIL no input device");
        return 1;
    }
    struct recording r;
    memset(&r, 0, sizeof(r));
    pthread_mutex_init(&r.lock, NULL);
    r.capacity = (size_t)rate * (size_t)seconds;
    r.samples = calloc(r.capacity, sizeof(int16_t));
    if (!r.samples) {
        say("FAIL no memory");
        return 1;
    }
    AudioStreamBasicDescription format;
    memset(&format, 0, sizeof(format));
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBytesPerPacket = 2;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 2;
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 16;
    AudioQueueRef queue = NULL;
    OSStatus status = AudioQueueNewInput(&format, take, &r, NULL, NULL, 0, &queue);
    if (status != noErr || !queue) {
        say("FAIL AudioQueueNewInput %d", (int)status);
        return 1;
    }
    CFStringRef device = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    status = AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice, &device, sizeof(device));
    if (status != noErr)
        say("AudioQueueSetProperty(CurrentDevice \"%s\") %d: the queue keeps the default input", uid, (int)status);
    UInt32 bytes = (UInt32)(rate / 10) * 2;                   // 100 ms a buffer
    r.running = true;
    for (int i = 0; i < 4; i++) {
        AudioQueueBufferRef buffer = NULL;
        status = AudioQueueAllocateBuffer(queue, bytes, &buffer);
        if (status == noErr)
            status = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
        if (status != noErr) {
            say("FAIL a queue buffer: %d", (int)status);
            return 1;
        }
    }
    status = AudioQueueStart(queue, NULL);
    if (status != noErr) {
        say("FAIL AudioQueueStart %d", (int)status);
        return 1;
    }
    say("STARTED: the input queue runs");
    size_t frames = 0;
    for (int tenths = 0; tenths < (seconds + 6) * 10 && frames < r.capacity; tenths++) {
        usleep(100000);
        pthread_mutex_lock(&r.lock);
        frames = r.frames;
        pthread_mutex_unlock(&r.lock);
        if (tenths % 50 == 49)
            say("%zu frames after %d s", frames, (tenths + 1) / 10);
    }
    pthread_mutex_lock(&r.lock);
    r.running = false;
    frames = r.frames;
    unsigned callbacks = r.callbacks;
    pthread_mutex_unlock(&r.lock);
    AudioQueueStop(queue, true);
    AudioQueueDispose(queue, true);
    CFRelease(device);
    say("STOPPED: %zu of %zu frames in %u buffers", frames, r.capacity, callbacks);

    int peak = 0;
    double squares = 0;
    for (size_t i = 0; i < frames; i++) {
        int v = r.samples[i] < 0 ? -(int)r.samples[i] : r.samples[i];
        if (v > peak)
            peak = v;
        squares += (double)r.samples[i] * (double)r.samples[i];
    }
    double peak_db = peak ? 20 * log10(peak / 32768.0) : -99, rms_db = frames && squares > 0 ? 10 * log10(squares / (double)frames / (32768.0 * 32768.0)) : -99;

    size_t data = frames * 2, total = 44 + data;
    uint8_t *wav = malloc(total);
    if (!wav) {
        say("FAIL no memory for the WAV");
        return 1;
    }
    memcpy(wav, "RIFF", 4);
    put32(wav + 4, (uint32_t)(36 + data));
    memcpy(wav + 8, "WAVEfmt ", 8);
    put32(wav + 16, 16);
    wav[20] = 1; wav[21] = 0;                                 // PCM
    wav[22] = 1; wav[23] = 0;                                 // one channel
    put32(wav + 24, (uint32_t)rate);
    put32(wav + 28, (uint32_t)rate * 2);
    wav[32] = 2; wav[33] = 0;
    wav[34] = 16; wav[35] = 0;
    memcpy(wav + 36, "data", 4);
    put32(wav + 40, (uint32_t)data);
    memcpy(wav + 44, r.samples, data);                        // arm64 is little-endian, as WAV is
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    char sum[2 * CC_SHA256_DIGEST_LENGTH + 1];
    CC_SHA256(wav, (CC_LONG)total, digest);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(sum + 2 * i, 3, "%02x", digest[i]);

    bool written = false, dropped = false;
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        written = write_all(fd, wav, total);
        close(fd);
    }
    if (!written)
        say("the WAV could not be written to %s", out);
    if (drop && frames) {
        // the node takes one file at a time: try for a minute
        for (int tries = 0; tries < 30 && !dropped; tries++) {
            fd = open(drop, O_WRONLY);
            if (fd >= 0) {
                dropped = write_all(fd, wav, total);
                close(fd);
                break;
            }
            sleep(2);
        }
        say("%s: %s", drop, dropped ? "the WAV was written into it" : "NOT written");
    }
    if (!frames) {
        say("FAIL no frame came from the queue in %d s (wav %zu bytes sha256 %s)", seconds + 6, total, sum);
        return 1;
    }
    say("DONE %zu frames at %u Hz (%zu.%02zu s), peak %.1f dBFS (%d of 32768), RMS %.1f dBFS%s; wav %zu bytes sha256 %s%s%s",
        frames, (unsigned)rate, frames / (size_t)rate, frames % (size_t)rate * 100 / (size_t)rate, peak_db, peak, rms_db,
        peak == 0 ? ": ALL ZERO (silence: is this program allowed to use the microphone?)" : "", total, sum,
        written ? "" : "; FILE NOT WRITTEN", drop ? (dropped ? "; sent to the node" : "; NOT sent to the node") : "");
    return peak == 0 ? 2 : (written || dropped) ? 0 : 1;
}
