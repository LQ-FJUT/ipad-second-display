#include "app/SenderApp.h"

#include "app/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <winsock2.h>

#include "display/DesktopDuplication.h"
#include "display/DisplayCatalog.h"
#include "display/VirtualDisplay.h"
#include "encode/H264Encoder.h"
#include "input/InputInjector.h"
#include "net/Connection.h"
#include "net/NetworkSafety.h"
#include "net/Protocol.h"
#include "net/UsbMux.h"

namespace od {

namespace {

constexpr int kSendTimeoutMs = 500;      // backpressure: how long to wait for the socket before dropping a frame
constexpr int kReconnectDelayMs = 2000;
constexpr int kKeepaliveMs = 1000;       // max silence on a static screen; well under the iPad's ~5s watchdog
constexpr int kSenderPingMs = 2000;
constexpr int kDisconnectGraceMs = 15'000;
constexpr int kActiveTailMs = 300;       // keep feeding the encoder this long after the last change (drains its 1-frame hold)
constexpr int kWrongSizeGraceMs = 3000;  // how long the monitor may sit on a foreign size before we rebuild it
constexpr int kBlockedRetryMs = 5000;    // how often a waiting iPad checks whether the display is free again
constexpr uint32_t kVirtualDisplayHz = 60; // Parsec custom modes are registered at 60 Hz

// Only one panel size may be on the air at a time.
//
// parsec-vdd puts a single custom resolution on all of its virtual monitors:
// whichever sender sets its size last drags every other monitor along, and
// those iPads then show a letterboxed desktop at the wrong aspect. Rather than
// serve a wrong-shaped picture, iPads of the same panel size run together and
// a different one waits until the display is free again.
//
// Process-wide, because the tray drives every sender. A headless CLI sender
// started next to the tray is outside this and can still take the mode with
// it — that path is for testing.
struct PanelState {
    std::mutex mutex;
    uint32_t width = 0;
    uint32_t height = 0;
    int holders = 0;
};

PanelState& Panel()
{
    static PanelState state;
    return state;
}

// Takes a share of the display for w x h. Returns false when a different size
// holds it, and then reports that size in activeW/activeH.
bool AcquirePanel(uint32_t w, uint32_t h, uint32_t& activeW, uint32_t& activeH)
{
    PanelState& panel = Panel();
    std::lock_guard<std::mutex> lock(panel.mutex);
    if (panel.holders > 0 && (panel.width != w || panel.height != h)) {
        activeW = panel.width;
        activeH = panel.height;
        return false;
    }
    panel.width = w;
    panel.height = h;
    ++panel.holders;
    return true;
}

void ReleasePanel()
{
    PanelState& panel = Panel();
    std::lock_guard<std::mutex> lock(panel.mutex);
    if (--panel.holders <= 0) {
        panel.holders = 0;
        panel.width = 0;
        panel.height = 0;
    }
}

// Rotation: the panel is the same device, just turned. Only the sole holder
// may change the size under the claim — with another iPad attached the two
// would fight over the one custom resolution again.
bool RetunePanel(uint32_t w, uint32_t h)
{
    PanelState& panel = Panel();
    std::lock_guard<std::mutex> lock(panel.mutex);
    if (panel.holders > 1)
        return false;
    panel.width = w;
    panel.height = h;
    return true;
}

// Releases the claim when the connection ends, whichever way it ends.
struct PanelHolder {
    bool held = false;
    ~PanelHolder()
    {
        if (held)
            ReleasePanel();
    }
};

bool NormalizePanelSize(int32_t rawWidth, int32_t rawHeight, uint32_t& width, uint32_t& height)
{
    constexpr int32_t kMinAxis = 320;
    constexpr int32_t kMaxAxis = 8192;
    constexpr uint64_t kMaxPixels = 8192ull * 4096ull;
    if (rawWidth < kMinAxis || rawWidth > kMaxAxis || rawHeight < kMinAxis || rawHeight > kMaxAxis ||
        static_cast<uint64_t>(rawWidth) * static_cast<uint64_t>(rawHeight) > kMaxPixels)
        return false;

    width = static_cast<uint32_t>(rawWidth) & ~1u;
    height = static_cast<uint32_t>(rawHeight) & ~1u; // NV12 4:2:0 needs even dimensions
    return width >= 2 && height >= 2;
}

std::optional<double> JsonNumber(const std::string& json, const char* key)
{
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos || (pos = json.find(':', pos + needle.size())) == std::string::npos)
        return std::nullopt;
    const char* begin = json.c_str() + pos + 1;
    char* end = nullptr;
    double value = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(value))
        return std::nullopt;
    return value;
}

} // namespace

SenderApp::~SenderApp()
{
    Stop();
}

void SenderApp::Start(std::string ip, uint16_t port, StreamSettings settings,
                      std::string expectedDeviceId, std::string transport)
{
    if (running_.exchange(true))
        return; // already running

    // A receiver can end a session itself with `closing`. In that case the
    // previous worker has finished and cleared running_, but its std::thread
    // remains joinable until somebody joins it. Assigning a new thread over a
    // joinable one calls std::terminate(), so reap the completed worker before
    // allowing the tray to reconnect.
    if (worker_.joinable())
        worker_.join();

    stopRequested_ = false;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        networkDrops_ = 0;
        receiverRttMs_ = -1.0;
        receiverMbps_ = -1.0;
        actualFps_ = -1.0;
        receiverStalls_ = -1;
        captureMs_ = -1.0;
        encodeMs_ = -1.0;
        displayName_.clear();
        deviceId_ = expectedDeviceId;
        transport_ = transport;
        ++attempt_;
    }
    PublishStatus(ConnectionPhase::Connecting);
    worker_ = std::thread(
        [this, ip = std::move(ip), port, settings = std::move(settings),
         expectedDeviceId = std::move(expectedDeviceId), transport = std::move(transport)]() mutable {
            RunLoop(std::move(ip), port, std::move(settings), std::move(expectedDeviceId), std::move(transport));
            running_ = false;
        });
}

void SenderApp::RequestStop()
{
    stopRequested_ = true;

    // Unblock the worker if it's parked in Connect's socket / a blocking
    // ReadFrame: closing the socket makes those calls fail promptly.
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        if (activeConn_ != nullptr)
            activeConn_->Interrupt();
    }

}

void SenderApp::Stop()
{
    RequestStop();
    if (worker_.joinable())
        worker_.join();

    running_ = false;
    stopRequested_ = false;
    state_ = State::Idle;
    PublishStatus(ConnectionPhase::Idle);
}

void SenderApp::RunBlocking(std::string ip, uint16_t port, StreamSettings settings)
{
    const std::string transport = IsUsbMuxTarget(ip) ? "usb" : "wifi";
    running_ = true;
    RunLoop(std::move(ip), port, std::move(settings), {}, transport);
    running_ = false;
}

void SenderApp::InterruptibleSleep(int ms)
{
    // Poll the stop flag in short slices so Stop() doesn't wait a full delay.
    constexpr int slice = 100;
    for (int waited = 0; waited < ms && !stopRequested_; waited += slice)
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
}

std::optional<RECT> SenderApp::StreamMonitorRect() const
{
    std::lock_guard<std::mutex> lock(monitorMutex_);
    if (monitorRect_.right <= monitorRect_.left || monitorRect_.bottom <= monitorRect_.top)
        return std::nullopt;
    return monitorRect_;
}

void SenderApp::PublishMonitorRect(RECT rect)
{
    std::lock_guard<std::mutex> lock(monitorMutex_);
    monitorRect_ = rect;
}

void SenderApp::PublishStatus(ConnectionPhase phase, FailureReason failure, std::string detail, int retryInMs)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    phase_ = phase;
    failure_ = failure;
    failureDetail_ = std::move(detail);
    retryAt_ = retryInMs > 0 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(retryInMs)
                             : std::chrono::steady_clock::time_point{};
}

void SenderApp::PublishDisplayName(std::wstring name)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    displayName_ = std::move(name);
}

void SenderApp::PublishReceiverStats(const std::string& json)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (auto value = JsonNumber(json, "rtt"))
        receiverRttMs_ = *value;
    if (auto value = JsonNumber(json, "mbps"))
        receiverMbps_ = *value;
    if (auto value = JsonNumber(json, "fps"))
        actualFps_ = *value;
    else if (auto actual = JsonNumber(json, "actualFps"))
        actualFps_ = *actual;
    if (auto value = JsonNumber(json, "stalls"))
        receiverStalls_ = static_cast<int>(*value);
}

void SenderApp::PublishPipelineTimings(double captureMs, std::optional<double> encodeMs)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    // A light exponential average is enough for diagnostics and avoids a
    // per-frame history allocation in the hot path.
    captureMs_ = captureMs_ < 0.0 ? captureMs : captureMs_ * 0.9 + captureMs * 0.1;
    if (encodeMs)
        encodeMs_ = encodeMs_ < 0.0 ? *encodeMs : encodeMs_ * 0.9 + *encodeMs * 0.1;
}

ConnectionSnapshot SenderApp::Snapshot() const
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    ConnectionSnapshot snapshot;
    snapshot.phase = phase_;
    snapshot.failure = failure_;
    snapshot.detail = failureDetail_;
    snapshot.width = width_.load();
    snapshot.height = height_.load();
    snapshot.networkDrops = networkDrops_;
    snapshot.receiverRttMs = receiverRttMs_;
    snapshot.receiverMbps = receiverMbps_;
    snapshot.actualFps = actualFps_;
    snapshot.receiverStalls = receiverStalls_;
    snapshot.captureMs = captureMs_;
    snapshot.encodeMs = encodeMs_;
    snapshot.displayName = displayName_;
    snapshot.deviceId = deviceId_;
    snapshot.transport = transport_;
    snapshot.attempt = attempt_;
    if (retryAt_ != std::chrono::steady_clock::time_point{}) {
        snapshot.retryInMs = static_cast<int>(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                          retryAt_ - std::chrono::steady_clock::now())
                                                                          .count()));
    }
    return snapshot;
}

void SenderApp::RunLoop(std::string ip, uint16_t port, StreamSettings settings,
                        std::string expectedDeviceId, std::string transport)
{
    (void)transport;
    settings.fps = std::clamp(settings.fps, 30u, 120u);
    settings.bitrateBps = std::clamp(settings.bitrateBps, 5'000'000u, 100'000'000u);
    const bool managedDisplay = settings.existingDisplayName.empty();

    // One connection per iPad, machine-wide: a per-IP named mutex. A second
    // instance (CLI or tray) aimed at the same iPad backs off instead of
    // fighting over its single listening socket and the virtual display.
    // Different IPs get different names, so multiple iPads run fine.
    // Keep the upstream mutex namespace so an older opendisplay-win process
    // cannot overlap this rebranded build during an upgrade.
    std::wstring lockName = L"Global\\opendisplay-win-";
    const std::string& lockIdentity = expectedDeviceId.empty() ? ip : expectedDeviceId;
    for (char c : lockIdentity)
        lockName += (c == '.' || c == ':') ? L'_' : static_cast<wchar_t>(c);
    HANDLE ipLock = CreateMutexW(nullptr, FALSE, lockName.c_str());
    if (ipLock == nullptr) {
        Logf(ip, "cannot create the cross-process connection guard (error %lu); refusing to run unlocked\n",
             GetLastError());
        state_ = State::Idle;
        PublishStatus(ConnectionPhase::Failed, FailureReason::AnotherSender,
                      "无法创建跨进程连接保护，已拒绝不安全启动");
        return;
    }
    if (ipLock != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        Logf(ip, "another MouseLink sender is already connected to this iPad\n");
        CloseHandle(ipLock);
        state_ = State::Idle;
        PublishStatus(ConnectionPhase::Failed, FailureReason::AnotherSender,
                      "已有另一个 iPad互联进程连接这台设备");
        return;
    }

    // Declaration order matters for teardown: H264Encoder's ctor initializes
    // COM (MTA) + Media Foundation for this thread and its dtor uninitializes
    // them. Locals are destroyed in reverse, so the encoder is declared FIRST
    // (destroyed LAST) — otherwise DesktopDuplication's D3D11/DXGI COM objects
    // would be released after CoUninitialize(), an access violation on Stop().
    H264Encoder encoder;
    VirtualDisplay vdisp;
    DesktopDuplication dup;
    InputInjector input;
    std::mutex pipelineMutex;

    // Keeps this sender's monitor position separate from the other iPads' —
    // several senders share one HKCU key.
    vdisp.SetIdentity(ip);

    auto currentMonitorRect = [&]() {
        if (managedDisplay)
            return vdisp.MonitorRect();
        auto display = FindAttachedDisplay(settings.existingDisplayName);
        return display ? display->bounds : RECT{};
    };

    auto buildPipeline = [&](uint32_t width, uint32_t height) {
        // Caller holds pipelineMutex.
        if (managedDisplay && !vdisp.IsOpen() && !vdisp.Open()) {
            Logf(ip, "VirtualDisplay::Open failed (parsec-vdd driver missing/inaccessible?)\n");
            PublishStatus(ConnectionPhase::Failed, FailureReason::DriverUnavailable,
                          "未找到可用的 Parsec Virtual Display Driver", kReconnectDelayMs);
            return false;
        }
        // Release the capture BEFORE touching the monitor. On a rotation rebuild
        // EnsureResolution removes and re-adds the virtual display; doing that
        // while a DXGI duplication is still live on the old output tears the
        // monitor down under an active capture — which destabilizes DWM (the
        // Display Settings dialog crashes) and can crash us. Closing first means
        // no duplication ever references a monitor that's being replaced.
        dup.Close();
        std::wstring deviceName = settings.existingDisplayName;
        if (managedDisplay) {
            // Encoding cadence is configurable, but Parsec's registered custom
            // modes in this product are 60 Hz. Requesting the encode FPS as a
            // display refresh (for example 30/120) produces BADMODE because
            // registration currently records only the 60 Hz mode.
            if (!vdisp.EnsureResolution(width, height, kVirtualDisplayHz)) {
                Logf(ip, "VirtualDisplay::EnsureResolution failed (run as Administrator?)\n");
                PublishStatus(ConnectionPhase::Failed, FailureReason::ResolutionUnavailable,
                              "无法注册或应用 iPad 分辨率", kReconnectDelayMs);
                return false;
            }
            deviceName = vdisp.DeviceName();
        } else if (!FindAttachedDisplay(deviceName)) {
            Logf(ip, "existing display %ls is not attached\n", deviceName.c_str());
            PublishStatus(ConnectionPhase::Failed, FailureReason::CaptureUnavailable,
                          "测试用显示器当前未连接", kReconnectDelayMs);
            return false;
        }
        if (!dup.Open(deviceName)) {
            Logf(ip, "DesktopDuplication::Open failed\n");
            PublishStatus(ConnectionPhase::Failed, FailureReason::CaptureUnavailable,
                          "无法捕获虚拟显示器", kReconnectDelayMs);
            return false;
        }
        if (!encoder.Configure(dup.Width(), dup.Height(), settings.fps, settings.bitrateBps)) {
            Logf(ip, "encoder configure failed\n");
            PublishStatus(ConnectionPhase::Failed, FailureReason::EncoderUnavailable,
                          "H.264 编码器初始化失败", kReconnectDelayMs);
            return false;
        }
        input.SetMonitorRect(currentMonitorRect());
        PublishMonitorRect(currentMonitorRect());
        PublishDisplayName(deviceName);
        Logf(ip, "pipeline ready: %ux%u at %u fps / %.1f Mbps (%s display)\n", dup.Width(), dup.Height(),
             settings.fps, settings.bitrateBps / 1'000'000.0, managedDisplay ? "managed" : "existing");
        return true;
    };

    auto detachManagedDisplay = [&] {
        if (!managedDisplay || !vdisp.IsOpen())
            return;

        RECT removed = vdisp.MonitorRect();
        // The DXGI duplication must never outlive the output it references.
        // Rotation rebuilds already follow this order; normal disconnect,
        // receiver sleep and the 15-second grace cleanup must do the same.
        dup.Close();
        POINT cursor{};
        if (GetCursorPos(&cursor) && PtInRect(&removed, cursor)) {
            for (const DisplayInfo& display : EnumerateAttachedDisplays()) {
                if (display.primary) {
                    SetCursorPos((display.bounds.left + display.bounds.right) / 2,
                                 (display.bounds.top + display.bounds.bottom) / 2);
                    break;
                }
            }
        }
        vdisp.Close();
        PublishMonitorRect({});
        PublishDisplayName({});
    };

    // RAII: publish the current connection so Stop() can close it, and clear it
    // before the connection is destroyed (dtors run in reverse declaration
    // order, so declare this *after* the connection it points at).
    struct ActiveConn {
        SenderApp* self;
        ActiveConn(SenderApp* s, Connection* c) : self(s)
        {
            std::lock_guard<std::mutex> lock(s->connMutex_);
            s->activeConn_ = c;
        }
        ~ActiveConn()
        {
            std::lock_guard<std::mutex> lock(self->connMutex_);
            self->activeConn_ = nullptr;
        }
    };

    bool blockedLogged = false; // the wait message belongs in the log once, not every retry
    bool everStreamed = false;
    std::string lastNetworkBlockReason;
    std::optional<std::chrono::steady_clock::time_point> disconnectedSince;

    while (!stopRequested_) {
        if (disconnectedSince &&
            std::chrono::steady_clock::now() - *disconnectedSince >=
                std::chrono::milliseconds(kDisconnectGraceMs)) {
            Logf(ip, "receiver absent for %dms; removing this session's virtual display\n", kDisconnectGraceMs);
            detachManagedDisplay();
            disconnectedSince.reset();
        }

        // Re-check before every connection attempt. A laptop can roam from a
        // trusted hotspot to public Wi-Fi while this process stays running;
        // a one-time startup check would otherwise permit the reconnect.
        NetworkSafetyResult networkSafety = IsUsbMuxTarget(ip)
                                              ? NetworkSafetyResult{true, "direct USB cable", ip}
                                              : CheckTrustedLan(ip, settings.requirePrivateNetwork);
        if (!networkSafety.allowed) {
            state_ = State::UnsafeNetwork;
            PublishStatus(ConnectionPhase::UnsafeNetwork, FailureReason::NetworkUnsafe, networkSafety.reason,
                          kBlockedRetryMs);
            if (networkSafety.reason != lastNetworkBlockReason) {
                lastNetworkBlockReason = networkSafety.reason;
                Logf(ip, "connection blocked by trusted-LAN policy: %s\n", networkSafety.reason.c_str());
            }
            InterruptibleSleep(kBlockedRetryMs);
            continue;
        }
        if (!lastNetworkBlockReason.empty()) {
            Logf(ip, "network safety restored: %s\n", networkSafety.reason.c_str());
            lastNetworkBlockReason.clear();
        }
        // A blocked sender keeps checking back every few seconds; flipping it to
        // Connecting for each of those attempts would make the tray entry
        // alternate between "waiting for 2732x2048" and "connecting..." while
        // nothing about its situation changed.
        if (state_ != State::Blocked)
            state_ = State::Connecting;
        PublishStatus(everStreamed ? ConnectionPhase::Reconnecting : ConnectionPhase::Connecting);
        Logf(ip, "connecting to port %u ...\n", port);
        auto conn = Connection::Connect(networkSafety.resolvedTarget.empty() ? ip : networkSafety.resolvedTarget, port);
        if (!conn) {
            if (IsUsbMuxTarget(ip)) {
                Logf(ip, "USB connect failed: %s; retrying in %dms\n", UsbMuxLastError().c_str(), kReconnectDelayMs);
                PublishStatus(ConnectionPhase::Reconnecting, FailureReason::UsbUnavailable, UsbMuxLastError(),
                              kReconnectDelayMs);
            } else {
                Logf(ip, "connect failed, retrying in %dms\n", kReconnectDelayMs);
                PublishStatus(ConnectionPhase::Reconnecting, FailureReason::ReceiverUnavailable,
                              "无法连接 iPad；请在前台打开 OpenDisplay", kReconnectDelayMs);
            }
            InterruptibleSleep(kReconnectDelayMs);
            continue;
        }
        ActiveConn activeConn(this, &*conn);
        Logf(ip, "connected, waiting for hello...\n");
        PublishStatus(ConnectionPhase::WaitingHello, FailureReason::OpenDisplayUnavailable,
                      "已连接设备，正在等待 OpenDisplay 握手");

        HelloMsg hello;
        bool gotHello = false;
        while (!gotHello && !stopRequested_) {
            auto frame = conn->ReadFrame();
            if (!frame)
                break;
            if (!IsControlPayload(frame->data(), frame->size()))
                continue;
            auto msg = ParseControlMessage(frame->data(), frame->size());
            if (msg && msg->type == ControlType::Hello) {
                hello = msg->hello;
                gotHello = true;
            }
        }
        if (!gotHello) {
            PublishStatus(ConnectionPhase::Reconnecting, FailureReason::OpenDisplayUnavailable,
                          "未收到 OpenDisplay 握手；请让接收端保持前台", kReconnectDelayMs);
            continue;
        }
        if (!HelloMatchesExpectedDevice(hello, expectedDeviceId)) {
            Logf(ip, "refusing receiver identity mismatch (expected stable id, hello.id differs)\n");
            conn->Close();
            PublishStatus(ConnectionPhase::Failed, FailureReason::DeviceIdentityMismatch,
                          "当前地址返回了另一台设备，已拒绝连接");
            return;
        }
        {
            std::lock_guard<std::mutex> statusLock(statusMutex_);
            deviceId_ = hello.id;
        }

        // Version handshake (receiver protocol 3+): the iPad only sends
        // `pencil`/`proximity` to a peer that announced protocol >= 3 —
        // without this it silently degrades the Apple Pencil to plain `touch`
        // and pressure never arrives. Sent once per connection and only from
        // this thread: the reader thread must never write to the socket, or
        // its frames would interleave with the video the capture loop sends.
        std::string welcome = SerializeWelcome();
        bool welcomeSent =
            conn->SendFrame(reinterpret_cast<const uint8_t*>(welcome.data()), static_cast<uint32_t>(welcome.size()));
        Logf(ip, "welcome sent: %s\n", welcomeSent ? "yes" : "FAILED");
        if (!welcomeSent) {
            // A timed-out write can leave a partial framed message on this TCP
            // stream. It is permanently desynchronized; never continue with
            // video on it.
            conn->Close();
            PublishStatus(ConnectionPhase::Reconnecting, FailureReason::ProtocolError,
                          "OpenDisplay 欢迎消息发送失败", kReconnectDelayMs);
            InterruptibleSleep(kReconnectDelayMs);
            continue;
        }

        uint32_t width = 0;
        uint32_t height = 0;
        if (!NormalizePanelSize(hello.pixelsWide, hello.pixelsHigh, width, height)) {
            Logf(ip, "refusing invalid panel dimensions %dx%d\n", hello.pixelsWide, hello.pixelsHigh);
            conn->Close();
            PublishStatus(ConnectionPhase::Failed, FailureReason::InvalidPanel,
                          "iPad 返回了无效的屏幕尺寸", kReconnectDelayMs);
            InterruptibleSleep(kReconnectDelayMs);
            continue;
        }
        Logf(ip, "hello: pv=%d %dx%d -> %ux%u\n", hello.pv, hello.pixelsWide, hello.pixelsHigh, width, height);
        PublishStatus(ConnectionPhase::PreparingDisplay);

        // Only iPads of the panel size that is already on the air may join (see
        // AcquirePanel). A different one hangs up and keeps checking back, so
        // it starts by itself once the other iPad disconnects.
        PanelHolder panel;
        uint32_t activeWidth = 0, activeHeight = 0;
        if (managedDisplay)
            panel.held = AcquirePanel(width, height, activeWidth, activeHeight);
        if (managedDisplay && !panel.held) {
            blockedByWidth_ = activeWidth;
            blockedByHeight_ = activeHeight;
            state_ = State::Blocked;
            PublishStatus(ConnectionPhase::Blocked, FailureReason::ResolutionUnavailable,
                          "另一台不同分辨率的 iPad 正在占用 Parsec 共享模式", kBlockedRetryMs);
            // Once per episode, not on every check: this comes back every few
            // seconds for as long as the other iPad streams.
            if (!blockedLogged) {
                blockedLogged = true;
                Logf(ip, "waiting: an iPad with a %ux%u panel is streaming and this one is %ux%u — parsec-vdd only "
                         "holds one custom resolution at a time\n",
                     activeWidth, activeHeight, width, height);
            }
            conn->Close();
            InterruptibleSleep(kBlockedRetryMs);
            continue;
        }
        blockedByWidth_ = 0;
        blockedByHeight_ = 0;
        blockedLogged = false;

        bool pipelineOk;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex);
            pipelineOk = buildPipeline(width, height);
        }
        if (!pipelineOk) {
            conn->Close();
            // A partially constructed pipeline may already have added the
            // monitor. Do not leave that display attached through an endless
            // retry loop when capture or encoder setup failed.
            detachManagedDisplay();
            InterruptibleSleep(kReconnectDelayMs);
            continue;
        }

        width_ = dup.Width();
        height_ = dup.Height();
        state_ = State::Streaming;
        PublishStatus(ConnectionPhase::Streaming);
        everStreamed = true;
        disconnectedSince.reset();

        std::atomic<bool> running{true};
        std::atomic<bool> receiverSleeping{false};
        std::atomic<bool> receiverClosing{false};
        std::atomic<bool> keyFrameRequested{false};
        std::mutex resizeMutex;
        std::optional<std::pair<uint32_t, uint32_t>> pendingResize;
        bool loggedPencil = false; // reader-thread only; one line per connection

        std::thread reader([&] {
            while (running && !stopRequested_) {
                auto frame = conn->ReadFrame();
                if (!frame) {
                    running = false;
                    break;
                }
                if (!IsControlPayload(frame->data(), frame->size()))
                    continue;
                auto msg = ParseControlMessage(frame->data(), frame->size());
                if (!msg)
                    continue;

                switch (msg->type) {
                    case ControlType::Ping: {
                        auto pong = SerializePong(msg->ping);
                        if (pong && !conn->SendFrame(reinterpret_cast<const uint8_t*>(pong->data()),
                                                     static_cast<uint32_t>(pong->size())))
                            running = false;
                        break;
                    }
                    case ControlType::Kf:
                        Logf(ip, "kf requested by receiver\n");
                        // H264Encoder and its Media Foundation COM objects are
                        // owned by the RunLoop worker thread. Publish the
                        // request; never touch the MFT from this reader thread.
                        keyFrameRequested = true;
                        break;
                    case ControlType::Hello: {
                        if (!HelloMatchesExpectedDevice(msg->hello, expectedDeviceId)) {
                            Logf(ip, "receiver identity changed during session; disconnecting\n");
                            PublishStatus(ConnectionPhase::Failed, FailureReason::DeviceIdentityMismatch,
                                          "会话中的设备身份发生变化，已断开");
                            running = false;
                            break;
                        }
                        std::string repeatedWelcome = SerializeWelcome();
                        if (!conn->SendFrame(reinterpret_cast<const uint8_t*>(repeatedWelcome.data()),
                                             static_cast<uint32_t>(repeatedWelcome.size()))) {
                            running = false;
                            break;
                        }
                        uint32_t w = 0;
                        uint32_t h = 0;
                        if (!NormalizePanelSize(msg->hello.pixelsWide, msg->hello.pixelsHigh, w, h)) {
                            Logf(ip, "refusing invalid rotated panel dimensions %dx%d\n",
                                 msg->hello.pixelsWide, msg->hello.pixelsHigh);
                            running = false;
                            conn->Interrupt();
                            break;
                        }
                        Logf(ip, "hello again: %dx%d -> scheduling pipeline rebuild at %ux%u\n",
                             msg->hello.pixelsWide, msg->hello.pixelsHigh, w, h);
                        {
                            std::lock_guard<std::mutex> resizeLock(resizeMutex);
                            pendingResize = std::pair{w, h};
                        }
                        break;
                    }
                    case ControlType::Touch:
                        // No pipelineMutex here on purpose: the capture loop
                        // holds it ~90% of the time (each capture blocks up to
                        // one frame interval), and taking it here starved
                        // input injection — the actual cause of the sluggish
                        // touch/drag. `input` is only ever touched by this
                        // reader thread (its initial SetMonitorRect happens on
                        // the main thread before this thread starts, and
                        // rotation rebuilds run on this same thread), so no
                        // lock is needed. Refresh the mapping rect first
                        // (thread-safe read) so a live monitor drag is followed
                        // immediately instead of only after the next reconnect.
                        input.SetMonitorRect(currentMonitorRect());
                        input.HandleTouch(msg->touch);
                        break;
                    case ControlType::Scroll:
                        input.SetMonitorRect(currentMonitorRect());
                        input.HandleScroll(msg->scroll);
                        break;
                    case ControlType::Pencil:
                        if (!loggedPencil) {
                            // Proof the version handshake landed: without our
                            // `welcome` the iPad would send `touch` here.
                            loggedPencil = true;
                            Logf(ip, "pencil input active (receiver honoured welcome pv=3)\n");
                        }
                        input.SetMonitorRect(currentMonitorRect());
                        input.HandlePencil(msg->pencil);
                        break;
                    case ControlType::Proximity:
                        input.SetMonitorRect(currentMonitorRect());
                        input.HandleProximity(msg->proximity);
                        break;
                    case ControlType::Stats: {
                        std::string stats = msg->stats.json.substr(0, 2048);
                        Logf(ip, "PHONE-STATS %s%s\n", stats.c_str(), msg->stats.json.size() > stats.size() ? "..." : "");
                        PublishReceiverStats(msg->stats.json);
                        break;
                    }
                    case ControlType::Sleeping:
                        Logf(ip, "receiver is sleeping; tearing down the display and waiting for wake\n");
                        PublishStatus(ConnectionPhase::Sleeping, FailureReason::OpenDisplayUnavailable,
                                      "iPad 接收端已休眠；返回 OpenDisplay 后会重新连接", kReconnectDelayMs);
                        receiverSleeping = true;
                        running = false;
                        conn->Interrupt();
                        break;
                    case ControlType::Closing:
                        Logf(ip, "receiver app is closing; ending this session\n");
                        PublishStatus(ConnectionPhase::Idle, FailureReason::OpenDisplayUnavailable,
                                      "OpenDisplay 已关闭");
                        receiverClosing = true;
                        running = false;
                        conn->Interrupt();
                        break;
                    default:
                        break;
                }
            }
        });

        std::vector<uint8_t> nv12;
        auto lastSend = std::chrono::steady_clock::now();
        auto lastPing = std::chrono::steady_clock::now();
        auto lastNetworkCheck = std::chrono::steady_clock::now();
        auto lastChange = std::chrono::steady_clock::now() - std::chrono::milliseconds(kActiveTailMs);
        // A drop is "pending" until something goes out again. Guards the replay
        // below against re-arming itself on every failed attempt.
        bool dropPending = false;
        // Set when the monitor rect couldn't be read right after a geometry
        // change (the desktop can still be mid-reconfigure); retried below
        // until it succeeds, because nothing else refreshes it in place.
        bool rectStale = false;

        // Watchdog for the panel size (see the check further down): when the
        // monitor is left on a size that isn't this iPad's, this is when it
        // started, and how many rebuilds we already spent on it.
        std::chrono::steady_clock::time_point wrongSizeSince{};
        bool sizeRebuildDone = false;
        bool sizeGiveUpLogged = false;

        while (running && !stopRequested_) {
            auto loopNow = std::chrono::steady_clock::now();
        if (loopNow - lastNetworkCheck >= std::chrono::milliseconds(kSenderPingMs)) {
                NetworkSafetyResult liveSafety = IsUsbMuxTarget(ip)
                                                    ? NetworkSafetyResult{true, "direct USB cable"}
                                                    : CheckTrustedLan(ip, settings.requirePrivateNetwork);
                lastNetworkCheck = loopNow;
                if (!liveSafety.allowed) {
                    Logf(ip, "stopping stream because trusted-LAN policy changed: %s\n", liveSafety.reason.c_str());
                    state_ = State::UnsafeNetwork;
                    running = false;
                    conn->Interrupt();
                    break;
                }
            }
            PublishMonitorRect(currentMonitorRect());

            std::optional<std::pair<uint32_t, uint32_t>> requestedResize;
            {
                std::lock_guard<std::mutex> resizeLock(resizeMutex);
                requestedResize.swap(pendingResize);
            }
            if (requestedResize) {
                auto [w, h] = *requestedResize;
                if (w != width || h != height) {
                    Logf(ip, "worker rebuilding pipeline at %ux%u\n", w, h);
                    // Turning the iPad changes the size under the claim. Alone
                    // that is safe; if another same-size iPad is attached the
                    // shared Parsec custom mode can temporarily letterbox it.
                    if (managedDisplay && !RetunePanel(w, h))
                        Logf(ip, "rotating while another iPad is attached — its picture may be letterboxed "
                                 "until one reconnects\n");

                    std::lock_guard<std::mutex> lock(pipelineMutex);
                    if (!buildPipeline(w, h)) {
                        Logf(ip, "pipeline rebuild failed, disconnecting\n");
                        running = false;
                        conn->Interrupt();
                        break;
                    }
                    width = w;
                    height = h;
                    width_ = dup.Width();
                    height_ = dup.Height();
                }
                keyFrameRequested = true;
            }

            std::vector<EncodedFrame> encoded;
            {
                // Only capture+encode need the pipeline lock (they touch dup
                // and encoder, which the reader thread may rebuild on
                // rotation). The network send is deliberately outside the
                // lock so a slow link never stalls a pending rebuild. Input
                // injection deliberately does NOT take this lock (see the
                // reader thread) — this loop holds it almost continuously.
                std::lock_guard<std::mutex> lock(pipelineMutex);

                if (keyFrameRequested.exchange(false))
                    encoder.RequestKeyFrame();

                nv12.resize(static_cast<size_t>(dup.Width()) * dup.Height() * 3 / 2);
                // CaptureFrameNv12 returns false on a pure timeout (nothing on
                // screen or cursor changed since last time).
                const auto captureStarted = std::chrono::steady_clock::now();
                bool changed = dup.CaptureFrameNv12(nv12, 1000 / static_cast<int>(settings.fps));
                const double captureMs = std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - captureStarted)
                                             .count();

                // Rotation or a resolution change made on the Windows side
                // never sends a `hello`, so nothing rebuilds the pipeline: the
                // encoder would keep the old geometry and read the new frame
                // with the wrong stride — a skewed picture on the iPad while
                // the host's own screenshot looks fine. Follow the capture.
                //
                // Only once a frame actually arrived: the recovery path in
                // CaptureFrameNv12 reopens the duplication and takes fresh
                // dimensions from the ModeDesc while returning false, so
                // dup.Width()/Height() can already describe the new geometry
                // while nv12 still holds the previous frame. Reconfiguring on
                // that would encode the old buffer with the new stride — one
                // skewed frame, exactly what this is here to prevent.
                if (changed && (encoder.Width() != dup.Width() || encoder.Height() != dup.Height())) {
                    Logf(ip, "capture is now %ux%u (encoder had %ux%u), reconfiguring\n", dup.Width(), dup.Height(),
                           encoder.Width(), encoder.Height());
                    if (encoder.Configure(dup.Width(), dup.Height(), settings.fps, settings.bitrateBps)) {
                        // The cached rect is only refreshed when the monitor
                        // moves, and a rotation in place doesn't move it.
                        rectStale = managedDisplay && !vdisp.QueryMonitorRect();
                        input.SetMonitorRect(currentMonitorRect());
                        encoder.RequestKeyFrame();
                        width_ = dup.Width();
                        height_ = dup.Height();
                    } else {
                        Logf(ip, "encoder reconfigure failed, dropping the connection\n");
                        running = false;
                    }
                } else if (managedDisplay && rectStale && vdisp.QueryMonitorRect()) {
                    // The desktop was still mid-reconfigure above. Without this
                    // retry the touch mapping would stay on the old geometry
                    // until the monitor is moved or the pipeline rebuilt.
                    input.SetMonitorRect(currentMonitorRect());
                    rectStale = false;
                }

                auto now = std::chrono::steady_clock::now();

                // Adding or removing *any* parsec virtual display resets the
                // mode of *every* parsec monitor; Windows then restores each
                // one from what it last persisted for that display path. With
                // two iPads the paths get swapped around, so a neighbour
                // connecting can leave this monitor on the other iPad's size —
                // the picture then arrives letterboxed on this panel. The
                // reconfigure above keeps it correct but wrong-shaped, so once
                // the churn has settled, put our own size back.
                //
                // Only when the size is neither the panel's nor the panel
                // rotated: a rotation made in Windows is the user's decision
                // and is adopted, not undone.
                if (managedDisplay && dup.Width() == height && dup.Height() == width) {
                    std::swap(width, height); // rotated in Windows: that is the panel size now
                }
                // Exactly one attempt, and only after the churn has settled. A
                // rebuild resets every parsec monitor in turn, so retrying is
                // how two senders end up trading rebuilds forever — and a
                // second attempt can't help anyway: either Windows had merely
                // restored a stale mode for this display path (the rebuild
                // fixes that), or another sender's panel size is in force, and
                // then no amount of rebuilding wins (see the note below).
                if (!managedDisplay || (dup.Width() == width && dup.Height() == height)) {
                    wrongSizeSince = {};
                    sizeRebuildDone = false;
                } else if (wrongSizeSince == std::chrono::steady_clock::time_point{}) {
                    wrongSizeSince = now;
                } else if (!sizeRebuildDone &&
                           now - wrongSizeSince > std::chrono::milliseconds(kWrongSizeGraceMs)) {
                    // A remove + re-add is what gets the panel size back;
                    // re-applying the mode on the live monitor is refused
                    // (DISP_CHANGE_BADMODE) while the capture runs.
                    sizeRebuildDone = true;
                    Logf(ip, "monitor sits at %ux%u instead of %ux%u, rebuilding once\n", dup.Width(), dup.Height(),
                         width, height);
                    if (!buildPipeline(width, height))
                        Logf(ip, "rebuild for the panel size failed, keeping what we have\n");
                    else
                        continue; // nv12 still holds the pre-rebuild frame — capture a fresh one
                } else if (sizeRebuildDone && !sizeGiveUpLogged) {
                    // parsec-vdd puts *one* custom resolution on all of its
                    // virtual monitors: whichever sender sets its panel size
                    // last drags every other monitor along. Two iPads with
                    // different panels therefore can't both run native, and
                    // the smaller one shows the picture letterboxed. Said once
                    // per connection, then we stop touching the monitor.
                    sizeGiveUpLogged = true;
                    Logf(ip, "monitor stays at %ux%u (this iPad is %ux%u): parsec-vdd shares one custom resolution "
                             "across all its monitors, so the picture stays letterboxed here\n",
                         dup.Width(), dup.Height(), width, height);
                }

                if (changed)
                    lastChange = now;

                // The async encoder holds one frame until the *next* frame is
                // fed, so if we stopped feeding the instant the screen went
                // idle, the last frame of an interaction (a tap, the end of a
                // scroll) would sit in the encoder until the next change or
                // keepalive — up to a second later, which feels sluggish. Keep
                // feeding at capture rate for a short tail after the last
                // change so the encoder stays drained and interaction stays
                // low-latency.
                bool active = now - lastChange < std::chrono::milliseconds(kActiveTailMs);

                // Keepalive re-encodes the last image (a tiny P-frame) purely
                // so the iPad's ~5s liveness watchdog (spec §5) never trips on
                // an otherwise idle desktop. Not gated on "captured a real
                // frame yet": if the first DXGI frame is delayed past the
                // watchdog, this still sends the (zero-filled) buffer so the
                // connection survives until real content arrives.
                bool keepaliveDue = now - lastSend >= std::chrono::milliseconds(kKeepaliveMs);

                std::optional<double> encodeMs;
                if (running && (active || keepaliveDue)) {
                    const auto encodeStarted = std::chrono::steady_clock::now();
                    encoded = encoder.EncodeNv12(nv12.data(), nv12.size());
                    encodeMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - encodeStarted)
                                   .count();
                }
                PublishPipelineTimings(captureMs, encodeMs);
            }

            bool sentSomething = false;
            for (auto& f : encoded) {
                // Backpressure: if the socket can't take data within the
                // budget, drop the whole frame (never a partial write — that
                // would desync the receiver's framing) and force a keyframe
                // so the next frame we do send resyncs the decoder.
                if (!conn->WaitWritable(kSendTimeoutMs)) {
                    encoder.RequestKeyFrame();
                    // A dropped frame leaves the receiver on the previous
                    // image, and Desktop Duplication delivers nothing new once
                    // the desktop goes static, so the correction would wait for
                    // the keepalive — up to a second. Count the drop as a
                    // change: the active tail re-feeds the last captured buffer
                    // right away, no second timer needed (upstream does the
                    // same with a dedicated 30ms replay timer, see #207).
                    //
                    // Only for the *first* drop of an episode. Re-arming on
                    // every failed attempt would keep `active` true for as long
                    // as the link stays congested, burning an encode per
                    // WaitWritable timeout on frames nobody can receive. One
                    // prompt replay, then fall back to the keepalive until the
                    // socket drains.
                    if (!dropPending) {
                        lastChange = std::chrono::steady_clock::now();
                        dropPending = true;
                        // Once per episode, not per dropped frame: on a link
                        // that stays congested this would otherwise be the
                        // loudest line in the log.
                        Logf(ip, "send backpressure, dropped a frame\n");
                        std::lock_guard<std::mutex> statusLock(statusMutex_);
                        ++networkDrops_;
                    }
                    break;
                }
                if (!conn->SendFrame(f.annexB.data(), static_cast<uint32_t>(f.annexB.size()), kSendTimeoutMs)) {
                    // Timeout/error can leave a partial frame on the wire.
                    // Drop the connection and let the outer loop resync with a
                    // fresh hello and keyframe; never reuse this TCP stream.
                    running = false;
                    break;
                }
                sentSomething = true;
                dropPending = false;
                if (f.isKeyFrame)
                    Logf(ip, "sent keyframe, %zu bytes\n", f.annexB.size());
            }
            if (sentSomething)
                lastSend = std::chrono::steady_clock::now();

            auto now = std::chrono::steady_clock::now();
            if (running && now - lastPing >= std::chrono::milliseconds(kSenderPingMs)) {
                std::string ping = SerializeSenderPing();
                if (!conn->SendFrame(reinterpret_cast<const uint8_t*>(ping.data()),
                                     static_cast<uint32_t>(ping.size())))
                    running = false;
                lastPing = now;
            }
        }

        conn->Interrupt();
        reader.join();
        conn->Close();
        // After the reader is gone, so nothing else touches the injector.
        input.EndSession();
        width_ = 0;
        height_ = 0;
        if (receiverSleeping || receiverClosing)
            detachManagedDisplay();
        else if (!stopRequested_ && !disconnectedSince)
            disconnectedSince = std::chrono::steady_clock::now();

        Logf(ip, "disconnected%s\n", stopRequested_ || receiverClosing ? "" : ", reconnecting");
        if (!stopRequested_ && !receiverClosing && !receiverSleeping)
            PublishStatus(ConnectionPhase::Reconnecting, FailureReason::TransportError,
                          "连接已中断，正在重连原通道", kReconnectDelayMs);
        if (receiverClosing)
            break;
    }

    detachManagedDisplay();
    if (ipLock != nullptr)
        CloseHandle(ipLock);
    state_ = State::Idle;
    if (stopRequested_)
        PublishStatus(ConnectionPhase::Idle);
}

} // namespace od
