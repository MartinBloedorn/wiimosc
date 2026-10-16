---
tags:
  - projects
status: ongoing
---
# Overview 

This page collects information and notes on using old Wiimotes (controllers, nunchucks, balance board) for A/V interactions.

## Libraries & utilities

### General

 - https://wiibrew.org/wiki/Wiimote/Library: *Homebrew wiki for the Wii*.
 - https://github.com/wiiuse/wiiuse: **GOAT**. Currently in use; see notes below.
 - https://github.com/grandelli/WiiC: C++ wrapper for `wiiuse`, untested.

#### wiiuse library

 - Provides an all-encompassing example.
 - Requires the Wiimotes to be already paired under Windows, then works out of the box.
 - Didn't get it to work under Linux, thought it theoretically should.
 - API docs: [link](https://wiiuse.net/?nav=api)

*Building it in Windows:*
- Builds with cmake(-gui) + Visual Studio 2022 out of the box, however,
- ... add the [BUILD_SHARED_LIBS](https://cmake.org/cmake/help/latest/variable/BUILD_SHARED_LIBS.html#variable:BUILD_SHARED_LIBS) definition to the CMake generation, to ensure a DLL is built (instead of a static lib), otherwise linking in Visual Studio will fail (or at least, it will when using a project generated with [ProJucer](JUCE.md)).

### Balance board / scale

Standalone Python scripts:
- https://github.com/namacha/wiiboard/blob/master/wiiboard.py: depends on outdated `pybluez` (unavailable for Python3). Couldn't get it to work (even after trying to convert it to PyQt's bluetooth stack).

C# apps...
- https://github.com/lshachar/Wiibalancewalker/releases
- https://github.com/schellingb/WiiBalanceScale
- ... both based on: https://github.com/BrianPeek/WiimoteLib

Others:
- https://github.com/aelveborn/Wii-Scale: web UI for scale functionality (untested)

## General Info

### Bluetooth pairing

[From the wiki](https://wiibrew.org/wiki/Wiimote#Bluetooth_Pairing):
> If connecting by holding down the 1+2 buttons, the PIN is the bluetooth address of the wiimote backwards, if connecting by pressing the "sync" button on the back of the wiimote, then the PIN is the bluetooth address of the host backwards.

Thus,
```
for an address "00:1E:35:3B:7E:6D":
char pin[6];
pin[0] = 0x6D; pin[1] = 0x7E; pin[2] = 0x3B; 
pin[3] = 0x35; pin[4] = 0x1E; pin[5] = 0x00;
```
You can use this utility to convert a MAC address into a PIN code string for pairing e.g. in Windows: https://mkwpaul.github.io/wiimotePinConverter/

>[!attention]
>For pairing in Windows, the generated PIN code needs to be a valid UTF-8 string - so a 0x00 byte in the MAC address screws things up. You can try to change the MAC with [this](https://macaddresschanger.com/) or [this](https://github.com/thxomas/bdaddr/tree/main), but they seem iffy and don't work for all adapters.

>[!note]
>My [ASUS Bluetooth adapter](https://www.amazon.de/dp/B08B9WQFTL):
>
>	MAC: BC:FC:E7:21:9F:54
>	PIN: T!çü¼

~~**Note**: pairing under [macOS doesn't seem to work](https://www.amazon.de/dp/B08B9WQFTL), for some reason.~~
#### Pairing under macOS

1. Get [WiimotePair](https://github.com/dolphin-emu/WiimotePair/releases/tag/1.0)
2. Run `sudo pkill bluetoothd`  (optional but recommended)
3. Press SYNC on the remote
4. Run *WiimotePair*

Utilities (optional):
- Install `blueutil` via `brew`
- Run `blueutil --inquiry` after pressing SYNC on the remote to query the remote's MAC without pairing

References: 
- [reddit](https://www.reddit.com/r/DolphinEmulator/comments/1dwkq3m/i_was_able_to_pair_wiimotes_to_macos_1431_with/), [reddit](https://www.reddit.com/r/macgaming/comments/19aiswa/any_new_way_to_connect_the_nintendo_wiimote_to/)
- [WiimotePair bug report](https://github.com/dolphin-emu/WiimotePair/issues/5)
- [YouTube: Wiimote + VDMX](https://www.youtube.com/watch?v=Hpz5QCL56EU)

