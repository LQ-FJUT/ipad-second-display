# iPad Second Display

[简体中文](README.md) | English

## Overview

iPad Second Display is a self-use Windows second-screen tool. It creates and captures a virtual display on Windows, sends H.264 video to an iPad running OpenDisplay in the foreground, and returns touch, scroll, and Apple Pencil input to Windows.

The current release is **v0.2.0**. It is a hardened Windows sender based on OpenDisplay and supports USB and Wi-Fi transport, Bonjour/mDNS discovery, stable device IDs, device priorities, and ordered failover.

## Before you start

- Windows 10 or Windows 11 x64.
- OpenDisplay installed and open in the foreground on the iPad while streaming.
- Parsec Virtual Display Driver installed on Windows. This third-party driver is not bundled with the project installer.
- A trusted home or private LAN for Wi-Fi use. OpenDisplay protocol v3 does not provide encryption or authentication.

## Install and connect

1. Follow the driver safety and verification instructions in the [Chinese technical README](README.md) before installing Parsec VDD.
2. Open [Releases](https://github.com/LQ-FJUT/ipad-second-display/releases), download `iPad互联-Setup-x64.exe`, and install it.
3. Open OpenDisplay on the iPad.
4. Start iPad Second Display, choose a discovered device, and select **Connect**. A private-network address can also be entered in advanced settings.
5. To switch devices, select another target. The app ends the old session before starting a new single-target session.
6. Closing the window can leave the app in the tray; disconnect or exit from the tray menu when finished.

The installer is unsigned. Verify the SHA-256 value on the Release page before running it.

## How it works

- **Virtual display:** creates a headless Windows display through Parsec VDD.
- **Capture and encode:** captures the virtual monitor and encodes H.264 video with Media Foundation.
- **Transport:** sends frames to the unmodified OpenDisplay receiver over USB or trusted Wi-Fi.
- **Input:** maps touch and scrolling to Windows input and injects Apple Pencil data as pen input where supported.
- **Recovery:** reconnects after transient failures and can fail over between known devices in priority order.

## Known limits

- USB changes the transport path but does not remove the requirement to keep OpenDisplay in the foreground.
- The project does not control native iPadOS apps and does not support audio, clipboard sharing, or H.265.
- Multiple iPads with different native resolutions are constrained by Parsec VDD's single custom-resolution behavior.
- A stable device ID provides continuity, not security authentication.
- This is a self-use preview, not a broadly compatible commercial display-driver product.

## Build from source

You need the Visual Studio or Build Tools C++ desktop workload, a Windows SDK, and CMake 3.25 or newer.

```powershell
.\scripts\build.ps1
```

Create the installer:

```powershell
.\scripts\package.ps1 -Version 0.2.0
```

## Release status

This repository contains the final source for v0.2.0, while the installer is distributed through GitHub Releases. Local builds, automated tests, and simulated receivers do not replace acceptance testing with a real iPad, multiple devices, DHCP address changes, long-running streams, and a clean Windows installation.

## License and attribution

This project is based on the OpenDisplay Windows sender and remains compatible with the unmodified [OpenDisplay](https://github.com/peetzweg/opendisplay) iPad receiver. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for license and attribution details.
