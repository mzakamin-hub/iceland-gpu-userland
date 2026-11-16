// Ours (MIT). IcelandCamera: the CoreMediaIO camera extension provider of the OnePlus Pad 4's front sensor. The process
// registers a provider ("Iceland Camera Provider", manufacturer "iceland") with one device ("Front Camera") and one
// stream (816 x 612, 32BGRA, 30 fps), and hands that stream pictures from one of two places, chosen at start:
//
//   kernel   the in-kernel IcelandCamera service (IOKit user client, see iceland-camera-kernel.h): RAW10 1632 x 1224
//            Bayer frames, turned into BGRA by raw10.c;
//   pattern  colour bars with a frame counter, for a boot without the camera driver, a build test, or when
//            ICELAND_CAMERA_PATTERN=1 (any value but "0") or the file /Library/Iceland/camera-pattern exists.
//
// What it does is said twice: on stderr and os_log ("CMIO ..." lines, iceland-camera-log.h), and, when the kernel
// service is there, as lines of the kernel log (selector 4, iceland-camera-kernel.h), which is the log a tablet run keeps.
//
// How the process is started on the tablet is not this file's decision; nothing here depends on it (the CMIOExtension
// mach service is Info.plist's CMIOExtensionMachServiceName, and the host app in host/ asks the system to activate it).
// Public macOS SDK only.
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMediaIO/CMIOExtension.h>
#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#import "iceland-camera-device.h"
#import "iceland-camera-kernel.h"
#import "iceland-camera-log.h"
#include "raw10.h"

static const char kIcelandCameraPatternFile[] = "/Library/Iceland/camera-pattern";
static const char kIcelandCameraPatternVariable[] = "ICELAND_CAMERA_PATTERN";

static BOOL iceland_camera_pattern_forced(void)
{
    const char *value = getenv(kIcelandCameraPatternVariable);

    if (value && value[0] != '\0' && strcmp(value, "0") != 0)
        return YES;
    return access(kIcelandCameraPatternFile, F_OK) == 0;
}

@interface IcelandCameraProviderSource : NSObject <CMIOExtensionProviderSource>

@property (nonatomic, readonly, strong) CMIOExtensionProvider *provider;

- (instancetype)initWithClientQueue:(dispatch_queue_t)clientQueue patternMode:(BOOL)patternMode;

@end

@implementation IcelandCameraProviderSource {
    IcelandCameraDeviceSource *_deviceSource;
}

- (instancetype)initWithClientQueue:(dispatch_queue_t)clientQueue patternMode:(BOOL)patternMode
{
    self = [super init];
    if (self) {
        NSError *error = nil;

        _deviceSource = [[IcelandCameraDeviceSource alloc] initWithPatternMode:patternMode];
        if (!_deviceSource)
            return nil;

        _provider = [CMIOExtensionProvider providerWithSource:self clientQueue:clientQueue];
        if (![_provider addDevice:_deviceSource.device error:&error])
            iceland_camera_log("start: addDevice failed: %s", iceland_camera_error_text(error));
    }
    return self;
}

#pragma mark - CMIOExtensionProviderSource

- (NSSet<CMIOExtensionProperty> *)availableProperties
{
    return [NSSet setWithObjects:CMIOExtensionPropertyProviderManufacturer, CMIOExtensionPropertyProviderName, nil];
}

- (CMIOExtensionProviderProperties *)providerPropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties
                                                              error:(NSError **)outError
{
    CMIOExtensionProviderProperties *answer = [CMIOExtensionProviderProperties providerPropertiesWithDictionary:@{}];

    (void)outError;
    if ([properties containsObject:CMIOExtensionPropertyProviderManufacturer])
        answer.manufacturer = @"iceland";
    if ([properties containsObject:CMIOExtensionPropertyProviderName])
        answer.name = @"Iceland Camera Provider";
    return answer;
}

- (BOOL)setProviderProperties:(CMIOExtensionProviderProperties *)providerProperties error:(NSError **)outError
{
    // The manufacturer and the provider's name are ours, not a client's.
    (void)providerProperties;
    (void)outError;
    return YES;
}

- (BOOL)connectClient:(CMIOExtensionClient *)client error:(NSError **)outError
{
    (void)client;
    (void)outError;
    return YES;
}

- (void)disconnectClient:(CMIOExtensionClient *)client
{
    (void)client;
}

@end

int main(void)
{
    @autoreleasepool {
        BOOL pattern;

        if (iceland_raw10_selftest() == 0)
            iceland_camera_log("raw10 converter self-test ok");
        else
            iceland_camera_log("raw10 converter self-test FAILED");

        pattern = iceland_camera_pattern_forced();
        if (!pattern && !iceland_camera_kernel_present())
            pattern = YES;
        iceland_camera_log("start: provider up (mode %s)", pattern ? "pattern" : "kernel");
        iceland_camera_note(ICELAND_CAMERA_NOTE_PROVIDER_UP, pattern ? 1 : 0, 0);

        // The clients' queue: the CMIOExtension calls the provider and the stream sources on it, one at a time.
        dispatch_queue_t clients = dispatch_queue_create("org.iceland.camera.clients", DISPATCH_QUEUE_SERIAL);
        IcelandCameraProviderSource *source = [[IcelandCameraProviderSource alloc] initWithClientQueue:clients
                                                                                           patternMode:pattern];
        if (!source) {
            iceland_camera_log("start: the device or its stream could not be made");
            return 1;
        }
        if (![CMIOExtensionProvider startServiceWithProvider:source.provider]) {
            iceland_camera_log("start: startServiceWithProvider failed");
            return 1;
        }
        CFRunLoopRun();
    }
    return 0;
}
