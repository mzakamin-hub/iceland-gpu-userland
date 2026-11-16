// Ours (MIT). IcelandCameraHost: asks the system to activate the CoreMediaIO extension org.iceland.camera.extension,
// which the app carries at Contents/Library/SystemExtensions/org.iceland.camera.extension.systemextension. The system asks
// the user once (requestNeedsUserApproval); every callback is logged on stderr with the "CMIO host: " prefix and, when
// the in-kernel IcelandCamera service is there, as a kernel log line (selector 4, kind "host activation"), so a boot
// log carries the request's whole life. The app has no UI (LSUIElement) and exits 0 when the request is finished, 1
// when it failed or has not finished after 120 s.
#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#import <SystemExtensions/SystemExtensions.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../iceland-camera-kernel.h"

static const char *iceland_host_text(NSString *text);

static void iceland_host_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void iceland_host_log(const char *format, ...)
{
    char text[512];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    fprintf(stderr, "CMIO host: %s\n", text);
    fflush(stderr);
}

static const char *iceland_host_text(NSString *text)
{
    return text ? text.UTF8String : "?";
}

static NSString *const kIcelandCameraExtension = @"org.iceland.camera.extension";

// Written by the delegate's callbacks only, which the request delivers on the queue it was made with (the main queue), so
// the run loop below reads them on the same thread.
static int iceland_host_status;
static int iceland_host_finished;

static void iceland_host_finish(int status)
{
    iceland_host_status = status;
    iceland_host_finished = 1;
    CFRunLoopStop(CFRunLoopGetMain());
}

@interface IcelandCameraHostDelegate : NSObject <OSSystemExtensionRequestDelegate>
@end

@implementation IcelandCameraHostDelegate

- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                  actionForReplacingExtension:(OSSystemExtensionProperties *)existing
                                withExtension:(OSSystemExtensionProperties *)extension
{
    (void)request;
    iceland_host_log("replacing extension %s (%s) with %s (%s) -> replace", iceland_host_text(existing.bundleIdentifier),
                     iceland_host_text(existing.bundleShortVersion), iceland_host_text(extension.bundleIdentifier),
                     iceland_host_text(extension.bundleShortVersion));
    return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    iceland_host_log("needs user approval for %s", iceland_host_text(request.identifier));
    iceland_camera_note(ICELAND_CAMERA_NOTE_HOST, 1, 0);
}

- (void)request:(OSSystemExtensionRequest *)request didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    iceland_camera_note(ICELAND_CAMERA_NOTE_HOST, 2, (uint64_t)result);
    if (result == OSSystemExtensionRequestWillCompleteAfterReboot) {
        iceland_host_log("finished for %s: it completes after a reboot", iceland_host_text(request.identifier));
        iceland_host_finish(0);
    } else if (result == OSSystemExtensionRequestCompleted) {
        iceland_host_log("finished for %s: completed", iceland_host_text(request.identifier));
        iceland_host_finish(0);
    } else {
        iceland_host_log("finished for %s: result %ld", iceland_host_text(request.identifier), (long)result);
        iceland_host_finish(1);
    }
}

- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error
{
    // The code is OSSystemExtensionErrorDomain's (1 unknown, 2 missing entitlement, 3 unsupported parent bundle
    // location, 4 extension not found, 8 code signature invalid, 9 validation failed, ...): the number is the answer
    // to "does the system take an ad hoc signed extension".
    iceland_host_log("failed for %s: %s %ld: %s", iceland_host_text(request.identifier),
                     iceland_host_text(error.domain), (long)error.code, iceland_host_text(error.localizedDescription));
    iceland_camera_note(ICELAND_CAMERA_NOTE_HOST, 3, (uint64_t)error.code);
    iceland_host_finish(1);
}

@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        // The wait for the final callback, in seconds: 120, or the first argument (a run in which a person approves the
        // extension in System Settings needs minutes).
        long patience = argc > 1 ? strtol(argv[1], NULL, 10) : 120;

        if (patience < 10 || patience > 7200)
            patience = 120;
        // The main queue is a serial queue, and the run loop below drains it: the callbacks arrive on this thread.
        dispatch_queue_t queue = dispatch_get_main_queue();
        OSSystemExtensionRequest *request = [OSSystemExtensionRequest activationRequestForExtension:kIcelandCameraExtension
                                                                                              queue:queue];
        IcelandCameraHostDelegate *delegate = [[IcelandCameraHostDelegate alloc] init];

        if (!request) {
            iceland_host_log("no request for %s", iceland_host_text(kIcelandCameraExtension));
            return 1;
        }
        request.delegate = delegate;
        iceland_host_log("requesting activation of %s", iceland_host_text(request.identifier));
        iceland_camera_note(ICELAND_CAMERA_NOTE_HOST, 0, 0);
        [OSSystemExtensionManager.sharedManager submitRequest:request];

        // The run loop's own timeout: a request the system never answers must not hold a boot or a CI job.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)patience * NSEC_PER_SEC), queue, ^{
            if (!iceland_host_finished) {
                iceland_host_log("no final callback within %ld s", patience);
                iceland_camera_note(ICELAND_CAMERA_NOTE_HOST, 3, 0);
                iceland_host_finish(1);
            }
        });
        CFRunLoopRun();
        return iceland_host_status;
    }
}
