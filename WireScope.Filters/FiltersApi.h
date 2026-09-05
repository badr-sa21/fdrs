#pragma once
#include "../include/WireScopeTypes.h"

#ifdef WIRESCOPE_FILTERS_EXPORTS
#define WSF_API __declspec(dllexport)
#else
#define WSF_API __declspec(dllimport)
#endif

extern "C"
{
    WSF_API BOOL __stdcall WSF_IsPacketAllowed(const WS_PACKET_EVENT* event_data, const WS_FILTER_CONFIG* config);
    WSF_API BOOL __stdcall WSF_IsPublicIp(const char* ip_address);
    WSF_API BOOL __stdcall WSF_IsNoiseIp(const char* ip_address);
    WSF_API BOOL __stdcall WSF_IsDiscordPort(std::uint16_t local_port, std::uint8_t protocol);
}
