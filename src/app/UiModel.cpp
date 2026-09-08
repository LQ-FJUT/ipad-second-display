#include "app/UiModel.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace od {

std::wstring ConnectionPhaseTitle(ConnectionPhase phase)
{
    switch (phase) {
        case ConnectionPhase::Connecting: return L"正在连接";
        case ConnectionPhase::WaitingHello: return L"等待 OpenDisplay";
        case ConnectionPhase::PreparingDisplay: return L"正在准备副屏";
        case ConnectionPhase::Streaming: return L"投屏中";
        case ConnectionPhase::Reconnecting: return L"正在重连";
        case ConnectionPhase::Sleeping: return L"iPad 已休眠";
        case ConnectionPhase::Blocked: return L"等待分辨率资源";
        case ConnectionPhase::UnsafeNetwork: return L"已阻止连接";
        case ConnectionPhase::Failed: return L"连接失败";
        default: return L"未连接";
    }
}

std::wstring FailureHelpText(FailureReason failure, const std::string& detail)
{
    if (!detail.empty()) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, detail.c_str(), static_cast<int>(detail.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>((std::max)(length, 0)), L'\0');
        if (length > 0)
            MultiByteToWideChar(CP_UTF8, 0, detail.c_str(), static_cast<int>(detail.size()), wide.data(), length);
        return wide;
    }
    switch (failure) {
        case FailureReason::ReceiverUnavailable:
        case FailureReason::OpenDisplayUnavailable: return L"请在 iPad 前台打开 OpenDisplay 后重试";
        case FailureReason::UsbUnavailable: return L"请解锁 iPad，并确认已信任此电脑";
        case FailureReason::NetworkUnsafe: return L"请将当前网络设为专用网络，或改用 USB";
        case FailureReason::DriverUnavailable: return L"请安装或检查 Parsec Virtual Display Driver";
        case FailureReason::ResolutionUnavailable: return L"请完成首次分辨率注册，或断开不同尺寸的另一台 iPad";
        case FailureReason::CaptureUnavailable: return L"Windows 无法捕获虚拟显示器，请重新连接";
        case FailureReason::EncoderUnavailable: return L"H.264 编码器不可用，请检查显卡驱动";
        case FailureReason::AnotherSender: return L"请退出另一个 iPad互联进程";
        case FailureReason::InvalidPanel:
        case FailureReason::ProtocolError: return L"OpenDisplay 返回的数据不兼容，请重新打开接收端";
        case FailureReason::DeviceIdentityMismatch: return L"地址已被另一台设备使用；请刷新发现记录后重试";
        case FailureReason::TransportError: return L"链路已中断，正在重连原通道";
        default: return L"点击重试；如果仍失败，可导出诊断报告";
    }
}

StreamProfile InferStreamProfile(uint32_t fps, uint32_t bitrateMbps)
{
    if (fps == 60 && bitrateMbps == 30)
        return StreamProfile::Balanced;
    if (fps == 60 && bitrateMbps == 15)
        return StreamProfile::LowBandwidth;
    if (fps == 60 && bitrateMbps == 45)
        return StreamProfile::Sharp;
    return StreamProfile::Custom;
}

std::vector<DeviceConfig> ParseDeviceEntriesText(const std::wstring& text, uint16_t defaultPort)
{
    std::vector<DeviceConfig> devices;
    for (size_t pos = 0; pos <= text.size();) {
        size_t end = text.find_first_of(L"\r\n", pos);
        if (end == std::wstring::npos)
            end = text.size();
        const std::wstring line = text.substr(pos, end - pos);
        const size_t first = line.find_first_not_of(L" \t");
        const size_t last = line.find_last_not_of(L" \t");
        if (first != std::wstring::npos) {
            const std::wstring trimmed = line.substr(first, last - first + 1);
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, trimmed.c_str(), static_cast<int>(trimmed.size()), nullptr,
                                                  0, nullptr, nullptr);
            std::string device(static_cast<size_t>((std::max)(bytes, 0)), '\0');
            if (bytes > 0)
                WideCharToMultiByte(CP_UTF8, 0, trimmed.c_str(), static_cast<int>(trimmed.size()), device.data(), bytes,
                                    nullptr, nullptr);
            const size_t separator = device.find_first_of(" \t");
            const std::string address = device.substr(0, separator);
            const bool known = std::any_of(devices.begin(), devices.end(), [&](const DeviceConfig& existing) {
                return existing.lastIpv4 == address || (!existing.bonjourHost.empty() && existing.bonjourHost == address);
            });
            if (!address.empty() && !known) {
                DeviceConfig configured;
                configured.lastIpv4 = address;
                configured.port = defaultPort;
                configured.priority = static_cast<uint32_t>(devices.size());
                if (separator != std::string::npos) {
                    const size_t nameStart = device.find_first_not_of(" \t", separator);
                    if (nameStart != std::string::npos) configured.name = device.substr(nameStart);
                }
                if (configured.name.empty()) configured.name = address;
                devices.push_back(std::move(configured));
            }
        }
        if (end == text.size())
            break;
        pos = end + 1;
    }
    return devices;
}

std::wstring DeviceEntryText(const DeviceConfig& device)
{
    const std::string endpoint = !device.lastIpv4.empty() ? device.lastIpv4 : device.bonjourHost;
    std::string line = endpoint;
    if (!device.name.empty() && device.name != endpoint) line += " " + device.name;
    if (line.empty()) line = device.id;
    const int chars = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>((std::max)(chars, 0)), L'\0');
    if (chars > 0) MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), result.data(), chars);
    return result;
}

namespace {

bool IsIpv4(std::string_view value)
{
    int parts = 0;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find('.', start);
        const size_t stop = end == std::string_view::npos ? value.size() : end;
        if (stop == start || stop - start > 3)
            return false;
        int octet = 0;
        for (size_t i = start; i < stop; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(value[i])))
                return false;
            octet = octet * 10 + (value[i] - '0');
        }
        if (octet > 255 || ++parts > 4)
            return false;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return parts == 4;
}

} // namespace

std::string RedactDiagnosticsText(std::string text, const std::string& userProfile)
{
    if (!userProfile.empty()) {
        for (size_t pos = text.find(userProfile); pos != std::string::npos; pos = text.find(userProfile, pos + 9))
            text.replace(pos, userProfile.size(), "<profile>");
    }

    for (size_t start = 0; start < text.size();) {
        if (!std::isdigit(static_cast<unsigned char>(text[start]))) {
            ++start;
            continue;
        }
        size_t end = start;
        int dots = 0;
        while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '.')) {
            dots += text[end] == '.' ? 1 : 0;
            ++end;
        }
        if (dots == 3 && IsIpv4(std::string_view(text).substr(start, end - start))) {
            text.replace(start, end - start, "<ip>");
            start += 4;
        } else {
            start = end;
        }
    }

    for (size_t pos = text.find("usbmux://"); pos != std::string::npos; pos = text.find("usbmux://", pos + 18)) {
        size_t end = pos + 9;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != ']')
            ++end;
        text.replace(pos, end - pos, "usbmux://<device>");
    }
    return text;
}

} // namespace od
