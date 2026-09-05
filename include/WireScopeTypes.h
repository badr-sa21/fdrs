#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstddef>

constexpr std::size_t WS_MAX_ADAPTER_ID = 512;
constexpr std::size_t WS_MAX_ADAPTER_NAME = 256;
constexpr std::size_t WS_MAX_IP = 64;
constexpr std::size_t WS_MAX_TEXT = 160;
constexpr std::size_t WS_MAX_PATH_CHARS = 520;
constexpr std::size_t WS_MAX_PORT_RANGES = 32;

enum WS_PROTOCOL : std::uint8_t
{
    WS_PROTOCOL_ANY = 0,
    WS_PROTOCOL_ICMP = 1,
    WS_PROTOCOL_TCP = 6,
    WS_PROTOCOL_UDP = 17,
    WS_PROTOCOL_ICMPV6 = 58
};

enum WS_DIRECTION : std::uint8_t
{
    WS_DIRECTION_SEEN = 0,
    WS_DIRECTION_IN = 1,
    WS_DIRECTION_OUT = 2,
    WS_DIRECTION_LOCAL = 3
};

struct WS_ADAPTER_INFO
{
    wchar_t id[WS_MAX_ADAPTER_ID]{};
    wchar_t name[WS_MAX_ADAPTER_NAME]{};
    wchar_t ipv4[WS_MAX_IP]{};
};

struct WS_PACKET_EVENT
{
    char remote_ip[WS_MAX_IP]{};
    std::uint16_t remote_port{};
    std::uint16_t local_port{};
    std::uint8_t protocol{};
    std::uint8_t direction{};
    std::uint32_t packet_size{};
};

struct WS_PORT_RANGE
{
    std::uint16_t start{};
    std::uint16_t end{};
};

struct WS_FILTER_CONFIG
{
    std::uint8_t protocol{WS_PROTOCOL_ANY};
    std::uint8_t incoming_only{1};
    std::uint8_t public_only{0};
    std::uint8_t hide_noise{1};
    std::uint8_t invert_ports{0};
    std::uint8_t discord_only{0};
    std::uint16_t reserved{};
    std::uint32_t port_count{};
    WS_PORT_RANGE ports[WS_MAX_PORT_RANGES]{};
};

struct WS_GEO_INFO
{
    wchar_t country_code[8]{};
    wchar_t country[96]{};
    wchar_t region[96]{};
    wchar_t city[96]{};
    wchar_t owner[WS_MAX_TEXT]{};
    wchar_t asn[32]{};
    wchar_t flag_path[WS_MAX_PATH_CHARS]{};
};

struct WS_SETTINGS
{
    COLORREF accent_color{RGB(255, 49, 88)};
    COLORREF text_color{RGB(242, 244, 248)};
    COLORREF background_color{RGB(9, 10, 15)};
    wchar_t font_name[LF_FACESIZE] = L"Segoe UI";
    int font_size{10};
    wchar_t background_image[WS_MAX_PATH_CHARS]{};
    int image_opacity{35};
    int geo_enabled{1};
};

using WS_PACKET_CALLBACK = void(__stdcall*)(const WS_PACKET_EVENT* event_data, void* user_data);
