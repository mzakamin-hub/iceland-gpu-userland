// Ours (MIT). See iceland-camera-kernel.h.
#include "iceland-camera-kernel.h"

#include <mach/mach.h>

#include <pthread.h>
#include <string.h>

int iceland_camera_kernel_present(void)
{
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching(ICELAND_CAMERA_KERNEL_SERVICE));
    if (service == IO_OBJECT_NULL)
        return 0;
    IOObjectRelease(service);
    return 1;
}

int iceland_camera_kernel_open(iceland_camera_kernel *kernel)
{
    io_service_t service;
    kern_return_t result;

    memset(kernel, 0, sizeof(*kernel));
    kernel->connection = IO_OBJECT_NULL;
    service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching(ICELAND_CAMERA_KERNEL_SERVICE));
    if (service == IO_OBJECT_NULL)
        return 1;
    result = IOServiceOpen(service, mach_task_self(), 0, &kernel->connection);
    IOObjectRelease(service);
    if (result != KERN_SUCCESS) {
        kernel->connection = IO_OBJECT_NULL;
        return 2;
    }
    return 0;
}

void iceland_camera_kernel_close(iceland_camera_kernel *kernel)
{
    if (kernel->connection != IO_OBJECT_NULL) {
        IOServiceClose(kernel->connection);
        kernel->connection = IO_OBJECT_NULL;
    }
}

int iceland_camera_kernel_start(iceland_camera_kernel *kernel)
{
    uint64_t output = 1;
    uint32_t output_count = 1;
    kern_return_t result = IOConnectCallScalarMethod(kernel->connection, ICELAND_CAMERA_KERNEL_SELECTOR_START, NULL, 0, &output,
                                                     &output_count);
    if (result != KERN_SUCCESS)
        return 1;
    return output == 0 ? 0 : 2;
}

void iceland_camera_kernel_stop(iceland_camera_kernel *kernel)
{
    uint64_t output = 0;
    uint32_t output_count = 0;

    IOConnectCallScalarMethod(kernel->connection, ICELAND_CAMERA_KERNEL_SELECTOR_STOP, NULL, 0, &output, &output_count);
}

int iceland_camera_kernel_get_info(iceland_camera_kernel *kernel, iceland_camera_kernel_info *info)
{
    uint64_t output[8] = {0};
    uint32_t output_count = 8;
    kern_return_t result = IOConnectCallScalarMethod(kernel->connection, ICELAND_CAMERA_KERNEL_SELECTOR_INFO, NULL, 0, output,
                                                     &output_count);
    if (result != KERN_SUCCESS || output_count < 8)
        return 1;
    info->state = output[0];
    info->width = output[1];
    info->height = output[2];
    info->stride = output[3];
    info->bytes_per_buffer = output[4];
    info->buffer_count = output[5];
    info->frames = output[6];
    info->clients = output[7];
    return 0;
}

int iceland_camera_kernel_map(iceland_camera_kernel *kernel, uint64_t bytes_per_buffer, uint64_t buffer_count)
{
    mach_vm_address_t address = 0;
    mach_vm_size_t size = bytes_per_buffer * buffer_count;
    kern_return_t result;

    if (bytes_per_buffer == 0 || buffer_count == 0)
        return 1;
    result = IOConnectMapMemory64(kernel->connection, 0, mach_task_self(), &address, &size,
                                  kIOMapAnywhere | kIOMapReadOnly);
    if (result != KERN_SUCCESS)
        return 2;
    kernel->buffers_address = address;
    kernel->buffers_size = size;
    kernel->bytes_per_buffer = bytes_per_buffer;
    return 0;
}

void iceland_camera_kernel_unmap(iceland_camera_kernel *kernel)
{
    if (kernel->buffers_address != 0) {
        IOConnectUnmapMemory64(kernel->connection, 0, mach_task_self(), kernel->buffers_address);
        kernel->buffers_address = 0;
        kernel->buffers_size = 0;
        kernel->bytes_per_buffer = 0;
    }
}

const uint8_t *iceland_camera_kernel_buffer(const iceland_camera_kernel *kernel, uint64_t index)
{
    uint64_t offset = index * kernel->bytes_per_buffer;

    if (kernel->buffers_address == 0 || kernel->bytes_per_buffer == 0)
        return NULL;
    if (offset + kernel->bytes_per_buffer > kernel->buffers_size)
        return NULL;
    return (const uint8_t *)(uintptr_t)(kernel->buffers_address + offset);
}

int iceland_camera_kernel_wait(iceland_camera_kernel *kernel, uint32_t timeout_ms, iceland_camera_kernel_frame *frame)
{
    uint64_t input[2] = {kernel->last_sequence, timeout_ms};
    uint64_t output[8] = {0};
    uint32_t output_count = 8;
    kern_return_t result = IOConnectCallScalarMethod(kernel->connection, ICELAND_CAMERA_KERNEL_SELECTOR_WAIT, input, 2, output,
                                                     &output_count);

    if (result == kIOReturnTimeout)
        return ICELAND_CAMERA_KERNEL_TIMEOUT;
    if (result == kIOReturnNotReady)
        return ICELAND_CAMERA_KERNEL_NOT_READY;
    if (result != KERN_SUCCESS || output_count < 8)
        return ICELAND_CAMERA_KERNEL_FAILED;
    frame->sequence = output[0];
    frame->index = output[1];
    frame->width = output[2];
    frame->height = output[3];
    frame->stride = output[4];
    frame->gain = output[5];
    frame->exposure = output[6];
    frame->host_time = output[7];
    kernel->last_sequence = output[0];
    return ICELAND_CAMERA_KERNEL_FRAME;
}

int iceland_camera_kernel_torn(iceland_camera_kernel *kernel, const iceland_camera_kernel_frame *frame)
{
    iceland_camera_kernel_info info;

    if (iceland_camera_kernel_get_info(kernel, &info) != 0)
        return 1;
    return info.frames >= frame->sequence + 2;
}

void iceland_camera_note(uint64_t kind, uint64_t a, uint64_t b)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static iceland_camera_kernel note;
    static int tried;
    uint64_t input[3] = {kind, a, b};

    pthread_mutex_lock(&lock);
    if (!tried) {
        tried = 1;
        if (iceland_camera_kernel_open(&note) != 0)
            note.connection = IO_OBJECT_NULL;
    }
    if (note.connection != IO_OBJECT_NULL)
        IOConnectCallScalarMethod(note.connection, ICELAND_CAMERA_KERNEL_SELECTOR_NOTE, input, 3, NULL, NULL);
    pthread_mutex_unlock(&lock);
}
