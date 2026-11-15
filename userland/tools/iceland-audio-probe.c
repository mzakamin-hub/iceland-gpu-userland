// Ours (MIT). iceland-audio-probe: print the audio devices coreaudiod publishes, one line each, so a boot log shows
// whether the HAL plug-in's device arrived without any app. Public CoreAudio calls only.
//
//   iceland-audio-probe            -> "audio-probe: N devices" and "audio-probe: <id> in=<channels> out=<channels> <name>"
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>

static UInt32 channels(AudioObjectID device, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address = {kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain};
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

int main(void)
{
    AudioObjectPropertyAddress address = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size);
    if (status != kAudioHardwareNoError) {
        printf("audio-probe: no device list (status %d)\n", (int)status);
        return 1;
    }
    UInt32 count = size / (UInt32)sizeof(AudioObjectID);
    AudioObjectID *devices = calloc(count ? count : 1, sizeof(AudioObjectID));
    if (!devices || (count && AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, devices) != kAudioHardwareNoError)) {
        printf("audio-probe: device list not read\n");
        return 1;
    }
    printf("audio-probe: %u devices\n", (unsigned)count);
    for (UInt32 i = 0; i < count; i++) {
        AudioObjectPropertyAddress name_address = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        CFStringRef name = NULL;
        UInt32 name_size = sizeof(name);
        char text[256] = "?";
        if (AudioObjectGetPropertyData(devices[i], &name_address, 0, NULL, &name_size, &name) == kAudioHardwareNoError && name) {
            CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8);
            CFRelease(name);
        }
        printf("audio-probe: %u in=%u out=%u %s\n", (unsigned)devices[i], (unsigned)channels(devices[i], kAudioObjectPropertyScopeInput),
               (unsigned)channels(devices[i], kAudioObjectPropertyScopeOutput), text);
    }
    free(devices);
    return 0;
}
