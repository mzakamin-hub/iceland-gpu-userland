// Ours (MIT). The skeleton of iceland's camera for CoreMediaIO, as a CMIOExtension provider (the form macOS loads
// today; the older DAL plug-ins are no longer loaded into apps): a process that registers a provider with no device
// yet and serves it. It builds with the public macOS SDK only (CoreMediaIO/CMIOExtension.h). The camera work adds a
// CMIOExtensionDevice with a CMIOExtensionStream to the provider and hands it sample buffers.
//
// The service name the provider is reached by is Info.plist's CMIOExtensionMachServiceName. How the process is started
// on the tablet (the system's extension machinery or a launchd job of ours) is the camera work's decision; nothing
// here depends on it.
#import <CoreMediaIO/CMIOExtension.h>
#import <Foundation/Foundation.h>

@interface IcelandCameraProviderSource : NSObject <CMIOExtensionProviderSource>
@property(nonatomic, readonly, strong) CMIOExtensionProvider *provider;
- (instancetype)initWithClientQueue:(dispatch_queue_t)clientQueue;
@end

@implementation IcelandCameraProviderSource

- (instancetype)initWithClientQueue:(dispatch_queue_t)clientQueue
{
    self = [super init];
    if (self) {
        _provider = [CMIOExtensionProvider providerWithSource:self clientQueue:clientQueue];
        // Devices go here: [_provider addDevice:device error:&error].
    }
    return self;
}

- (BOOL)connectClient:(CMIOExtensionClient *)client error:(NSError *__autoreleasing *)outError
{
    return YES;
}

- (void)disconnectClient:(CMIOExtensionClient *)client
{
}

- (NSSet<CMIOExtensionProperty> *)availableProperties
{
    return [NSSet setWithObject:CMIOExtensionPropertyProviderManufacturer];
}

- (CMIOExtensionProviderProperties *)providerPropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties
                                                               error:(NSError *__autoreleasing *)outError
{
    CMIOExtensionProviderProperties *answer = [CMIOExtensionProviderProperties providerPropertiesWithDictionary:@{}];
    if ([properties containsObject:CMIOExtensionPropertyProviderManufacturer]) {
        answer.manufacturer = @"iceland";
    }
    return answer;
}

- (BOOL)setProviderProperties:(CMIOExtensionProviderProperties *)providerProperties error:(NSError *__autoreleasing *)outError
{
    return YES;
}

@end

int main(void)
{
    @autoreleasepool {
        IcelandCameraProviderSource *source = [[IcelandCameraProviderSource alloc] initWithClientQueue:nil];
        [CMIOExtensionProvider startServiceWithProvider:source.provider];
        CFRunLoopRun();
    }
    return 0;
}
