#pragma once
#include "../include/WireScopeTypes.h"

#ifdef WIRESCOPE_NETWORK_EXPORTS
#define WSN_API __declspec(dllexport)
#else
#define WSN_API __declspec(dllimport)
#endif

extern "C"
{
    WSN_API BOOL __stdcall WSN_Initialize();
    WSN_API void __stdcall WSN_Shutdown();
    WSN_API int __stdcall WSN_GetAdapters(WS_ADAPTER_INFO* buffer, int capacity);
    WSN_API BOOL __stdcall WSN_StartCapture(const wchar_t* adapter_id, WS_PACKET_CALLBACK callback, void* user_data);
    WSN_API void __stdcall WSN_StopCapture();
    WSN_API BOOL __stdcall WSN_IsCapturing();
    WSN_API BOOL __stdcall WSN_GetLastError(wchar_t* buffer, int capacity);
}
