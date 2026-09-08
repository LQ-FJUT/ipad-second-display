#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <windows.h>

namespace od {

class Connection;

struct StreamSettings {
    uint32_t fps = 60;
    uint32_t bitrateBps = 30'000'000;
    bool requirePrivateNetwork = true;

    // Empty: create and own a Parsec virtual display (normal product mode).
    // Non-empty: capture that already-attached \\.\DISPLAYn read-only. This is
    // intended for the no-driver encoder/protocol gate and never adds/removes a
    // monitor or changes its resolution.
    std::wstring existingDisplayName;
};

// User-facing lifecycle and failure information.  The wire protocol remains
// unchanged; this is a read-only snapshot published by the sender worker for
// the tray/control-panel UI and diagnostics export.
enum class ConnectionPhase {
    Idle,
    Connecting,
    WaitingHello,
    PreparingDisplay,
    Streaming,
    Reconnecting,
    Sleeping,
    Blocked,
    UnsafeNetwork,
    Failed,
};

enum class FailureReason {
    None,
    ReceiverUnavailable,
    OpenDisplayUnavailable,
    UsbUnavailable,
    NetworkUnsafe,
    DriverUnavailable,
    ResolutionUnavailable,
    CaptureUnavailable,
    EncoderUnavailable,
    InvalidPanel,
    ProtocolError,
    DeviceIdentityMismatch,
    AnotherSender,
    TransportError,
};

struct ConnectionSnapshot {
    std::string deviceId;
    std::string transport;
    uint32_t attempt = 0;
    ConnectionPhase phase = ConnectionPhase::Idle;
    FailureReason failure = FailureReason::None;
    std::string detail;
    uint32_t width = 0;
    uint32_t height = 0;
    int retryInMs = 0;
    uint64_t networkDrops = 0;
    double receiverRttMs = -1.0;
    double receiverMbps = -1.0;
    double actualFps = -1.0;
    int receiverStalls = -1;
    double captureMs = -1.0;
    double encodeMs = -1.0;
    std::wstring displayName;
};

// Owns the sender lifecycle: connect, wait for hello, build the capture/encode
// pipeline, stream, recover from disconnects/rotation. Can run either on a
// background thread (Start/Stop, for the tray GUI) or inline (RunBlocking, for
// the headless CLI). The reconnect loop runs until stopped.
class SenderApp {
public:
    // Blocked: an iPad with a different panel size is streaming. parsec-vdd
    // puts one custom resolution on all of its virtual monitors, so this one
    // would only get a letterboxed picture — it waits for the other to finish.
    enum class State { Idle, Connecting, Streaming, Blocked, UnsafeNetwork };

    SenderApp() = default;
    ~SenderApp();

    SenderApp(const SenderApp&) = delete;
    SenderApp& operator=(const SenderApp&) = delete;

    // Start streaming to ip:port on a background thread. No-op if already
    // running (Stop first to retarget).
    void Start(std::string ip, uint16_t port, StreamSettings settings = {},
               std::string expectedDeviceId = {}, std::string transport = "wifi");

    // Non-blocking half of Stop, for UI command/switch paths. The UI can poll
    // IsRunning and start the replacement only after the old worker exits.
    void RequestStop();

    // Signal stop and join the worker. Safe to call when not running.
    void Stop();

    // Run the loop inline on the calling thread until the process is killed
    // (headless CLI). Does not return under normal operation.
    void RunBlocking(std::string ip, uint16_t port, StreamSettings settings = {});

    bool IsRunning() const { return running_.load(); }
    State GetState() const { return state_.load(); }
    uint32_t Width() const { return width_.load(); }   // current capture px, 0 until connected
    uint32_t Height() const { return height_.load(); }
    std::optional<RECT> StreamMonitorRect() const;

    // While Blocked: the panel size currently holding the display, so the UI
    // can name what this iPad is waiting for. Zero otherwise.
    uint32_t BlockedByWidth() const { return blockedByWidth_.load(); }
    uint32_t BlockedByHeight() const { return blockedByHeight_.load(); }

    ConnectionSnapshot Snapshot() const;

private:
    void RunLoop(std::string ip, uint16_t port, StreamSettings settings,
                 std::string expectedDeviceId, std::string transport);
    void InterruptibleSleep(int ms);
    void PublishMonitorRect(RECT rect);
    void PublishStatus(ConnectionPhase phase, FailureReason failure = FailureReason::None,
                       std::string detail = {}, int retryInMs = 0);
    void PublishDisplayName(std::wstring name);
    void PublishReceiverStats(const std::string& json);
    void PublishPipelineTimings(double captureMs, std::optional<double> encodeMs);

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<State> state_{State::Idle};
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    std::atomic<uint32_t> blockedByWidth_{0};
    std::atomic<uint32_t> blockedByHeight_{0};
    mutable std::mutex monitorMutex_;
    RECT monitorRect_{};

    mutable std::mutex statusMutex_;
    ConnectionPhase phase_ = ConnectionPhase::Idle;
    FailureReason failure_ = FailureReason::None;
    std::string failureDetail_;
    std::chrono::steady_clock::time_point retryAt_{};
    uint64_t networkDrops_ = 0;
    double receiverRttMs_ = -1.0;
    double receiverMbps_ = -1.0;
    double actualFps_ = -1.0;
    int receiverStalls_ = -1;
    double captureMs_ = -1.0;
    double encodeMs_ = -1.0;
    std::wstring displayName_;
    std::string deviceId_;
    std::string transport_;
    uint32_t attempt_ = 0;

    // Lets Stop() close the socket the worker is currently blocked on
    // (Connect's result / ReadFrame), so a stop doesn't wait for the next
    // timeout. Guarded because Stop() runs on another thread.
    std::mutex connMutex_;
    Connection* activeConn_ = nullptr;
};

} // namespace od
