#pragma once

#include <string>

namespace od {

struct NetworkSafetyResult {
    bool allowed = false;
    std::string reason;
    // Numeric endpoint produced by the same resolution that passed policy.
    // Callers use this value for connect, avoiding a second DNS resolution.
    std::string resolvedTarget;
};

// Enforces the product's trusted-LAN boundary before a screen frame can leave
// the PC. Loopback is always accepted for the bundled mock receiver. Remote
// targets must resolve to a private/link-local IPv4 address and Windows must
// report at least one connected Private or Domain network.
NetworkSafetyResult CheckTrustedLan(const std::string& target, bool requirePrivateNetwork);

} // namespace od
