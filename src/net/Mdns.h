#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace od {

// An OpenDisplay receiver as it advertises itself over Bonjour.
struct DiscoveryRecord {
    std::string id;
    std::string name;
    std::string address;  // IPv4 from the A record
    std::vector<std::string> addresses;
    std::string instance; // Bonjour instance name — the name set in the app's settings
    std::string host;     // SRV target, e.g. "iPad-Pro.local" (the iOS device name)
    uint16_t port = 0;    // SRV port
    std::string source = "mdns";
    uint32_t ttlSeconds = 120;
    std::chrono::steady_clock::time_point lastSeen{};
    std::chrono::steady_clock::time_point expiresAt{};
    bool online = true;
    bool goodbye = false;
};
using MdnsReceiver = DiscoveryRecord;

class DiscoveryCache {
public:
    void Merge(const std::vector<DiscoveryRecord>& records,
               std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void Expire(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::optional<DiscoveryRecord> FindById(const std::string& id) const;
    std::vector<DiscoveryRecord> Records() const;
private:
    std::vector<DiscoveryRecord> records_;
};

// Browses `_opensidecar._tcp.local` for up to timeoutMs and returns what
// answered. The receivers publish PTR, SRV, TXT and A in one packet, so a
// single query is enough — no follow-up resolve.
//
// The query goes out on *every* IPv4 interface on purpose: left to Windows,
// multicast follows the route metric, which on a machine with VMware or Hyper-V
// adapters means the query never reaches the network the iPads are on.
std::vector<MdnsReceiver> BrowseReceivers(int timeoutMs);

// Runs the response parser against a well-formed packet and a handful of
// malformed ones (truncated names, a TXT length running past the record, a
// compression pointer loop, a header claiming records that aren't there).
// Prints what it checked and returns false on the first mismatch. Wired to
// `--browse-mdns`, because this parser is the only place in the sender that
// reads data from whoever happens to answer on the network.
bool SelfCheck();

} // namespace od
