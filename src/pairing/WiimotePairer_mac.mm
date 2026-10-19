#include "WiimotePairer.h"

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

/*! \brief Required by IOBluetoothDeviceInquiry; the results are read from the inquiry itself. */
@interface WiimoteInquiryDelegate : NSObject <IOBluetoothDeviceInquiryDelegate>
@end

@implementation WiimoteInquiryDelegate
@end

// Private IOBluetooth API, as used by Dolphin's WiimotePair (https://github.com/dolphin-emu/WiimotePair). The public
// replyPINCode:PINCode: passes the PIN through an NSString, which mangles address bytes like 0x00 or >= 0x80 (as
// BluetoothAuthenticateDevice() does on Windows), and without setUserDefinedPincode: the delegate isn't asked for a
// PIN at all. The methods are looked up at runtime, so a macOS without them fails with a message.
@interface IOBluetoothDevicePair (WiimoscPrivate)
- (void)setUserDefinedPincode:(BOOL)isEnabled;
- (NSUInteger)currentPairingType;
@end

@interface IOBluetoothDevice (WiimoscPrivate)
- (id)classicPeer;
@end

@protocol WiimoscBluetoothCoordinator
+ (id)sharedInstance;
- (void)pairPeer:(id)peer forType:(NSUInteger)type withKey:(NSNumber *)key;
@end

/*! \brief Answers the legacy PIN request of a pairing attempt with the raw address bytes. */
@interface WiimotePairingDelegate : NSObject <IOBluetoothDevicePairDelegate> {
@public
    BluetoothPINCode pin;
}
@property(atomic) BOOL isFinished;
@property(atomic) IOReturn result;
@end

@implementation WiimotePairingDelegate
- (void)devicePairingPINCodeRequest:(id)sender
{
    IOBluetoothDevicePair *pairing = sender;
    IOBluetoothDevice *device = pairing.device;
    Class coordinator = NSClassFromString(@"IOBluetoothCoreBluetoothCoordinator");
    if (coordinator && [coordinator respondsToSelector:@selector(sharedInstance)]
        && [device respondsToSelector:@selector(classicPeer)]
        && [pairing respondsToSelector:@selector(currentPairingType)]) {
        // The key holds the PIN's bytes in memory order (the rest is zero).
        uint64_t key = 0;
        std::memcpy(&key, pin.data, sizeof(key));
        id<WiimoscBluetoothCoordinator> instance = [(Class<WiimoscBluetoothCoordinator>)coordinator sharedInstance];
        [instance pairPeer:[device classicPeer] forType:[pairing currentPairingType] withKey:@(key)];
    } else {
        std::fprintf(stderr, "  private pairing API missing, the PIN may arrive mangled\n");
        [sender replyPINCode:6 PINCode:&pin];
    }
}

- (void)devicePairingUserConfirmationRequest:(id)sender numericValue:(BluetoothNumericValue)numericValue
{
    // Not used by Nintendo devices (legacy PIN pairing only)
    std::fprintf(stderr, "  unexpected confirmation request, rejecting\n");
    [sender replyUserConfirmation:NO];
}

- (void)devicePairingFinished:(id)sender error:(IOReturn)error
{
    self.result = error;
    self.isFinished = YES;
}
@end

/*! \brief Notes the completion of a remote name request. */
@interface WiimoteNameDelegate : NSObject
@property(atomic) BOOL isFinished;
@end

@implementation WiimoteNameDelegate
- (void)remoteNameRequestComplete:(IOBluetoothDevice *)device status:(IOReturn)status
{
    self.isFinished = YES;
}
@end

namespace {

const NSTimeInterval sPairingTimeout = 20.0;
const NSTimeInterval sConnectTimeout = 5.0;
const NSTimeInterval sUnpairDelay = 1.0;
const NSTimeInterval sNameTimeout = 5.0;

/*! \brief Waits until \a isDone, or for \a timeout (IOBluetooth's delegate calls arrive on the main thread). */
bool waitUntil(const std::function<bool()> &isDone, NSTimeInterval timeout)
{
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + timeout;
    while (!isDone()) {
        if (CFAbsoluteTimeGetCurrent() >= deadline)
            return false;
        WiimotePairer::processEvents(20);
    }
    return true;
}

std::string formatAddress(const BluetoothDeviceAddress &address)
{
    const UInt8 *bytes = address.data;
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return buf;
}

/*! \brief The PIN: an address in reverse byte order (BluetoothDeviceAddress holds it in display order). */
BluetoothPINCode pinFromAddress(const BluetoothDeviceAddress &address)
{
    BluetoothPINCode pin = {};
    for (int i = 0; i < 6; ++i)
        pin.data[i] = address.data[5 - i];
    return pin;
}

bool hasNintendoName(IOBluetoothDevice *device)
{
    return [device.name hasPrefix:@"Nintendo RVL-"];
}

/*! \brief Checks the name, or the class of device while the name is unknown.

    The inquiry reports devices whose name macOS doesn't know (e.g. after unpairing) with their address as the name,
    and doesn't update it. A name request fixes that. If it fails, the class of device decides: peripheral, with
    minor class joystick (Wiimote, Balance Board) or gamepad (Wiimote Plus).
 */
bool isNintendoDevice(IOBluetoothDevice *device)
{
    if (hasNintendoName(device))
        return true;
    if (device.deviceClassMajor != kBluetoothDeviceClassMajorPeripheral)
        return false;
    const BluetoothDeviceClassMinor minor = device.deviceClassMinor;
    if (minor != kBluetoothDeviceClassMinorPeripheral2Joystick && minor != kBluetoothDeviceClassMinorPeripheral2Gamepad)
        return false;

    WiimoteNameDelegate *delegate = [[WiimoteNameDelegate alloc] init];
    if ([device remoteNameRequest:delegate] == kIOReturnSuccess)
        waitUntil([delegate] { return delegate.isFinished; }, sNameTimeout);
    if (hasNintendoName(device))
        return true;
    // A real name that isn't Nintendo's: some other gamepad
    NSString *address = [device.addressString stringByReplacingOccurrencesOfString:@"-" withString:@":"];
    return device.name.length == 0 || [device.name caseInsensitiveCompare:address] == NSOrderedSame;
}

/*! \brief Removes the pairing of \a device.

    IOBluetooth has no public API for this; -remove is what the Bluetooth settings use (as does blueutil).
 */
bool unpair(IOBluetoothDevice *device)
{
    const SEL remove = NSSelectorFromString(@"remove");
    if (![device respondsToSelector:remove])
        return false;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    [device performSelector:remove];
#pragma clang diagnostic pop
    return !device.isPaired;
}

bool pair(IOBluetoothDevice *device, const BluetoothDeviceAddress &keyAddress)
{
    WiimotePairingDelegate *delegate = [[WiimotePairingDelegate alloc] init];
    delegate->pin = pinFromAddress(keyAddress);
    IOBluetoothDevicePair *pairing = [IOBluetoothDevicePair pairWithDevice:device];
    pairing.delegate = delegate;
    if ([pairing respondsToSelector:@selector(setUserDefinedPincode:)])
        [pairing setUserDefinedPincode:YES];
    const IOReturn result = [pairing start];
    if (result != kIOReturnSuccess) {
        std::fprintf(stderr, "  starting to pair failed (error 0x%x)\n", result);
        return false;
    }
    if (!waitUntil([delegate] { return delegate.isFinished; }, sPairingTimeout)) {
        [pairing stop];
        std::fprintf(stderr, "  pairing timed out\n");
        return false;
    }
    pairing.delegate = nil;
    if (delegate.result != kIOReturnSuccess) {
        std::fprintf(stderr, "  pairing failed (error 0x%x: %s)\n", delegate.result, mach_error_string(delegate.result));
        return false;
    }
    return true;
}

} // namespace

WiimotePairer::WiimotePairer() = default;

WiimotePairer::~WiimotePairer() = default;

bool WiimotePairer::open()
{
    @autoreleasepool {
        IOBluetoothHostController *controller = [IOBluetoothHostController defaultController];
        if (!controller || controller.powerState != kBluetoothHCIPowerStateON)
            return false;
        BluetoothDeviceAddress address = {};
        if (IOBluetoothNSStringToDeviceAddress(controller.addressAsString, &address) != kIOReturnSuccess)
            return false;
        std::memcpy(mHostAddress, address.data, sizeof(mHostAddress));
        return true;
    }
}

bool WiimotePairer::disconnect(const std::string &address)
{
    @autoreleasepool {
        BluetoothDeviceAddress bluetoothAddress = {};
        NSString *text = [[NSString stringWithUTF8String:address.c_str()] stringByReplacingOccurrencesOfString:@":"
                                                                                                    withString:@"-"];
        if (IOBluetoothNSStringToDeviceAddress(text, &bluetoothAddress) != kIOReturnSuccess)
            return false;
        IOBluetoothDevice *device = [IOBluetoothDevice deviceWithAddress:&bluetoothAddress];
        return device && [device closeConnection] == kIOReturnSuccess;
    }
}

void WiimotePairer::processEvents(int milliseconds)
{
    if (![NSThread isMainThread]) {
        [NSThread sleepForTimeInterval:milliseconds / 1000.0];
        return;
    }
    // Returns early after handling a source, so keep running until the time is up.
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + milliseconds / 1000.0;
    for (CFTimeInterval left = milliseconds / 1000.0; left > 0; left = deadline - CFAbsoluteTimeGetCurrent()) {
        @autoreleasepool {
            if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, left, true) == kCFRunLoopRunFinished)
                [NSThread sleepForTimeInterval:left]; // no sources yet
        }
    }
}

std::string WiimotePairer::hostAddress() const
{
    BluetoothDeviceAddress address = {};
    std::memcpy(address.data, mHostAddress, sizeof(mHostAddress));
    return formatAddress(address);
}

int WiimotePairer::removeStaleDevices()
{
    // macOS only remembers devices it paired with (or that the user added), and those don't block an inquiry.
    // Paired remotes that are in pairing mode again get re-paired in scanAndPair().
    return 0;
}

std::vector<WiimotePairer::Device> WiimotePairer::scanAndPair(Method method, int inquiryUnits)
{
    std::vector<Device> paired;
    @autoreleasepool {
        // An inquiry only finds discoverable devices: remotes in SYNC or 1+2 mode. Paired remotes that reconnect
        // with a button press are accepted by macOS on its own.
        // The completion call comes late or not at all (seen on macOS 14), so the inquiry is stopped after its
        // length. Devices are found with their names.
        WiimoteInquiryDelegate *delegate = [[WiimoteInquiryDelegate alloc] init];
        IOBluetoothDeviceInquiry *inquiry = [IOBluetoothDeviceInquiry inquiryWithDelegate:delegate];
        const NSTimeInterval length = std::max(1.0, inquiryUnits * 1.28);
        inquiry.inquiryLength = static_cast<uint8_t>(std::ceil(length));
        inquiry.updateNewDeviceNames = YES;
        inquiry.searchType = kIOBluetoothDeviceSearchClassic;
        if ([inquiry start] != kIOReturnSuccess) {
            std::fprintf(stderr, "starting the inquiry failed\n");
            waitUntil([] { return false; }, length); // don't spin
            return paired;
        }
        waitUntil([] { return false; }, length);
        [inquiry stop];
        inquiry.delegate = nil;

        for (IOBluetoothDevice *device in inquiry.foundDevices) {
            if (device.isConnected || !isNintendoDevice(device))
                continue;

            const Device found{hasNintendoName(device) ? device.name.UTF8String : "Nintendo device (name unknown)",
                               formatAddress(*device.getAddress)};
            if (device.isPaired) {
                // It's discoverable, so it's in pairing mode and wants a fresh pairing.
                std::printf("found paired %s [%s] in pairing mode, re-pairing\n", found.name.c_str(),
                            found.address.c_str());
                if (!unpair(device)) {
                    std::fprintf(stderr, "  removing old pairing failed\n");
                    continue;
                }
                waitUntil([] { return false; }, sUnpairDelay); // pairing right away fails (error 0x2)
            } else {
                std::printf("found %s [%s]\n", found.name.c_str(), found.address.c_str());
            }

            if (method != Method::None) {
                BluetoothDeviceAddress hostAddress = {};
                std::memcpy(hostAddress.data, mHostAddress, sizeof(mHostAddress));
                if (!pair(device, method == Method::SyncButton ? hostAddress : *device.getAddress))
                    continue;
            }

            // macOS normally sets up the HID connection after pairing; open the baseband connection if it hasn't.
            if (!device.isConnected) {
                const IOReturn result = [device openConnection];
                if (result != kIOReturnSuccess) {
                    std::fprintf(stderr, "  connecting failed (error 0x%x)\n", result);
                    continue;
                }
                waitUntil([device] { return device.isConnected; }, sConnectTimeout);
            }
            paired.push_back(found);
        }
    }
    return paired;
}
