// Ours (MIT). One line, twice: os_log into subsystem org.iceland.camera, and a plain line on stderr with the "CMIO "
// prefix. The prefix is the proof a boot log is grepped for: "CMIO start: provider up (mode kernel|pattern)",
// "CMIO client pid N authorized", "CMIO stream start", "CMIO frames served N (kernel seq S)" and "CMIO stream stop".
#ifndef ICELAND_CAMERA_LOG_H
#define ICELAND_CAMERA_LOG_H

#import <Foundation/Foundation.h>

void iceland_camera_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Never NULL: "?" for a nil string, "domain code: description" for an error, "no error" for a nil error. Valid until the
// next call on the same thread.
const char *iceland_camera_text(NSString *text);
const char *iceland_camera_error_text(NSError *error);

#endif
