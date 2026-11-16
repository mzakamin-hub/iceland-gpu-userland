// Ours (MIT). See iceland-camera-log.h.
#import "iceland-camera-log.h"

#import <os/log.h>

#include <stdarg.h>
#include <stdio.h>

static os_log_t iceland_camera_log_handle(void)
{
    static os_log_t handle;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        handle = os_log_create("org.iceland.camera", "extension");
    });
    return handle;
}

void iceland_camera_log(const char *format, ...)
{
    char text[512];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);

    os_log(iceland_camera_log_handle(), "%{public}s", text);
    fprintf(stderr, "CMIO %s\n", text);
    fflush(stderr);
}

const char *iceland_camera_text(NSString *text)
{
    return text ? text.UTF8String : "?";
}

const char *iceland_camera_error_text(NSError *error)
{
    static _Thread_local char text[512];

    if (!error)
        return "no error";
    snprintf(text, sizeof(text), "%s %ld: %s", iceland_camera_text(error.domain), (long)error.code,
             iceland_camera_text(error.localizedDescription));
    return text;
}
