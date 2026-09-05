#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cwchar>
#include <cstdlib>
#include <ShlObj.h>
#include <filesystem>
#include <string>
#include <iterator>
#include "CoreApi.h"

#pragma comment(lib, "Shell32.lib")

namespace
{
    std::wstring GetConfigDir()
    {
        wchar_t path[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, path)))
            return L".";

        std::filesystem::path dir(path);
        dir /= L"WireScope";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir.wstring();
    }

    std::wstring GetIniPath()
    {
        return (std::filesystem::path(GetConfigDir()) / L"settings.ini").wstring();
    }

    DWORD ReadDword(const wchar_t* section, const wchar_t* key, DWORD fallback, const std::wstring& ini)
    {
        wchar_t value[64]{};
        const auto fallbackText = std::to_wstring(fallback);
        GetPrivateProfileStringW(section, key, fallbackText.c_str(), value, static_cast<DWORD>(std::size(value)), ini.c_str());
        wchar_t* end = nullptr;
        const auto parsed = wcstoul(value, &end, 10);
        return (end && *end == L'\0') ? static_cast<DWORD>(parsed) : fallback;
    }

    int ReadInt(const wchar_t* section, const wchar_t* key, int fallback, const std::wstring& ini)
    {
        return static_cast<int>(ReadDword(section, key, static_cast<DWORD>(fallback), ini));
    }

    void WriteDword(const wchar_t* section, const wchar_t* key, DWORD value, const std::wstring& ini)
    {
        const auto text = std::to_wstring(value);
        WritePrivateProfileStringW(section, key, text.c_str(), ini.c_str());
    }
}

extern "C" BOOL __stdcall WSC_LoadSettings(WS_SETTINGS* settings)
{
    if (!settings) return FALSE;

    *settings = WS_SETTINGS{};
    const auto ini = GetIniPath();

    settings->accent_color = static_cast<COLORREF>(ReadDword(L"Theme", L"AccentColor", settings->accent_color, ini));
    settings->text_color = static_cast<COLORREF>(ReadDword(L"Theme", L"TextColor", settings->text_color, ini));
    settings->background_color = static_cast<COLORREF>(ReadDword(L"Theme", L"BackgroundColor", settings->background_color, ini));
    settings->font_size = ReadInt(L"Theme", L"FontSize", settings->font_size, ini);
    settings->image_opacity = ReadInt(L"Theme", L"ImageOpacity", settings->image_opacity, ini);
    settings->geo_enabled = ReadInt(L"General", L"GeoEnabled", settings->geo_enabled, ini);

    GetPrivateProfileStringW(L"Theme", L"FontName", L"Segoe UI", settings->font_name,
        static_cast<DWORD>(std::size(settings->font_name)), ini.c_str());
    GetPrivateProfileStringW(L"Theme", L"BackgroundImage", L"", settings->background_image,
        static_cast<DWORD>(std::size(settings->background_image)), ini.c_str());

    if (settings->font_size < 8 || settings->font_size > 30) settings->font_size = 10;
    if (settings->image_opacity < 0 || settings->image_opacity > 100) settings->image_opacity = 35;
    settings->geo_enabled = settings->geo_enabled ? 1 : 0;
    return TRUE;
}

extern "C" BOOL __stdcall WSC_SaveSettings(const WS_SETTINGS* settings)
{
    if (!settings) return FALSE;
    const auto ini = GetIniPath();

    WriteDword(L"Theme", L"AccentColor", settings->accent_color, ini);
    WriteDword(L"Theme", L"TextColor", settings->text_color, ini);
    WriteDword(L"Theme", L"BackgroundColor", settings->background_color, ini);
    WriteDword(L"Theme", L"FontSize", static_cast<DWORD>(settings->font_size), ini);
    WriteDword(L"Theme", L"ImageOpacity", static_cast<DWORD>(settings->image_opacity), ini);
    WriteDword(L"General", L"GeoEnabled", static_cast<DWORD>(settings->geo_enabled), ini);
    WritePrivateProfileStringW(L"Theme", L"FontName", settings->font_name, ini.c_str());
    WritePrivateProfileStringW(L"Theme", L"BackgroundImage", settings->background_image, ini.c_str());
    return TRUE;
}

extern "C" BOOL __stdcall WSC_GetConfigDirectory(wchar_t* buffer, int capacity)
{
    if (!buffer || capacity <= 0) return FALSE;
    const auto dir = GetConfigDir();
    if (static_cast<int>(dir.size()) + 1 > capacity) return FALSE;
    wcsncpy_s(buffer, static_cast<std::size_t>(capacity), dir.c_str(), _TRUNCATE);
    return TRUE;
}
