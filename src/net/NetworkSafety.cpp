#include "net/NetworkSafety.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <netlistmgr.h>
#include <wrl/client.h>

#include <vector>

namespace od {

namespace {

using Microsoft::WRL::ComPtr;

bool IsLoopback(uint32_t hostOrder)
{
    return (hostOrder & 0xFF000000u) == 0x7F000000u;
}

bool IsPrivateOrLinkLocal(uint32_t hostOrder)
{
    return (hostOrder & 0xFF000000u) == 0x0A000000u ||
           (hostOrder & 0xFFF00000u) == 0xAC100000u ||
           (hostOrder & 0xFFFF0000u) == 0xC0A80000u ||
           (hostOrder & 0xFFFF0000u) == 0xA9FE0000u;
}

struct ResolvedAddress {
    sockaddr_storage storage{};
    int length = 0;
    std::string numeric;
};

bool ResolveTarget(const std::string& target, std::vector<ResolvedAddress>& addresses)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    if (getaddrinfo(target.c_str(), nullptr, &hints, &result) != 0) return false;
    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        if (item->ai_family != AF_INET && item->ai_family != AF_INET6) continue;
        ResolvedAddress resolved;
        resolved.length = static_cast<int>(item->ai_addrlen);
        std::memcpy(&resolved.storage, item->ai_addr, item->ai_addrlen);
        char host[NI_MAXHOST]{};
        if (getnameinfo(item->ai_addr, static_cast<int>(item->ai_addrlen), host, sizeof(host), nullptr, 0,
                        NI_NUMERICHOST) == 0) {
            resolved.numeric = host;
            addresses.push_back(std::move(resolved));
        }
    }
    freeaddrinfo(result);
    return !addresses.empty();
}

bool IsLoopback(const ResolvedAddress& address)
{
    if (address.storage.ss_family == AF_INET)
        return IsLoopback(ntohl(reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_addr.s_addr));
    const auto& value = reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_addr;
    return IN6_IS_ADDR_LOOPBACK(&value) != 0;
}

bool IsPrivateOrLinkLocal(const ResolvedAddress& address)
{
    if (address.storage.ss_family == AF_INET)
        return IsPrivateOrLinkLocal(ntohl(reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_addr.s_addr));
    const auto& bytes = reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_addr.u.Byte;
    return (bytes[0] & 0xFE) == 0xFC || (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80);
}

bool HasTrustedWindowsNetwork(const ResolvedAddress& target)
{
    sockaddr_storage destination = target.storage;
    DWORD interfaceIndex = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &interfaceIndex) != NO_ERROR)
        return false;

    NET_LUID interfaceLuid{};
    GUID adapterId{};
    if (ConvertInterfaceIndexToLuid(interfaceIndex, &interfaceLuid) != NO_ERROR ||
        ConvertInterfaceLuidToGuid(&interfaceLuid, &adapterId) != NO_ERROR)
        return false;

    ComPtr<INetworkListManager> manager;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager))))
        return false;

    ComPtr<IEnumNetworkConnections> connections;
    if (FAILED(manager->GetNetworkConnections(&connections)))
        return false;

    bool foundConnectedRoute = false;
    for (;;) {
        ComPtr<INetworkConnection> connection;
        ULONG fetched = 0;
        HRESULT hr = connections->Next(1, connection.GetAddressOf(), &fetched);
        if (hr == S_FALSE && fetched == 0)
            break;
        if (hr != S_OK || fetched != 1)
            return false; // enumeration failure is not proof of a trusted route

        GUID candidateId{};
        if (FAILED(connection->GetAdapterId(&candidateId)))
            return false;
        if (!IsEqualGUID(candidateId, adapterId))
            continue;

        VARIANT_BOOL connected = VARIANT_FALSE;
        if (FAILED(connection->get_IsConnected(&connected)))
            return false;
        if (connected != VARIANT_TRUE)
            continue;
        foundConnectedRoute = true;

        ComPtr<INetwork> network;
        if (FAILED(connection->GetNetwork(&network)))
            return false;
        NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
        if (FAILED(network->GetCategory(&category)) ||
            (category != NLM_NETWORK_CATEGORY_PRIVATE && category != NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED))
            return false;
    }
    return foundConnectedRoute;
}

} // namespace

NetworkSafetyResult CheckTrustedLan(const std::string& target, bool requirePrivateNetwork)
{
    std::vector<ResolvedAddress> addresses;
    if (!ResolveTarget(target, addresses))
        return {false, "target could not be resolved to IPv4 or IPv6"};
    if (!requirePrivateNetwork)
        return {true, "private-network enforcement disabled by explicit configuration", addresses.front().numeric};

    bool allLoopback = true;
    for (const ResolvedAddress& address : addresses) {
        bool loopback = IsLoopback(address);
        allLoopback = allLoopback && loopback;
        if (!loopback && !IsPrivateOrLinkLocal(address))
            return {false, "target resolves to a non-private address"};
    }
    if (allLoopback)
        return {true, "loopback self-test", addresses.front().numeric};

    // Validate the profile of the route that Windows would actually use for
    // every address returned by the name. Merely finding some unrelated
    // Private adapter is insufficient when the iPad route itself is Public.
    for (const ResolvedAddress& address : addresses) {
        if (!IsLoopback(address) && !HasTrustedWindowsNetwork(address))
            return {false, "the Windows network route to the target is Public or cannot be verified; set that trusted LAN to Private"};
    }
    return {true, "trusted private LAN", addresses.front().numeric};
}

} // namespace od
