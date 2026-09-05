#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <iterator>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include "NetworkApi.h"
#include "NpcapDyn.h"

#pragma comment(lib, "Ws2_32.lib")

namespace
{
    struct AdapterInternal
    {
        std::string id;
        std::wstring name;
        std::wstring ipv4;
        std::unordered_set<std::string> local_ips;
    };

    NpcapApi g_api;
    std::atomic_bool g_running{false};
    std::thread g_thread;
    pcap_t* g_handle = nullptr;
    std::mutex g_lock;
    WS_PACKET_CALLBACK g_callback = nullptr;
    void* g_user = nullptr;
    std::unordered_set<std::string> g_local_ips;
    std::wstring g_last_error;

    constexpr int DLT_NULL = 0;
    constexpr int DLT_EN10MB = 1;

    void SetError(std::wstring value)
    {
        std::scoped_lock guard(g_lock);
        g_last_error = std::move(value);
    }

    std::wstring ToWide(const char* value)
    {
        if (!value || !*value) return {};
        int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
        UINT cp = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (needed <= 0)
        {
            cp = CP_ACP;
            flags = 0;
            needed = MultiByteToWideChar(cp, flags, value, -1, nullptr, 0);
        }
        if (needed <= 0) return {};
        std::wstring out(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(cp, flags, value, -1, out.data(), needed);
        if (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    }

    std::string SockaddrToString(const sockaddr* addr)
    {
        if (!addr) return {};
        char text[INET6_ADDRSTRLEN]{};
        if (addr->sa_family == AF_INET)
        {
            const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
            if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text))) return text;
        }
        else if (addr->sa_family == AF_INET6)
        {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(addr);
            if (inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text))) return text;
        }
        return {};
    }

    std::vector<AdapterInternal> EnumerateAdapters()
    {
        std::vector<AdapterInternal> result;
        if (!g_api.Load())
        {
            SetError(L"Npcap could not be loaded. Install Npcap and restart WireScope.");
            return result;
        }

        pcap_if_t* devices = nullptr;
        char errbuf[512]{};
        if (g_api.findalldevs(&devices, errbuf) != 0 || !devices)
        {
            SetError(L"Npcap could not enumerate capture adapters.");
            return result;
        }

        for (auto* dev = devices; dev; dev = dev->next)
        {
            if (!dev->name) continue;
            AdapterInternal item;
            item.id = dev->name;
            item.name = ToWide(dev->description);
            if (item.name.empty()) item.name = ToWide(dev->name);

            for (auto* a = dev->addresses; a; a = a->next)
            {
                const auto ip = SockaddrToString(a->addr);
                if (ip.empty()) continue;
                item.local_ips.insert(ip);
                if (item.ipv4.empty() && a->addr && a->addr->sa_family == AF_INET)
                    item.ipv4 = ToWide(ip.c_str());
            }
            if (item.ipv4.empty()) item.ipv4 = L"—";
            result.push_back(std::move(item));
        }

        g_api.freealldevs(devices);
        return result;
    }

    std::uint16_t ReadU16(const unsigned char* p)
    {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
    }

    bool ParsePacket(const unsigned char* data, std::uint32_t caplen, std::uint32_t wirelen, int linkType, WS_PACKET_EVENT& evt)
    {
        if (!data || caplen < 20) return false;
        std::size_t offset = 0;
        std::uint16_t etherType = 0;

        if (linkType == DLT_EN10MB)
        {
            if (caplen < 14) return false;
            etherType = ReadU16(data + 12);
            offset = 14;
            if (etherType == 0x8100 || etherType == 0x88A8)
            {
                if (caplen < 18) return false;
                etherType = ReadU16(data + 16);
                offset = 18;
            }
        }
        else if (linkType == DLT_NULL)
        {
            if (caplen < 4) return false;
            const auto family = *reinterpret_cast<const std::uint32_t*>(data);
            etherType = family == AF_INET6 ? 0x86DD : 0x0800;
            offset = 4;
        }
        else
        {
            return false;
        }

        std::string source;
        std::string destination;
        std::uint8_t protocol = 0;
        std::size_t transportOffset = 0;

        if (etherType == 0x0800)
        {
            if (caplen < offset + 20) return false;
            const auto* ip = data + offset;
            if ((ip[0] >> 4) != 4) return false;
            const auto ihl = static_cast<std::size_t>((ip[0] & 0x0F) * 4);
            if (ihl < 20 || caplen < offset + ihl) return false;
            protocol = ip[9];
            char src[INET_ADDRSTRLEN]{}, dst[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, ip + 12, src, sizeof(src));
            inet_ntop(AF_INET, ip + 16, dst, sizeof(dst));
            source = src;
            destination = dst;
            transportOffset = offset + ihl;
        }
        else if (etherType == 0x86DD)
        {
            if (caplen < offset + 40) return false;
            const auto* ip = data + offset;
            if ((ip[0] >> 4) != 6) return false;
            protocol = ip[6];
            char src[INET6_ADDRSTRLEN]{}, dst[INET6_ADDRSTRLEN]{};
            inet_ntop(AF_INET6, ip + 8, src, sizeof(src));
            inet_ntop(AF_INET6, ip + 24, dst, sizeof(dst));
            source = src;
            destination = dst;
            transportOffset = offset + 40;
        }
        else
        {
            return false;
        }

        std::uint16_t sourcePort = 0, destinationPort = 0;
        if ((protocol == WS_PROTOCOL_TCP || protocol == WS_PROTOCOL_UDP) && caplen >= transportOffset + 4)
        {
            sourcePort = ReadU16(data + transportOffset);
            destinationPort = ReadU16(data + transportOffset + 2);
        }

        const bool sourceLocal = g_local_ips.contains(source);
        const bool destinationLocal = g_local_ips.contains(destination);
        std::string remote;

        if (!sourceLocal && destinationLocal)
        {
            evt.direction = WS_DIRECTION_IN;
            remote = source;
            evt.remote_port = sourcePort;
            evt.local_port = destinationPort;
        }
        else if (sourceLocal && !destinationLocal)
        {
            evt.direction = WS_DIRECTION_OUT;
            remote = destination;
            evt.remote_port = destinationPort;
            evt.local_port = sourcePort;
        }
        else
        {
            evt.direction = sourceLocal ? WS_DIRECTION_LOCAL : WS_DIRECTION_SEEN;
            remote = sourceLocal ? destination : source;
            evt.remote_port = sourceLocal ? destinationPort : sourcePort;
            evt.local_port = sourceLocal ? sourcePort : destinationPort;
        }

        strncpy_s(evt.remote_ip, remote.c_str(), _TRUNCATE);
        evt.protocol = protocol;
        evt.packet_size = wirelen;
        return true;
    }

    void CaptureLoop()
    {
        const int linkType = g_api.datalink(g_handle);
        while (g_running.load(std::memory_order_relaxed))
        {
            pcap_pkthdr* header = nullptr;
            const unsigned char* data = nullptr;
            const int result = g_api.next_ex(g_handle, &header, &data);
            if (!g_running.load(std::memory_order_relaxed)) break;
            if (result == 0) continue;
            if (result < 0) break;
            if (!header || !data) continue;

            WS_PACKET_EVENT evt{};
            if (ParsePacket(data, header->caplen, header->len, linkType, evt) && g_callback)
                g_callback(&evt, g_user);
        }
        g_running.store(false, std::memory_order_relaxed);
    }
}

extern "C" BOOL __stdcall WSN_Initialize()
{
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        SetError(L"Winsock initialization failed.");
        return FALSE;
    }
    if (!g_api.Load())
    {
        SetError(L"Npcap could not be loaded. Make sure Npcap is installed.");
        return FALSE;
    }
    return TRUE;
}

extern "C" void __stdcall WSN_Shutdown()
{
    WSN_StopCapture();
    g_api.Unload();
    WSACleanup();
}

extern "C" int __stdcall WSN_GetAdapters(WS_ADAPTER_INFO* buffer, int capacity)
{
    const auto adapters = EnumerateAdapters();
    if (!buffer || capacity <= 0) return static_cast<int>(adapters.size());

    const int count = (std::min)(capacity, static_cast<int>(adapters.size()));
    for (int i = 0; i < count; ++i)
    {
        const auto& src = adapters[static_cast<std::size_t>(i)];
        auto& dst = buffer[i];
        const auto idWide = ToWide(src.id.c_str());
        wcsncpy_s(dst.id, idWide.c_str(), _TRUNCATE);
        wcsncpy_s(dst.name, src.name.c_str(), _TRUNCATE);
        wcsncpy_s(dst.ipv4, src.ipv4.c_str(), _TRUNCATE);
    }
    return count;
}

extern "C" BOOL __stdcall WSN_StartCapture(const wchar_t* adapter_id, WS_PACKET_CALLBACK callback, void* user_data)
{
    if (!adapter_id || !*adapter_id || !callback) return FALSE;
    WSN_StopCapture();

    const auto adapters = EnumerateAdapters();
    AdapterInternal selected;
    bool found = false;
    for (const auto& item : adapters)
    {
        if (_wcsicmp(ToWide(item.id.c_str()).c_str(), adapter_id) == 0)
        {
            selected = item;
            found = true;
            break;
        }
    }
    if (!found)
    {
        SetError(L"The selected capture adapter no longer exists.");
        return FALSE;
    }

    char errbuf[512]{};
    g_handle = g_api.open_live(selected.id.c_str(), 65535, 1, 250, errbuf);
    if (!g_handle)
    {
        SetError(L"Npcap could not open the selected adapter. Try running WireScope as Administrator.");
        return FALSE;
    }

    g_local_ips = std::move(selected.local_ips);
    g_callback = callback;
    g_user = user_data;
    g_running.store(true, std::memory_order_relaxed);
    g_thread = std::thread(CaptureLoop);
    return TRUE;
}

extern "C" void __stdcall WSN_StopCapture()
{
    g_running.store(false, std::memory_order_relaxed);
    if (g_thread.joinable()) g_thread.join();
    if (g_handle)
    {
        g_api.close(g_handle);
        g_handle = nullptr;
    }
    g_callback = nullptr;
    g_user = nullptr;
    g_local_ips.clear();
}

extern "C" BOOL __stdcall WSN_IsCapturing()
{
    return g_running.load(std::memory_order_relaxed) ? TRUE : FALSE;
}

extern "C" BOOL __stdcall WSN_GetLastError(wchar_t* buffer, int capacity)
{
    if (!buffer || capacity <= 0) return FALSE;
    std::scoped_lock guard(g_lock);
    wcsncpy_s(buffer, static_cast<std::size_t>(capacity), g_last_error.c_str(), _TRUNCATE);
    return TRUE;
}
