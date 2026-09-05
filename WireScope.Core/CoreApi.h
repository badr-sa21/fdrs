#pragma once
#include "../include/WireScopeTypes.h"

#ifdef WIRESCOPE_CORE_EXPORTS
#define WSC_API __declspec(dllexport)
#else
#define WSC_API __declspec(dllimport)
#endif

extern "C"
{
    WSC_API BOOL __stdcall WSC_LoadSettings(WS_SETTINGS* settings);
    WSC_API BOOL __stdcall WSC_SaveSettings(const WS_SETTINGS* settings);
    WSC_API BOOL __stdcall WSC_GetConfigDirectory(wchar_t* buffer, int capacity);
}
