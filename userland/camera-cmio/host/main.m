// Ours (MIT). IcelandCameraHost: asks the system to activate the CoreMediaIO extension org.iceland.camera.extension,
// which the app carries at Contents/Library/SystemExtensions/org.iceland.camera.extension.systemextension. The system asks
// the user once (requestNeedsUserApproval); every callback is logged on stderr with the "CMIO host: " prefix and, when
// the in-kernel IcelandCamera service is there, as a kernel log line (selector 4, kind "host activation"), so a boot
// log carries the request's whole life. The app has no UI (LSUIElement) and exits 0 when the request is finished, 1
// when it failed or has not finished after 120 s.
//
// An optional second argument is a URL that is opened at the moment the request needs the user's approval: a boot in
// which a person has to click something then has a line in the log saying that the page to click in was opened, and
// whether /usr/bin/open took it.
#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#import <SystemExtensions/SystemExtensions.h>

#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../iceland-camera-kernel.h"

// The environment posix_spawn hands to /usr/bin/open: this app's own, so the page opens in the session's browser.
extern char **environ;

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

// The URL the second argument named, or NULL: opened when the request needs the user's approval.
static const char *iceland_host_approval_url;

// Written by the delegate's callbacks only, which the request delivers on the queue it was made with (the main queue), so
// the run loop below reads them on the same thread.
static int iceland_host_status;
static int iceland_host_finished;

// /usr/bin/open, with no shell in between (the URL is one argument, whatever it holds), and its exit code in the log.
static void iceland_host_open(const char *url)
{
    char *arguments[3];
    pid_t child = 0;
    int status = 0;
    int failed;

    arguments[0] = (char *)"/usr/bin/open";
    arguments[1] = (char *)url;
    arguments[2] = NULL;
    failed = posix_spawn(&child, "/usr/bin/open", NULL, NULL, arguments, environ);
    if (failed != 0) {
        iceland_host_log("opened %s: posix_spawn %d", url, failed);
        return;
    }
    if (waitpid(child, &status, 0) < 0) {
        iceland_host_log("opened %s: waitpid failed", url);
        return;
    }
    if (WIFEXITED(status))
        iceland_host_log("opened %s: exit %d", url, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        iceland_host_log("opened %s: signal %d", url, WTERMSIG(status));
    else
        iceland_host_log("opened %s: status %d", url, status);
}

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
    if (iceland_host_approval_url)
        iceland_host_open(iceland_host_approval_url);
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
        // extension in System Settings needs minutes). The second argument, when it is there, is a URL to open at the
        // moment the system asks for that approval ("IcelandCameraHost [patience seconds] [URL to open on approval]").
        long patience = argc > 1 ? strtol(argv[1], NULL, 10) : 120;

        if (patience < 10 || patience > 7200)
            patience = 120;
        iceland_host_approval_url = argc > 2 && argv[2][0] != '\0' ? argv[2] : NULL;
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
