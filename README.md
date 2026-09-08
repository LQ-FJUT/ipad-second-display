# iPad互联

> 简明安装与使用说明见 [项目介绍与使用指南](项目介绍与使用指南.md)。

本工程是在 `martinhoess/opendisplay-win` 提交 `06af3a9` 基础上进行的
Windows 自用加固版本，继续兼容未经修改的 OpenDisplay iPad 接收端。
中文安装与安全说明见 [`docs/使用说明.md`](docs/使用说明.md)，驱动共存和
精确回滚要求见 [`docs/驱动安全与回滚.md`](docs/驱动安全与回滚.md)。

与上游原型相比，本分支支持 USB/Wi-Fi、稳定设备 ID + Bonjour 动态地址、
按设备优先级单目标故障转移和异步快捷切换；它记忆最后成功通道但不让地址
覆盖设备优先级，默认拒绝公用网络，补齐 OpenDisplay v3 生命周期，并禁用按
全局数字索引删除显示器的危险维护入口。正式发行物名称为 `iPad互联.exe`。

## Upstream technical documentation

Use an iPad as a wireless second monitor for **Windows**, driving the
**unmodified** [OpenDisplay](https://github.com/peetzweg/opendisplay) receiver
app on the iPad. OpenDisplay ships a macOS sender only; this project
reimplements that sender side for Windows and speaks the iPad's exact wire
protocol, so the iOS app needs no changes.

> **Status: self-use preview.** The pinned upstream has an end-to-end Windows
> sender, and this fork adds protocol-v3 lifecycle handling, configurable
> quality, a trusted-LAN gate, safer display teardown and a current-user
> installer. Version 0.2.0 adds stable-device discovery with TTL/goodbye,
> ordered failover, hello.id validation, atomic configuration and non-blocking
> device switching on top of the 0.1.2 daily-use panel. It remains a
> self-use preview and must not be treated as a broadly compatible commercial
> display driver.

## How it works

The iPad app listens on TCP port 9000; the Windows sender connects to it and:

1. **Virtual monitor** — creates a headless display sized to the iPad's panel
   via the [parsec-vdd](https://github.com/nomi-san/parsec-vdd) driver, so
   Windows sees a real second monitor you can drag windows onto.
2. **Capture** — grabs that monitor with DXGI Desktop Duplication and
   composites the mouse cursor into each frame (Desktop Duplication delivers the
   cursor out-of-band; color, masked-color and monochrome shapes are handled).
3. **Encode** — H.264 via Media Foundation (hardware NVENC/QuickSync/AMF when
   available), emitting an Annex-B byte stream normalized to the exact framing
   the iPad decoder expects (4-byte start codes, SPS/PPS on every keyframe).
4. **Stream** — one length-prefixed message per frame over the single TCP
   connection, only re-encoding when the screen or cursor actually changes (an
   idle desktop drops to ~1 keepalive frame/second).
5. **Input** — the iPad's touch and scroll events come back on the same
   connection and are injected as mouse input with `SendInput`, mapped onto the
   virtual monitor's rectangle. Apple Pencil is injected separately, as a real
   Windows pen pointer (`InjectSyntheticPointerInput`) carrying pressure, tilt
   and hover, so pressure-aware apps see a pen rather than a mouse.

It reconnects after transient link loss or receiver sleep, and rebuilds the
pipeline if the iPad rotates (the panel dimensions change). An explicit
receiver `closing` message ends that manually started session.

### Deliberately out of scope

H.265, audio, clipboard sharing and control of native iPadOS apps. H.264 is
required — the iPad receiver is hardcoded to it. USB is supported through the
Apple Mobile Device Service/usbmux and still requires OpenDisplay in the
foreground. Several iPads
of the same native panel size can run together; different panel sizes are
serialized because Parsec VDD exposes one shared custom resolution.

## Install

Needs Windows 10/11 (x64) and, on the iPad, the
[OpenDisplay](https://github.com/peetzweg/opendisplay) receiver app.
The upstream currently distributes the iOS/iPadOS 16+ receiver through its
[public TestFlight](https://testflight.apple.com/join/3NYaY11c), with Xcode
self-signing as the fallback.

### 1. Validate and install the parsec-vdd driver

This is a **separate third-party driver** and is not bundled with the release —
without it there is no virtual monitor to capture, and the app will refuse to
connect. On this machine, first run the read-only gate below and continue only
when its newest JSON report says `"OverallPass": true`. The gate also binds the
exact standalone installer to the Microsoft WinGet manifest hash and to its
Parsec Authenticode identity:

```powershell
$driverInstaller = (Resolve-Path '.\.tools\drivers\parsec-vdd-0.45.0.0.exe').Path
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\safety\Test-PreInstallSafety.ps1 -NetworkInterfaceAlias 'WLAN 2' -DriverInstallerPath $driverInstaller
```

The gate deliberately fails while that route is `Public`; the installer never
changes the network category for you. Once the gate passes, install the driver
once as administrator:

1. Obtain `parsec-vdd-0.45.0.0.exe` from the official
   [`builds.parsec.app`](https://builds.parsec.app/vdd/parsec-vdd-0.45.0.0.exe)
   URL. The approved SHA-256 is
   `E23332448FDAF5AA017CB308DB5EF6855FAC526A7DED05D80C039404126D5362`;
   the gate above verifies it again. The binary is proprietary and is not part
   of this project's GPL/BSD source distribution.
2. After accepting the vendor terms, run that exact installer interactively as
   administrator. The first real-machine acceptance deliberately does not use
   `/S`: Parsec's current VDD instructions document the interactive flow,
   while the standalone silent switch is only inferred from its NSIS/WinGet
   metadata.
3. Do **not** automatically run `vddinstall.bat`, `nefconw`, or another helper.
   Parsec's current official instructions do not require that extra step. If
   Device Manager does not show a present/OK **Parsec Virtual Display Adapter**
   with hardware ID `Root\Parsec\VDA`, stop and collect evidence instead of
   attempting a second installation path.
4. Immediately run `New-PostInstallReceipt.ps1` with the exact passing baseline
   report. MouseLink must not start until that audit confirms the device, INF,
   version, provider, Windows driver signature, installer identity, Private
   network and protected GameViewer baseline.

The VDD is officially intended for Parsec sessions. Using its community IOCTL
interface from MouseLink is an experimental, vendor-unsupported integration;
keep the generated receipt until final acceptance and rollback are complete.

> To remove it later, first use the receipt-validated dry-run and exact rollback
> documented in [`docs/驱动安全与回滚.md`](docs/驱动安全与回滚.md). Only then use the
> vendor's *Settings → Apps* entry for remaining application files; never bulk
> remove display devices or driver packages by name.

### 2. Install iPad互联

Build this pinned source tree, or use this project's own
`dist\iPad互联-Setup-x64.exe`. Do not repackage the older upstream v0.1.0
binary: it predates the protocol, lifecycle and safety fixes in this branch.

**SmartScreen will warn you.** The binary is not code-signed yet, so Windows
shows *"Windows protected your PC"* on first launch. Click **More info** →
**Run anyway**. Signing is planned via the SignPath Foundation's free OSS
programme (see the [ROADMAP](ROADMAP.md)) — until that comes through, every
release is unsigned. If you would rather not trust an unsigned binary from a
stranger on the internet: [build it from source](#build-from-source), it is two
commands.

### 3. First start

Double-click the exe. The main control panel opens immediately while the sender
continues to live in the tray after the window is closed. Keep OpenDisplay in
the iPad foreground, then choose a discovered LAN receiver or use a cable and
click **连接**. Manual IP entry remains under **高级设置**.

The device selector shows priority, the stable OpenDisplay install ID, mDNS
online state, current address and the latest failure. **连接/切换** stops the old
target asynchronously and starts exactly one selected Wi-Fi target after the
old session has exited. **记住为默认** changes device preference; the most recent
USB/Wi-Fi success changes only transport preference. **上移** changes automatic
device order. Startup gives each Wi-Fi device a bounded attempt, then advances
to the next configured device; once streaming, a transient drop reconnects the
same device instead of jumping elsewhere.

The tray menu names the iPads by itself: the receiver advertises over Bonjour
(`_opensidecar._tcp`), and the app publishes the name from its settings there.
That name is used, or — when it is still the generic "iPad" iOS hands out — the
device's host name (`iPad-Pro.local` → *iPad-Pro*). None of this arrives over
the wire: the `hello` only carries `"device":"iPad"` and a UUID.

You can still put a name after the address (`192.168.1.42 Mini`); a name set
here wins over the advertised one.

Configuration is versioned at `%APPDATA%\MouseLink\config.json`. Old `ip` and
string-array files migrate in memory, while the next save uses structured
device records and a sibling temporary file + atomic replace + `.bak` recovery.
MAC addresses are never used as TCP endpoints; `macHint`, when present, is
diagnostic-only. Hostnames and numeric IPv4/IPv6 are resolved through the same
trusted-private-network gate used for the actual socket endpoint.

**One panel size at a time.** parsec-vdd puts a single custom resolution on all
of its virtual monitors, so iPads with *different* panels cannot both run
native — whichever sender sets its size last drags the other monitors along and
their picture arrives letterboxed. So iPads of the same size stream together,
and a different one waits: its menu entry reads *waiting for 2732x2048* until
the iPad holding the display disconnects, then it starts on its own within a
few seconds. Disconnect the one you don't need and the waiting one takes over.
Lifting this needs a driver that keeps a stable monitor identity — see the
[ROADMAP](ROADMAP.md).

The first time you connect a **new** iPad, you get **one UAC prompt** — the app
registers that panel's resolution as a parsec-vdd custom mode, which needs a
single `HKLM` write. Accept it; it happens once per iPad model, never again.
Details in [Admin rights](#admin-rights) below.

**Do not batch-clean monitor nodes.** This computer already has unrelated
virtual-display software. The hardened build refuses both the upstream
`--cleanup-monitors` and `--remove-display` commands. Recovery must use the
instance-ID/INF receipt workflow in `tools\safety` so it cannot touch
GameViewer or a different Parsec client.

### Admin rights

The app **runs un-elevated**. The *only* thing that needs admin is registering
an iPad's native resolution as a parsec-vdd custom mode — one `HKLM` write, done
once per panel size. The first time you connect a new iPad, the app self-elevates
a one-off (a single UAC prompt) to register both orientations, then keeps running
un-elevated; after that it never prompts again. You can also do it by hand:

```
build\Release\iPad互联.exe --register-resolution <width> <height>
```

Running the whole app as administrator is **optional** (tray menu → *Run as
administrator*) and only matters if you want touch to control *elevated* windows
on the iPad screen — Windows blocks input from an un-elevated process into
higher-integrity windows (UIPI).

## Build from source

Needs Visual Studio Build Tools (Desktop C++ workload) or Visual Studio with
the Windows 10/11 SDK, and CMake ≥ 3.25. The parsec-vdd driver from
[Install](#1-install-the-parsec-vdd-driver) is required to *run* it, not to
build it.

```
cmake --preset vs2022-x64
cmake --build --preset release
```

The result is `build\Release\iPad互联.exe`, statically linked against
the CRT — the same single-file binary the releases ship.

## Run

The commands below spell out the build-tree path; if you downloaded the release
binary, substitute wherever you put `iPad互联.exe`.

Launched **without arguments** it opens the unified control panel and also runs
as a **tray app**. The panel exposes connection/disconnection, retry or
USB/Wi-Fi switching, status guidance, display shortcuts, receiver discovery,
quality presets and redacted diagnostics export. Advanced settings retain
manual addresses (`address [name]`), port, fps and bitrate. The tray menu mirrors
the per-device connection states. Config is saved to
`%APPDATA%\MouseLink\config.json`; a log goes to `log.txt` next to it,
with each line tagged by the iPad it belongs to.

```
build\Release\iPad互联.exe
```

Launched **with an IP** it runs **headless** (no UI), handy for a single-target
test. It logs to its own `log-<pid>.txt`. Do not run several product processes
in parallel: panel-size coordination is process-local. Configure several
same-size iPads in one tray process instead.

```
build\Release\iPad互联.exe <ipad-ip>
```

Either way the iPad receiver app must already be running (listening on port
9000); the sender reconnects automatically on drops.

The upstream `--cleanup-monitors` and `--remove-display` arguments are retained
only to print a refusal message. Use the receipt-validated safety tools for any
driver recovery.

## Testing without an iPad

`tools/mock_receiver.py` (Python 3, stdlib only) stands in for the iPad:
listens on :9000, sends `hello`, logs control messages, and structurally
validates every video payload against the Annex-B framing rules (4-byte start
codes only, SPS/PPS immediately before each keyframe) — handy for exercising
the transport, encoder byte format, reconnect and rotation paths locally.

```
python tools/mock_receiver.py --width 2388 --height 1668
# then, in another terminal:
build\Release\iPad互联.exe --capture-existing "\\.\DISPLAY1" 127.0.0.1
```

`--rotate-after N` sends a second `hello` with the panel dimensions swapped
after N frames, i.e. what the iPad sends when it is turned — the path where the
sender tears its virtual monitor down and has to claim a new one.

Two iPads at once can be faked over the loopback range: bind one mock to
`127.0.0.1` and another to `127.0.0.2` (`--host`), then start a sender against
each. Both senders must end up on **different** `\\.\DISPLAYn` monitors — the
`claimed virtual monitor` line in the log says which.

## License

GPL-3.0-or-later — a derivative of OpenDisplay (GPL-3.0). See [LICENSE](LICENSE).
`third_party/parsec-vdd/parsec-vdd.h` originates from
[nomi-san/parsec-vdd](https://github.com/nomi-san/parsec-vdd) and includes local
compatibility fixes for the current v0.45 IOCTL/overlapped-I/O contract. The
header's BSD-2-Clause terms are reproduced in `THIRD_PARTY_NOTICES.txt` and are
GPL-3.0-compatible. The separately installed Parsec driver binary is not bundled
and has its own distribution terms.
