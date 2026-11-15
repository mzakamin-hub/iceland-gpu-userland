// Ours (MIT). The skeleton of iceland's CoreAudio AudioServerPlugIn (a HAL plug-in, /Library/Audio/Plug-Ins/HAL):
// a driver object that coreaudiod can load and ask about itself, with no device yet. It builds with the public macOS
// SDK only (CoreAudio/AudioServerPlugIn.h). The audio work replaces the "no device" answers below: publish a device
// and its streams in the plug-in's object lists, answer their properties, and move samples in DoIOOperation.
//
// Object IDs: the plug-in itself is kAudioObjectPlugInObject (1); devices, streams and controls take 2 and up.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>
#include <stdbool.h>

static atomic_uint ice_references = 1;
static AudioServerPlugInHostRef ice_host;

static HRESULT ice_query_interface(void *driver, REFIID uuid, LPVOID *out);
static ULONG ice_add_ref(void *driver);
static ULONG ice_release(void *driver);
static OSStatus ice_initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host);
static OSStatus ice_create_device(AudioServerPlugInDriverRef driver, CFDictionaryRef description, const AudioServerPlugInClientInfo *client, AudioObjectID *out_device);
static OSStatus ice_destroy_device(AudioServerPlugInDriverRef driver, AudioObjectID device);
static OSStatus ice_add_device_client(AudioServerPlugInDriverRef driver, AudioObjectID device, const AudioServerPlugInClientInfo *client);
static OSStatus ice_remove_device_client(AudioServerPlugInDriverRef driver, AudioObjectID device, const AudioServerPlugInClientInfo *client);
static OSStatus ice_perform_configuration_change(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt64 action, void *info);
static OSStatus ice_abort_configuration_change(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt64 action, void *info);
static Boolean ice_has_property(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address);
static OSStatus ice_is_property_settable(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address, Boolean *out_settable);
static OSStatus ice_get_property_data_size(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 *out_size);
static OSStatus ice_get_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 size, UInt32 *out_size, void *out_data);
static OSStatus ice_set_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 size, const void *data);
static OSStatus ice_start_io(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client);
static OSStatus ice_stop_io(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client);
static OSStatus ice_get_zero_time_stamp(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client, Float64 *out_sample_time, UInt64 *out_host_time, UInt64 *out_seed);
static OSStatus ice_will_do_io_operation(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client, UInt32 operation, Boolean *out_will_do, Boolean *out_in_place);
static OSStatus ice_begin_io_operation(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle);
static OSStatus ice_do_io_operation(AudioServerPlugInDriverRef driver, AudioObjectID device, AudioObjectID stream, UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *main_buffer, void *secondary_buffer);
static OSStatus ice_end_io_operation(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle);

static AudioServerPlugInDriverInterface ice_interface = {
    NULL,
    ice_query_interface,
    ice_add_ref,
    ice_release,
    ice_initialize,
    ice_create_device,
    ice_destroy_device,
    ice_add_device_client,
    ice_remove_device_client,
    ice_perform_configuration_change,
    ice_abort_configuration_change,
    ice_has_property,
    ice_is_property_settable,
    ice_get_property_data_size,
    ice_get_property_data,
    ice_set_property_data,
    ice_start_io,
    ice_stop_io,
    ice_get_zero_time_stamp,
    ice_will_do_io_operation,
    ice_begin_io_operation,
    ice_do_io_operation,
    ice_end_io_operation,
};
static AudioServerPlugInDriverInterface *ice_interface_pointer = &ice_interface;
static AudioServerPlugInDriverRef ice_driver = &ice_interface_pointer;

// The factory Info.plist's CFPlugInFactories names. The one exported symbol.
__attribute__((visibility("default")))
void *IcelandAudio_Create(CFAllocatorRef allocator, CFUUIDRef requested_type)
{
    (void)allocator;
    if (!CFEqual(requested_type, kAudioServerPlugInTypeUUID))
        return NULL;
    return ice_driver;
}

static HRESULT ice_query_interface(void *driver, REFIID uuid, LPVOID *out)
{
    if (driver != ice_driver || !out)
        return kAudioHardwareBadObjectError;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&ice_references, 1);
        *out = ice_driver;
        result = S_OK;
    }
    CFRelease(requested);
    return result;
}

static ULONG ice_add_ref(void *driver)
{
    (void)driver;
    return atomic_fetch_add(&ice_references, 1) + 1;
}

static ULONG ice_release(void *driver)
{
    (void)driver;
    // The driver object is static: the count only mirrors what the host holds.
    unsigned now = atomic_load(&ice_references);
    while (now > 0 && !atomic_compare_exchange_weak(&ice_references, &now, now - 1)) {
    }
    return now > 0 ? now - 1 : 0;
}

static OSStatus ice_initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host)
{
    if (driver != ice_driver)
        return kAudioHardwareBadObjectError;
    ice_host = host;
    return kAudioHardwareNoError;
}

static OSStatus ice_create_device(AudioServerPlugInDriverRef driver, CFDictionaryRef description, const AudioServerPlugInClientInfo *client, AudioObjectID *out_device)
{
    (void)driver; (void)description; (void)client; (void)out_device;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus ice_destroy_device(AudioServerPlugInDriverRef driver, AudioObjectID device)
{
    (void)driver; (void)device;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus ice_add_device_client(AudioServerPlugInDriverRef driver, AudioObjectID device, const AudioServerPlugInClientInfo *client)
{
    (void)driver; (void)device; (void)client;
    return kAudioHardwareBadObjectError;
}

static OSStatus ice_remove_device_client(AudioServerPlugInDriverRef driver, AudioObjectID device, const AudioServerPlugInClientInfo *client)
{
    (void)driver; (void)device; (void)client;
    return kAudioHardwareBadObjectError;
}

static OSStatus ice_perform_configuration_change(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt64 action, void *info)
{
    (void)driver; (void)device; (void)action; (void)info;
    return kAudioHardwareBadObjectError;
}

static OSStatus ice_abort_configuration_change(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt64 action, void *info)
{
    (void)driver; (void)device; (void)action; (void)info;
    return kAudioHardwareBadObjectError;
}

// The plug-in object's own properties. A device, a stream or a control adds its own switch on `object`.
static Boolean ice_plugin_has(AudioObjectPropertySelector selector)
{
    switch (selector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:
    case kAudioPlugInPropertyTranslateUIDToDevice:
    case kAudioPlugInPropertyResourceBundle:
        return true;
    default:
        return false;
    }
}

static Boolean ice_has_property(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address)
{
    (void)client;
    return driver == ice_driver && address && object == kAudioObjectPlugInObject && ice_plugin_has(address->mSelector);
}

static OSStatus ice_is_property_settable(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address, Boolean *out_settable)
{
    if (!ice_has_property(driver, object, client, address))
        return kAudioHardwareUnknownPropertyError;
    *out_settable = false;
    return kAudioHardwareNoError;
}

static OSStatus ice_get_property_data_size(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 *out_size)
{
    (void)qualifier_size; (void)qualifier;
    if (!ice_has_property(driver, object, client, address))
        return kAudioHardwareUnknownPropertyError;
    switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
        *out_size = sizeof(AudioClassID);
        break;
    case kAudioObjectPropertyOwner:
    case kAudioPlugInPropertyTranslateUIDToDevice:
        *out_size = sizeof(AudioObjectID);
        break;
    case kAudioObjectPropertyManufacturer:
    case kAudioPlugInPropertyResourceBundle:
        *out_size = sizeof(CFStringRef);
        break;
    default:   // the two object lists: no device yet
        *out_size = 0;
        break;
    }
    return kAudioHardwareNoError;
}

static OSStatus ice_get_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 size, UInt32 *out_size, void *out_data)
{
    UInt32 needed = 0;
    OSStatus status = ice_get_property_data_size(driver, object, client, address, qualifier_size, qualifier, &needed);
    if (status != kAudioHardwareNoError)
        return status;
    if (size < needed)
        return kAudioHardwareBadPropertySizeError;
    switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
        *(AudioClassID *)out_data = kAudioObjectClassID;
        break;
    case kAudioObjectPropertyClass:
        *(AudioClassID *)out_data = kAudioPlugInClassID;
        break;
    case kAudioObjectPropertyOwner:
    case kAudioPlugInPropertyTranslateUIDToDevice:
        *(AudioObjectID *)out_data = kAudioObjectUnknown;
        break;
    case kAudioObjectPropertyManufacturer:
        *(CFStringRef *)out_data = CFSTR("iceland");
        break;
    case kAudioPlugInPropertyResourceBundle:
        *(CFStringRef *)out_data = CFSTR("");
        break;
    default:
        break;
    }
    *out_size = needed;
    return kAudioHardwareNoError;
}

static OSStatus ice_set_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 size, const void *data)
{
    (void)driver; (void)object; (void)client; (void)address; (void)qualifier_size; (void)qualifier; (void)size; (void)data;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus ice_start_io(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client)
{
    (void)driver; (void)device; (void)client;
    return kAudioHardwareBadObjectError;
}

static OSStatus ice_stop_io(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client)
{
    (void)driver; (void)device; (void)client;
    return kAudioHardwareBadObjectError;
}

static OSStatus ice_get_zero_time_stamp(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client, Float64 *out_sample_time, UInt64 *out_host_time, UInt64 *out_seed)
{
    (void)driver; (void)device; (void)client; (void)out_sample_time; (void)out_host_time; (void)out_seed;
    return kAudioHardwareBadObjectError;
}

static OSStatus ice_will_do_io_operation(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client, UInt32 operation, Boolean *out_will_do, Boolean *out_in_place)
{
    (void)driver; (void)device; (void)client; (void)operation;
    *out_will_do = false;
    *out_in_place = true;
    return kAudioHardwareNoError;
}

static OSStatus ice_begin_io_operation(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle)
{
    (void)driver; (void)device; (void)client; (void)operation; (void)frames; (void)cycle;
    return kAudioHardwareNoError;
}

static OSStatus ice_do_io_operation(AudioServerPlugInDriverRef driver, AudioObjectID device, AudioObjectID stream, UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *main_buffer, void *secondary_buffer)
{
    (void)driver; (void)device; (void)stream; (void)client; (void)operation; (void)frames; (void)cycle; (void)main_buffer; (void)secondary_buffer;
    return kAudioHardwareNoError;
}

static OSStatus ice_end_io_operation(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle)
{
    (void)driver; (void)device; (void)client; (void)operation; (void)frames; (void)cycle;
    return kAudioHardwareNoError;
}
