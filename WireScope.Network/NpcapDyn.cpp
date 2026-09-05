#include "NpcapDyn.h"
#include <string>
#include <iterator>

namespace
{
    HMODULE TryLoadNpcap()
    {
        if (auto mod = LoadLibraryW(L"wpcap.dll")) return mod;

        wchar_t systemDir[MAX_PATH]{};
        if (GetSystemDirectoryW(systemDir, static_cast<UINT>(std::size(systemDir))))
        {
            std::wstring path(systemDir);
            path += L"\\Npcap\\wpcap.dll";
            if (auto mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return mod;
        }

        wchar_t windowsDir[MAX_PATH]{};
        if (GetWindowsDirectoryW(windowsDir, static_cast<UINT>(std::size(windowsDir))))
        {
            std::wstring path(windowsDir);
            path += L"\\System32\\Npcap\\wpcap.dll";
            if (auto mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return mod;
        }

        return nullptr;
    }
}

bool NpcapApi::Load()
{
    if (module) return true;
    module = TryLoadNpcap();
    if (!module) return false;

#define WS_LOAD_PROC(name) reinterpret_cast<decltype(name)>(GetProcAddress(module, #name))
    findalldevs = WS_LOAD_PROC(findalldevs);
    freealldevs = WS_LOAD_PROC(freealldevs);
    open_live = WS_LOAD_PROC(open_live);
    next_ex = WS_LOAD_PROC(next_ex);
    close = WS_LOAD_PROC(close);
    datalink = WS_LOAD_PROC(datalink);
#undef WS_LOAD_PROC

    // Export names in wpcap.dll use the pcap_ prefix.
    if (!findalldevs) findalldevs = reinterpret_cast<decltype(findalldevs)>(GetProcAddress(module, "pcap_findalldevs"));
    if (!freealldevs) freealldevs = reinterpret_cast<decltype(freealldevs)>(GetProcAddress(module, "pcap_freealldevs"));
    if (!open_live) open_live = reinterpret_cast<decltype(open_live)>(GetProcAddress(module, "pcap_open_live"));
    if (!next_ex) next_ex = reinterpret_cast<decltype(next_ex)>(GetProcAddress(module, "pcap_next_ex"));
    if (!close) close = reinterpret_cast<decltype(close)>(GetProcAddress(module, "pcap_close"));
    if (!datalink) datalink = reinterpret_cast<decltype(datalink)>(GetProcAddress(module, "pcap_datalink"));

    if (!findalldevs || !freealldevs || !open_live || !next_ex || !close || !datalink)
    {
        Unload();
        return false;
    }

    return true;
}

void NpcapApi::Unload()
{
    findalldevs = nullptr;
    freealldevs = nullptr;
    open_live = nullptr;
    next_ex = nullptr;
    close = nullptr;
    datalink = nullptr;
    if (module)
    {
        FreeLibrary(module);
        module = nullptr;
    }
}
