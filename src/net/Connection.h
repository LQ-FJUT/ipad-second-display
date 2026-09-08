#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Prevent windows.h from pulling in winsock.h before winsock2.h; caller must
// only ever include this header, never <windows.h> first, in TUs that use it.
#include <winsock2.h>

namespace od {

// One TCP connection to the iPad receiver at <ip>:9000, with the wire framing
// ([4-byte big-endian length][payload], identical in both directions) built in.
// We are the connecting side; the iPad listens (spec §1).
class Connection {
public:
    Connection() = default;
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    // Connects to ip:port, sets TCP_NODELAY. Returns nullopt on failure.
    static std::optional<Connection> Connect(const std::string& ip, uint16_t port);

    // Blocking read of the next framed message. Returns nullopt on
    // disconnect/socket error.
    std::optional<std::vector<uint8_t>> ReadFrame();

    // Sends one length-prefixed frame within a whole-frame deadline. False can
    // mean the wire contains only a prefix/partial payload; callers must then
    // discard this connection immediately and reconnect, never send another
    // frame on the desynchronized stream.
    bool SendFrame(const uint8_t* data, uint32_t size, int timeoutMs = 1000);

    // Returns true if the socket can accept data now (send buffer has room).
    // Used for backpressure: the caller drops a whole frame rather than
    // blocking on a congested link and building latency. timeoutMs 0 = poll.
    bool WaitWritable(int timeoutMs);

    bool IsValid() const { return socket_.load() != INVALID_SOCKET; }

    // Wakes blocking recv/send from another thread without releasing the
    // numeric socket handle. The owner calls Close only after worker threads
    // have joined, preventing a closed handle value from being reused by a
    // different connection while an old thread still holds it locally.
    void Interrupt();
    void Close();

private:
    explicit Connection(SOCKET s) : socket_(s) {}

    bool ReadExact(uint8_t* buffer, size_t size);
    bool WriteExactUntil(const uint8_t* buffer, size_t size,
                         std::chrono::steady_clock::time_point deadline);

    // Stop() interrupts the socket from the UI thread while the worker can be
    // in recv/send. Atomic ownership avoids a C++ data race on the handle.
    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    // Serializes Interrupt's load+shutdown with Close's exchange+close. It is
    // intentionally not held by blocking recv/send, so interruption stays
    // able to wake those operations.
    std::mutex closeMutex_;
    std::mutex sendMutex_;
};

} // namespace od
