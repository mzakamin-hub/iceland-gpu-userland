// Ours (MIT). iceland-bt-scan: what Bluetooth's scanner hears, so a boot with nobody at the tablet can show that
// macOS's stack brings the controller up and that CoreBluetooth's central manager sees other people's devices: wait
// for poweredOn, scan with no service filter, one line per new peripheral. Foundation / CoreBluetooth only, ARC.
//
//   iceland-bt-scan [-w seconds] [-s seconds]     (defaults: wait up to 300 s for poweredOn, then scan 40 s)
//
// Every line starts "iceland-bt-scan: ". A line about someone else's device carries no identity: the first 8 hex
// characters of its identifier, whether it has a name (yes/no), how long its manufacturer data is and how many
// service UUIDs it advertises. The last line is "iceland-bt-scan: DONE ..." or "... FAIL ...". Exit 0: the scan
// ran and stopped; 1: usage; 2: Bluetooth was not powered on in time (or unsupported); 3: not authorized.
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// The delegate runs on g_queue, the tool's main thread polls; g_lock guards everything below it.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableSet<NSString *> *g_seen;         // the peripheral identifiers already reported
static CBCentralManager *g_manager;
static CBManagerState g_state = CBManagerStateUnknown;
static bool g_scanning;
static unsigned g_devices, g_advertisements;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("iceland-bt-scan: ", stdout);
    vfprintf(stdout, format, arguments);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(arguments);
}

// CBManagerState's six values and CBManagerAuthorization's four, in the SDK's order; a value not in the table is "?".
static const char *state_name(CBManagerState state)
{
    static const char *const names[] = {"unknown", "resetting", "unsupported", "unauthorized", "poweredOff", "poweredOn"};
    return state >= 0 && state <= CBManagerStatePoweredOn ? names[state] : "?";
}

static const char *authorization_name(CBManagerAuthorization authorization)
{
    static const char *const names[] = {"notDetermined", "restricted", "denied", "allowedAlways"};
    return authorization >= 0 && authorization <= CBManagerAuthorizationAllowedAlways ? names[authorization] : "?";
}

@interface IcelandBTScan : NSObject <CBCentralManagerDelegate>
@end

@implementation IcelandBTScan

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    CBManagerState state = central.state;
    bool start;
    pthread_mutex_lock(&g_lock);
    g_state = state;
    say("state %d %s", (int)state, state_name(state));
    start = state == CBManagerStatePoweredOn && !g_scanning;
    if (start)
        g_scanning = true;
    pthread_mutex_unlock(&g_lock);
    if (start)
        [central scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
    NSString *identifier = peripheral.identifier.UUIDString;
    char short_id[9] = "?";                      // the identifier's first 8 hex characters, and nothing more of it
    (void)central;
    if (identifier.length < 8 || ![[identifier substringToIndex:8] getCString:short_id maxLength:sizeof(short_id)
                                                                   encoding:NSUTF8StringEncoding])
        short_id[0] = '\0';
    pthread_mutex_lock(&g_lock);
    g_advertisements++;
    if (identifier.length && ![g_seen containsObject:identifier]) {
        [g_seen addObject:identifier];
        unsigned devices = ++g_devices;
        // The name itself stays out of the log, and so do the manufacturer data bytes: only their presence is said.
        say("device %u id %s rssi %d name %s conn %d services %lu mfr %lu", devices, short_id, RSSI.intValue,
            peripheral.name.length ? "yes" : "no", peripheral.state == CBPeripheralStateConnected ? 1 : 0,
            (unsigned long)[advertisementData[CBAdvertisementDataServiceUUIDsKey] count],
            (unsigned long)[advertisementData[CBAdvertisementDataManufacturerDataKey] length]);
    }
    pthread_mutex_unlock(&g_lock);
}

@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        int wait_seconds = 300, scan_seconds = 40, option, tenth;
        IcelandBTScan *delegate;
        CBManagerState state = CBManagerStateUnknown;

        // Line buffering, so the kernel's log copy sees one write() per line, as the recorder's does.
        setvbuf(stdout, NULL, _IOLBF, 0);
        while ((option = getopt(argc, argv, "w:s:")) != -1) {
            if (option == 'w') wait_seconds = atoi(optarg);
            else if (option == 's') scan_seconds = atoi(optarg);
            else { say("FAIL usage: iceland-bt-scan [-w wait seconds] [-s scan seconds]"); return 1; }
        }
        if (wait_seconds < 1 || wait_seconds > 3600) wait_seconds = 300;
        if (scan_seconds < 1 || scan_seconds > 600) scan_seconds = 40;
        say("start pid %d", (int)getpid());
        say("authorization %d %s", (int)CBManager.authorization, authorization_name(CBManager.authorization));
        g_seen = [NSMutableSet set];
        // The manager holds its delegate weakly: this local reference keeps it alive to the end of main.
        delegate = [[IcelandBTScan alloc] init];
        g_manager = [[CBCentralManager alloc] initWithDelegate:delegate
                                                         queue:dispatch_queue_create("org.iceland.bt-scan",
                                                                                     DISPATCH_QUEUE_SERIAL)
                                                       options:@{CBCentralManagerOptionShowPowerAlertKey: @NO}];

        // Wait for poweredOn, a tenth of a second at a time as the recorder polls. unauthorized and unsupported
        // cannot become poweredOn by waiting, so they answer before WAIT_S is spent.
        for (tenth = 0; tenth < wait_seconds * 10; tenth++) {
            pthread_mutex_lock(&g_lock);
            state = g_state;
            pthread_mutex_unlock(&g_lock);
            if (state == CBManagerStatePoweredOn || state == CBManagerStateUnauthorized ||
                state == CBManagerStateUnsupported)
                break;
            usleep(100000);
        }
        if (state != CBManagerStatePoweredOn) {
            if (state == CBManagerStateUnauthorized) {
                say("FAIL state unauthorized after %d s: not authorized to use Bluetooth (authorization %d %s)",
                    tenth / 10, (int)CBManager.authorization, authorization_name(CBManager.authorization));
                return 3;
            }
            say("FAIL state %s after %d s", state_name(state), tenth / 10);
            return 2;
        }

        // The scan: the same tenth-of-a-second loop, with a count every 10 s.
        for (tenth = 1; tenth <= scan_seconds * 10; tenth++) {
            usleep(100000);
            if (tenth % 100 == 0) {
                pthread_mutex_lock(&g_lock);
                say("scanning, %u devices so far, %u advertisements", g_devices, g_advertisements);
                pthread_mutex_unlock(&g_lock);
            }
        }
        pthread_mutex_lock(&g_lock);
        unsigned devices = g_devices, advertisements = g_advertisements;
        state = g_state;
        pthread_mutex_unlock(&g_lock);
        [g_manager stopScan];
        say("DONE %u devices, %u advertisements, state %s", devices, advertisements, state_name(state));
        return 0;
    }
}
