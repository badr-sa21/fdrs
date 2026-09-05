#pragma once
#include "../include/WireScopeTypes.h"

#ifdef WIRESCOPE_GEO_EXPORTS
#define WSG_API __declspec(dllexport)
#else
#define WSG_API __declspec(dllimport)
#endif

extern "C"
{
    WSG_API BOOL __stdcall WSG_Lookup(const char* ip_address, WS_GEO_INFO* info);
    WSG_API void __stdcall WSG_ClearCache();
}
