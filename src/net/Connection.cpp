#include "net/Connection.h"
#include "net/UsbMux.h"

#include <algorithm>
#include <chrono>
#include <limits>

#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace od {

namespace {

// Bounds a single connect attempt so an unreachable iPad (SYN black-hole ~20s)
// fails fast — keeps reconnect snappy and Stop()/Disconnect from hanging.
constexpr int kConnectTimeoutMs = 3000;

bool ConnectWithTimeout(SOCKET s, const sockaddr* addr, int addrlen, int timeoutMs)
{
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    bool ok = false;
    int r = ::connect(s, addr, addrlen);
    if (r == 0) {
        ok = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        WSAPOLLFD pfd{};
        pfd.fd = s;
        pfd.events = POLLWRNORM;
        if (WSAPoll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & POLLWRNORM) != 0) {
            int soErr = 0;
            int len = sizeof(soErr);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len) == 0 && soErr == 0)
                ok = true;
        }
    }

    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking); // rest of the code uses blocking recv/send
    return ok;
}

} // namespace

Connection::~Connection()
{
    Close();
}

Connection::Connection(Connection&& other) noexcept : socket_(other.socket_.exchange(INVALID_SOCKET))
{
}

Connection& Connection::operator=(Connection&& other) noexcept
{
    if (this != &other) {
        Close();
        socket_ = other.socket_.exchange(INVALID_SOCKET);
    }
    return *this;
}

std::optional<Connection> Connection::Connect(const std::string& ip, uint16_t port)
{
    if (IsUsbMuxTarget(ip)) {
        auto socket = ConnectUsbMuxTarget(ip, port);
        if (!socket)
            return std::nullopt;
        BOOL noDelay = TRUE;
        setsockopt(*socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        return Connection(*socket);
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;

    addrinfo* result = nullptr;
    if (getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &result) != 0)
        return std::nullopt;

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;

        if (ConnectWithTimeout(s, p->ai_addr, static_cast<int>(p->ai_addrlen), kConnectTimeoutMs))
            break;

        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);

    if (s == INVALID_SOCKET)
        return std::nullopt;

    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    return Connection(s);
}

bool Connection::WaitWritable(int timeoutMs)
{
    SOCKET socket = socket_.load();
    if (socket == INVALID_SOCKET)
        return false;

    WSAPOLLFD pfd{};
    pfd.fd = socket;
    pfd.events = POLLWRNORM;
    int r = WSAPoll(&pfd, 1, timeoutMs);
    return r > 0 && (pfd.revents & POLLWRNORM) != 0;
}

bool Connection::ReadExact(uint8_t* buffer, size_t size)
{
    size_t total = 0;
    while (total < size) {
        SOCKET socket = socket_.load();
        if (socket == INVALID_SOCKET)
            return false;
        int n = recv(socket, reinterpret_cast<char*>(buffer + total), static_cast<int>(size - total), 0);
        if (n <= 0)
            return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

bool Connection::WriteExactUntil(const uint8_t* buffer, size_t size,
                                 std::chrono::steady_clock::time_point deadline)
{
    size_t total = 0;
    while (total < size) {
        SOCKET socket = socket_.load();
        if (socket == INVALID_SOCKET)
            return false;

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        DWORD timeout = static_cast<DWORD>(std::clamp<int64_t>(remaining, 1, std::numeric_limits<DWORD>::max()));
        if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0)
            return false;

        size_t chunk = std::min<size_t>(size - total, static_cast<size_t>(std::numeric_limits<int>::max()));
        int n = send(socket, reinterpret_cast<const char*>(buffer + total), static_cast<int>(chunk), 0);
        if (n <= 0)
            return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

std::optional<std::vector<uint8_t>> Connection::ReadFrame()
{
    if (!IsValid())
        return std::nullopt;

    uint8_t lenBytes[4];
    if (!ReadExact(lenBytes, sizeof(lenBytes)))
        return std::nullopt;

    uint32_t len = (static_cast<uint32_t>(lenBytes[0]) << 24) |
                    (static_cast<uint32_t>(lenBytes[1]) << 16) |
                    (static_cast<uint32_t>(lenBytes[2]) << 8) |
                    static_cast<uint32_t>(lenBytes[3]);

    // The iPad only ever sends us small control JSON (hello/touch/scroll/kf,
    // all < 32 KB per spec §3). A length larger than this cap means a corrupt
    // or desynced stream — refuse it rather than attempt a multi-GB allocation
    // from an attacker/garbage-controlled 32-bit length.
    constexpr uint32_t kMaxInboundFrame = 1u << 20; // 1 MiB, generous
    if (len > kMaxInboundFrame)
        return std::nullopt;

    std::vector<uint8_t> payload(len);
    if (len > 0 && !ReadExact(payload.data(), len))
        return std::nullopt;

    return payload;
}

bool Connection::SendFrame(const uint8_t* data, uint32_t size, int timeoutMs)
{
    // Receiver pings are handled on the reader thread while video is written
    // by the capture thread. Serialize the complete length+payload pair so a
    // pong can never splice itself into an H.264 frame on the TCP stream.
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (!IsValid())
        return false;
    if (timeoutMs <= 0)
        return false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    uint8_t lenBytes[4] = {
        static_cast<uint8_t>((size >> 24) & 0xFF),
        static_cast<uint8_t>((size >> 16) & 0xFF),
        static_cast<uint8_t>((size >> 8) & 0xFF),
        static_cast<uint8_t>(size & 0xFF),
    };

    if (!WriteExactUntil(lenBytes, sizeof(lenBytes), deadline)) {
        // Failure may follow a partial prefix. Poison the stream before
        // releasing sendMutex_ so the reader thread cannot append a pong to
        // already desynchronized framing.
        Interrupt();
        return false;
    }

    if (size > 0 && !WriteExactUntil(data, size, deadline)) {
        Interrupt();
        return false;
    }

    return true;
}

void Connection::Interrupt()
{
    std::lock_guard<std::mutex> lock(closeMutex_);
    SOCKET socket = socket_.load();
    if (socket != INVALID_SOCKET)
        shutdown(socket, SD_BOTH);
}

void Connection::Close()
{
    std::lock_guard<std::mutex> lock(closeMutex_);
    SOCKET socket = socket_.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
}

} // namespace od
