#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <string>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "FiltersApi.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")

namespace
{
    std::mutex g_discord_lock;
    std::unordered_set<std::uint16_t> g_discord_udp_ports;
    std::chrono::steady_clock::time_point g_last_discord_refresh{};

    std::wstring BaseName(std::wstring path)
    {
        const auto pos = path.find_last_of(L"\\/");
        if (pos != std::wstring::npos) path.erase(0, pos + 1);
        std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return path;
    }

    bool IsDiscordPid(DWORD pid, std::unordered_map<DWORD, bool>& cache)
    {
        if (auto it = cache.find(pid); it != cache.end()) return it->second;

        bool result = false;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process)
        {
            wchar_t path[2048]{};
            DWORD size = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &size))
            {
                const auto name = BaseName(std::wstring(path, size));
                result = name == L"discord.exe" || name == L"discordcanary.exe" || name == L"discordptb.exe";
            }
            CloseHandle(process);
        }

        cache.emplace(pid, result);
        return result;
    }

    void RefreshDiscordPortsIfNeeded()
    {
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock guard(g_discord_lock);
        if (g_last_discord_refresh.time_since_epoch().count() != 0 && now - g_last_discord_refresh < std::chrono::milliseconds(1200))
            return;

        g_last_discord_refresh = now;
        g_discord_udp_ports.clear();
        std::unordered_map<DWORD, bool> pidCache;

        ULONG size = 0;
        if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER || size == 0)
            return;

        std::vector<std::byte> buffer(size);
        if (GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR)
        {
            const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i)
            {
                const auto& row = table->table[i];
                if (IsDiscordPid(row.dwOwningPid, pidCache))
                    g_discord_udp_ports.insert(ntohs(static_cast<u_short>(row.dwLocalPort)));
            }
        }

        size = 0;
        if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER && size > 0)
        {
            buffer.assign(size, std::byte{});
            if (GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == NO_ERROR)
            {
                const auto* table6 = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(buffer.data());
                for (DWORD i = 0; i < table6->dwNumEntries; ++i)
                {
                    const auto& row = table6->table[i];
                    if (IsDiscordPid(row.dwOwningPid, pidCache))
                        g_discord_udp_ports.insert(ntohs(static_cast<u_short>(row.dwLocalPort)));
                }
            }
        }
    }

    bool ParseIp(const char* text, IN_ADDR& out4, IN6_ADDR& out6, int& family)
    {
        family = 0;
        if (!text || !*text) return false;
        if (InetPtonA(AF_INET, text, &out4) == 1)
        {
            family = AF_INET;
            return true;
        }
        if (InetPtonA(AF_INET6, text, &out6) == 1)
        {
            family = AF_INET6;
            return true;
        }
        return false;
    }

    bool IsPrivateV4(std::uint32_t host)
    {
        const auto a = (host >> 24) & 0xFF;
        const auto b = (host >> 16) & 0xFF;
        return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
    }

    bool IsNoiseV4(std::uint32_t host)
    {
        const auto a = (host >> 24) & 0xFF;
        const auto b = (host >> 16) & 0xFF;
        if (host == 0 || host == 0xFFFFFFFFu) return true;
        if (a == 127) return true;
        if (a == 169 && b == 254) return true;
        if (a >= 224) return true;
        return false;
    }

    bool IsPrivateV6(const IN6_ADDR& a)
    {
        const auto* b = reinterpret_cast<const unsigned char*>(&a);
        return (b[0] & 0xFE) == 0xFC;
    }

    bool IsNoiseV6(const IN6_ADDR& a)
    {
        const auto* b = reinterpret_cast<const unsigned char*>(&a);
        bool allZero = true;
        for (int i = 0; i < 16; ++i) allZero &= b[i] == 0;
        if (allZero) return true;

        bool loopback = true;
        for (int i = 0; i < 15; ++i) loopback &= b[i] == 0;
        loopback &= b[15] == 1;
        if (loopback) return true;

        if (b[0] == 0xFF) return true; // multicast
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true; // link-local
        return false;
    }
}

extern "C" BOOL __stdcall WSF_IsPublicIp(const char* ip_address)
{
    IN_ADDR a4{};
    IN6_ADDR a6{};
    int family = 0;
    if (!ParseIp(ip_address, a4, a6, family)) return FALSE;

    if (family == AF_INET)
    {
        const auto host = ntohl(a4.S_un.S_addr);
        return (!IsPrivateV4(host) && !IsNoiseV4(host)) ? TRUE : FALSE;
    }
    return (!IsPrivateV6(a6) && !IsNoiseV6(a6)) ? TRUE : FALSE;
}

extern "C" BOOL __stdcall WSF_IsNoiseIp(const char* ip_address)
{
    IN_ADDR a4{};
    IN6_ADDR a6{};
    int family = 0;
    if (!ParseIp(ip_address, a4, a6, family)) return TRUE;
    if (family == AF_INET) return IsNoiseV4(ntohl(a4.S_un.S_addr)) ? TRUE : FALSE;
    return IsNoiseV6(a6) ? TRUE : FALSE;
}

extern "C" BOOL __stdcall WSF_IsDiscordPort(std::uint16_t local_port, std::uint8_t protocol)
{
    if (protocol != WS_PROTOCOL_UDP || local_port == 0) return FALSE;
    RefreshDiscordPortsIfNeeded();
    std::scoped_lock guard(g_discord_lock);
    return g_discord_udp_ports.contains(local_port) ? TRUE : FALSE;
}

extern "C" BOOL __stdcall WSF_IsPacketAllowed(const WS_PACKET_EVENT* event_data, const WS_FILTER_CONFIG* config)
{
    if (!event_data || !config) return FALSE;

    if (config->incoming_only && event_data->direction != WS_DIRECTION_IN) return FALSE;
    if (config->protocol != WS_PROTOCOL_ANY && event_data->protocol != config->protocol) return FALSE;
    if (config->hide_noise && WSF_IsNoiseIp(event_data->remote_ip)) return FALSE;
    if (config->public_only && !WSF_IsPublicIp(event_data->remote_ip)) return FALSE;
    if (config->discord_only && !WSF_IsDiscordPort(event_data->local_port, event_data->protocol)) return FALSE;

    if (config->port_count > 0)
    {
        bool matched = false;
        const auto count = (std::min)(config->port_count, static_cast<std::uint32_t>(WS_MAX_PORT_RANGES));
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& range = config->ports[i];
            if (event_data->remote_port >= range.start && event_data->remote_port <= range.end)
            {
                matched = true;
                break;
            }
        }
        if (config->invert_ports ? matched : !matched) return FALSE;
    }

    return TRUE;
}
