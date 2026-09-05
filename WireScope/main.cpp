#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <CommCtrl.h>
#include <CommDlg.h>
#include <Dwmapi.h>
#include <Uxtheme.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <iterator>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../include/WireScopeTypes.h"
#include "../WireScope.Core/CoreApi.h"
#include "../WireScope.Network/NetworkApi.h"
#include "../WireScope.Filters/FiltersApi.h"
#include "../WireScope.Geo/GeoApi.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Gdiplus.lib")

using namespace Gdiplus;

namespace
{
    constexpr wchar_t MAIN_CLASS[] = L"WireScope.Native.Main";
    constexpr wchar_t SETTINGS_CLASS[] = L"WireScope.Native.Settings";
    constexpr wchar_t FILTER_CLASS[] = L"WireScope.Native.Filters";

    constexpr UINT WM_APP_GEO = WM_APP + 1;
    constexpr UINT TIMER_FLUSH = 1;

    enum ControlId
    {
        IDC_ADAPTER = 1001,
        IDC_PRESET,
        IDC_SEARCH,
        IDC_START,
        IDC_STOP,
        IDC_FILTERS,
        IDC_SETTINGS,
        IDC_EXPORT,
        IDC_CLEAR,
        IDC_LIST,

        IDC_S_ACCENT = 2001,
        IDC_S_TEXT,
        IDC_S_BG,
        IDC_S_FONT,
        IDC_S_IMAGE,
        IDC_S_REMOVE_IMAGE,
        IDC_S_OPACITY,
        IDC_S_GEO,
        IDC_S_SAVE,
        IDC_S_RESET,

        IDC_F_INCOMING = 3001,
        IDC_F_PUBLIC,
        IDC_F_NOISE,
        IDC_F_INVERT,
        IDC_F_PORTS,
        IDC_F_SAVE,
        IDC_F_RESET
    };

    struct TrafficRow
    {
        std::string key;
        std::string ip;
        std::uint16_t remotePort{};
        std::uint8_t protocol{};
        std::uint64_t packets{};
        std::uint64_t bytes{};
        std::chrono::system_clock::time_point firstSeen{};
        std::chrono::system_clock::time_point lastSeen{};
        WS_GEO_INFO geo{};
        bool geoReady{};
        bool geoPending{};
    };

    struct GeoMessage
    {
        std::uint64_t generation{};
        std::size_t rowIndex{};
        bool ok{};
        WS_GEO_INFO info{};
    };

    struct AppState
    {
        HINSTANCE instance{};
        HWND hwnd{};
        HWND adapterCombo{};
        HWND presetCombo{};
        HWND searchEdit{};
        HWND startButton{};
        HWND stopButton{};
        HWND filterButton{};
        HWND settingsButton{};
        HWND exportButton{};
        HWND clearButton{};
        HWND list{};
        HWND settingsWindow{};
        HWND filterWindow{};

        WS_SETTINGS settings{};
        WS_FILTER_CONFIG filter{};
        std::mutex filterMutex;
        std::vector<WS_ADAPTER_INFO> adapters;
        std::deque<WS_PACKET_EVENT> pending;
        std::mutex pendingMutex;
        std::vector<std::unique_ptr<TrafficRow>> rows;
        std::unordered_map<std::string, std::size_t> rowIndex;
        std::uint64_t totalPackets{};
        std::uint64_t generation{1};
        bool capturing{};
        std::wstring status{L"Idle"};

        HFONT font{};
        HBRUSH bgBrush{};
        HBRUSH editBrush{};
        HIMAGELIST flagImages{};
        std::unordered_map<std::wstring, int> flagImageIndex;
        std::unique_ptr<Image> backgroundArt;
    };

    ULONG_PTR g_gdiplusToken{};

    COLORREF Mix(COLORREF a, COLORREF b, double t)
    {
        const auto blend = [t](BYTE x, BYTE y) -> BYTE
        {
            return static_cast<BYTE>(x + (y - x) * t);
        };
        return RGB(blend(GetRValue(a), GetRValue(b)), blend(GetGValue(a), GetGValue(b)), blend(GetBValue(a), GetBValue(b)));
    }

    std::wstring ToWide(const std::string& value)
    {
        if (value.empty()) return {};
        const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0) return {};
        std::wstring out(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), count);
        return out;
    }

    std::string ToUtf8(const std::wstring& value)
    {
        if (value.empty()) return {};
        const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0) return {};
        std::string out(static_cast<std::size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), count, nullptr, nullptr);
        return out;
    }

    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return value;
    }

    std::wstring FormatTime(const std::chrono::system_clock::time_point& point)
    {
        const auto t = std::chrono::system_clock::to_time_t(point);
        tm local{};
        localtime_s(&local, &t);
        wchar_t text[32]{};
        wcsftime(text, std::size(text), L"%H:%M:%S", &local);
        return text;
    }

    std::wstring FormatBytes(std::uint64_t bytes)
    {
        wchar_t text[64]{};
        if (bytes >= 1024ull * 1024ull * 1024ull)
            swprintf_s(text, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
        else if (bytes >= 1024ull * 1024ull)
            swprintf_s(text, L"%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        else if (bytes >= 1024ull)
            swprintf_s(text, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
        else
            swprintf_s(text, L"%llu B", static_cast<unsigned long long>(bytes));
        return text;
    }

    std::wstring ProtocolText(std::uint8_t protocol)
    {
        switch (protocol)
        {
        case WS_PROTOCOL_TCP: return L"TCP";
        case WS_PROTOCOL_UDP: return L"UDP";
        case WS_PROTOCOL_ICMP:
        case WS_PROTOCOL_ICMPV6: return L"ICMP";
        default: return std::to_wstring(protocol);
        }
    }

    std::wstring LocationText(const WS_GEO_INFO& geo)
    {
        std::wstring out;
        const wchar_t* parts[] = {geo.city, geo.region, geo.country};
        for (const auto* p : parts)
        {
            if (!p || !*p) continue;
            if (!out.empty()) out += L", ";
            out += p;
        }
        return out.empty() ? L"—" : out;
    }

    std::wstring OwnerText(const WS_GEO_INFO& geo)
    {
        std::wstring out = geo.owner;
        if (*geo.asn)
        {
            if (!out.empty()) out += L"  •  ";
            out += geo.asn;
        }
        return out.empty() ? L"—" : out;
    }

    void SetDarkTitleBar(HWND hwnd)
    {
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
            DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }

    void ApplyDwmTheme(HWND hwnd, const WS_SETTINGS& settings)
    {
        if (!hwnd) return;
        SetDarkTitleBar(hwnd);
        // Windows 11: border/caption/text colors. Unsupported attributes are simply ignored on older builds.
        DwmSetWindowAttribute(hwnd, 34, &settings.accent_color, sizeof(settings.accent_color));
        DwmSetWindowAttribute(hwnd, 35, &settings.background_color, sizeof(settings.background_color));
        DwmSetWindowAttribute(hwnd, 36, &settings.text_color, sizeof(settings.text_color));
    }

    HWND MakeButton(HWND parent, const wchar_t* text, int id)
    {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 90, 36, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    }

    void DeleteUiObjects(AppState& s)
    {
        if (s.font) { DeleteObject(s.font); s.font = nullptr; }
        if (s.bgBrush) { DeleteObject(s.bgBrush); s.bgBrush = nullptr; }
        if (s.editBrush) { DeleteObject(s.editBrush); s.editBrush = nullptr; }
    }

    void LoadBackgroundArt(AppState& s)
    {
        s.backgroundArt.reset();
        if (*s.settings.background_image && std::filesystem::exists(s.settings.background_image))
        {
            auto image = std::make_unique<Image>(s.settings.background_image);
            if (image->GetLastStatus() == Ok) s.backgroundArt = std::move(image);
        }
    }

    void ApplyTheme(AppState& s)
    {
        DeleteUiObjects(s);
        s.bgBrush = CreateSolidBrush(s.settings.background_color);
        s.editBrush = CreateSolidBrush(Mix(s.settings.background_color, RGB(255, 255, 255), 0.06));

        HDC dc = GetDC(s.hwnd);
        const int dpiY = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(s.hwnd, dc);
        const int height = -MulDiv(s.settings.font_size, dpiY, 72);
        s.font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            s.settings.font_name);

        EnumChildWindows(s.hwnd, [](HWND child, LPARAM lp) -> BOOL
        {
            auto* state = reinterpret_cast<AppState*>(lp);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&s));

        if (s.list)
        {
            const auto panel = Mix(s.settings.background_color, RGB(255, 255, 255), 0.035);
            ListView_SetBkColor(s.list, panel);
            ListView_SetTextBkColor(s.list, panel);
            ListView_SetTextColor(s.list, s.settings.text_color);
            SetWindowTheme(s.list, L"DarkMode_Explorer", nullptr);
            if (auto header = ListView_GetHeader(s.list)) SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
        }

        LoadBackgroundArt(s);
        ApplyDwmTheme(s.hwnd, s.settings);
        if (s.settingsWindow && IsWindow(s.settingsWindow)) ApplyDwmTheme(s.settingsWindow, s.settings);
        if (s.filterWindow && IsWindow(s.filterWindow)) ApplyDwmTheme(s.filterWindow, s.settings);
        InvalidateRect(s.hwnd, nullptr, TRUE);
    }

    void DrawOwnerButton(const DRAWITEMSTRUCT* dis, AppState& s)
    {
        if (!dis || dis->CtlType != ODT_BUTTON) return;
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        RECT r = dis->rcItem;
        const auto base = Mix(s.settings.background_color, RGB(255, 255, 255), pressed ? 0.14 : 0.08);
        HBRUSH fill = CreateSolidBrush(base);
        HPEN pen = CreatePen(PS_SOLID, 1, s.settings.accent_color);
        const auto oldBrush = SelectObject(dis->hDC, fill);
        const auto oldPen = SelectObject(dis->hDC, pen);
        RoundRect(dis->hDC, r.left, r.top, r.right, r.bottom, 10, 10);
        SelectObject(dis->hDC, oldBrush);
        SelectObject(dis->hDC, oldPen);
        DeleteObject(fill);
        DeleteObject(pen);

        wchar_t text[128]{};
        GetWindowTextW(dis->hwndItem, text, static_cast<int>(std::size(text)));
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, disabled ? Mix(s.settings.text_color, s.settings.background_color, 0.55) : s.settings.text_color);
        const auto oldFont = SelectObject(dis->hDC, s.font);
        DrawTextW(dis->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dis->hDC, oldFont);
    }

    void DrawOwnerCombo(const DRAWITEMSTRUCT* dis, AppState& s)
    {
        if (!dis || dis->CtlType != ODT_COMBOBOX) return;
        HBRUSH fill = CreateSolidBrush(Mix(s.settings.background_color, RGB(255, 255, 255),
            (dis->itemState & ODS_SELECTED) ? 0.13 : 0.06));
        FillRect(dis->hDC, &dis->rcItem, fill);
        DeleteObject(fill);

        wchar_t text[512]{};
        if (dis->itemID != static_cast<UINT>(-1))
            SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, reinterpret_cast<LPARAM>(text));
        else
            GetWindowTextW(dis->hwndItem, text, static_cast<int>(std::size(text)));

        RECT r = dis->rcItem;
        r.left += 8;
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, s.settings.text_color);
        const auto oldFont = SelectObject(dis->hDC, s.font);
        DrawTextW(dis->hDC, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dis->hDC, oldFont);
    }

    HBITMAP MakeFlagBitmap(const wchar_t* path)
    {
        if (!path || !*path || !std::filesystem::exists(path)) return nullptr;
        Bitmap source(path);
        if (source.GetLastStatus() != Ok) return nullptr;
        Bitmap scaled(24, 18, PixelFormat32bppARGB);
        Graphics graphics(&scaled);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.DrawImage(&source, Rect(0, 0, 24, 18), 0, 0,
            static_cast<INT>(source.GetWidth()), static_cast<INT>(source.GetHeight()), UnitPixel);
        HBITMAP bitmap = nullptr;
        if (scaled.GetHBITMAP(Color(0, 0, 0, 0), &bitmap) != Ok) return nullptr;
        return bitmap;
    }

    int FlagImageIndex(AppState& s, const wchar_t* path)
    {
        if (!path || !*path) return 0;
        std::wstring key(path);
        if (const auto it = s.flagImageIndex.find(key); it != s.flagImageIndex.end()) return it->second;
        HBITMAP bitmap = MakeFlagBitmap(path);
        if (!bitmap) return 0;
        const int index = ImageList_Add(s.flagImages, bitmap, nullptr);
        DeleteObject(bitmap);
        if (index >= 0) s.flagImageIndex.emplace(std::move(key), index);
        return index >= 0 ? index : 0;
    }

    bool RowMatchesSearch(const TrafficRow& row, const std::wstring& search)
    {
        if (search.empty()) return true;
        std::wstring hay = ToWide(row.ip) + L" " + ProtocolText(row.protocol) + L" " +
            std::to_wstring(row.remotePort) + L" " + LocationText(row.geo) + L" " + OwnerText(row.geo);
        return Lower(std::move(hay)).find(search) != std::wstring::npos;
    }

    void RefreshList(AppState& s)
    {
        if (!s.list) return;
        wchar_t searchText[256]{};
        GetWindowTextW(s.searchEdit, searchText, static_cast<int>(std::size(searchText)));
        const auto search = Lower(searchText);

        SendMessageW(s.list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(s.list);

        int visibleIndex = 0;
        for (std::size_t i = 0; i < s.rows.size(); ++i)
        {
            const auto& row = *s.rows[i];
            if (!RowMatchesSearch(row, search)) continue;

            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            item.iItem = visibleIndex++;
            item.iSubItem = 0;
            item.pszText = const_cast<LPWSTR>(L"");
            item.lParam = static_cast<LPARAM>(i);
            item.iImage = row.geoReady ? FlagImageIndex(s, row.geo.flag_path) : 0;
            const int idx = ListView_InsertItem(s.list, &item);

            auto setText = [&](int sub, const std::wstring& value)
            {
                ListView_SetItemText(s.list, idx, sub, const_cast<LPWSTR>(value.c_str()));
            };

            setText(1, ToWide(row.ip));
            setText(2, WSF_IsPublicIp(row.ip.c_str()) ? L"PUBLIC" : L"LAN");
            setText(3, ProtocolText(row.protocol));
            setText(4, row.geoReady ? LocationText(row.geo) : L"Looking up…");
            setText(5, row.geoReady ? OwnerText(row.geo) : L"—");
            setText(6, std::to_wstring(row.remotePort));
            setText(7, std::to_wstring(row.packets));
            setText(8, FormatBytes(row.bytes));
            setText(9, FormatTime(row.firstSeen));
            setText(10, FormatTime(row.lastSeen));
        }

        SendMessageW(s.list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(s.list, nullptr, TRUE);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }

    void StartGeoLookup(AppState& s, std::size_t index)
    {
        if (!s.settings.geo_enabled || index >= s.rows.size()) return;
        auto& row = *s.rows[index];
        if (row.geoPending || row.geoReady || !WSF_IsPublicIp(row.ip.c_str())) return;
        row.geoPending = true;
        const auto ip = row.ip;
        const auto generation = s.generation;
        const HWND hwnd = s.hwnd;

        std::thread([hwnd, index, generation, ip]
        {
            auto* message = new GeoMessage{};
            message->generation = generation;
            message->rowIndex = index;
            message->ok = WSG_Lookup(ip.c_str(), &message->info) == TRUE;
            if (IsWindow(hwnd)) PostMessageW(hwnd, WM_APP_GEO, 0, reinterpret_cast<LPARAM>(message));
            else delete message;
        }).detach();
    }

    void ProcessPacket(AppState& s, const WS_PACKET_EVENT& evt)
    {
        const auto now = std::chrono::system_clock::now();
        std::string key = evt.remote_ip;
        key += '|';
        key += std::to_string(evt.protocol);
        key += '|';
        key += std::to_string(evt.remote_port);

        auto it = s.rowIndex.find(key);
        std::size_t index = 0;
        if (it == s.rowIndex.end())
        {
            auto row = std::make_unique<TrafficRow>();
            row->key = key;
            row->ip = evt.remote_ip;
            row->remotePort = evt.remote_port;
            row->protocol = evt.protocol;
            row->packets = 1;
            row->bytes = evt.packet_size;
            row->firstSeen = now;
            row->lastSeen = now;
            index = s.rows.size();
            s.rowIndex.emplace(key, index);
            s.rows.push_back(std::move(row));
            StartGeoLookup(s, index);
        }
        else
        {
            index = it->second;
            auto& row = *s.rows[index];
            ++row.packets;
            row.bytes += evt.packet_size;
            row.lastSeen = now;
        }
        ++s.totalPackets;
    }

    void DrainPending(AppState& s)
    {
        std::deque<WS_PACKET_EVENT> batch;
        {
            std::scoped_lock guard(s.pendingMutex);
            batch.swap(s.pending);
        }
        if (batch.empty()) return;
        for (const auto& evt : batch) ProcessPacket(s, evt);
        RefreshList(s);
    }

    void __stdcall CaptureCallback(const WS_PACKET_EVENT* evt, void* user)
    {
        if (!evt || !user) return;
        auto& s = *reinterpret_cast<AppState*>(user);
        WS_FILTER_CONFIG config{};
        {
            std::scoped_lock guard(s.filterMutex);
            config = s.filter;
        }
        if (!WSF_IsPacketAllowed(evt, &config)) return;

        std::scoped_lock guard(s.pendingMutex);
        if (s.pending.size() < 25000) s.pending.push_back(*evt);
    }

    void AddColumn(HWND list, int index, const wchar_t* text, int width)
    {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(text);
        col.cx = width;
        col.iSubItem = index;
        ListView_InsertColumn(list, index, &col);
    }

    void LoadAdapters(AppState& s)
    {
        const int count = WSN_GetAdapters(nullptr, 0);
        s.adapters.assign(count > 0 ? static_cast<std::size_t>(count) : 0, WS_ADAPTER_INFO{});
        if (count > 0) WSN_GetAdapters(s.adapters.data(), count);
        SendMessageW(s.adapterCombo, CB_RESETCONTENT, 0, 0);
        for (const auto& a : s.adapters)
        {
            std::wstring text = a.name;
            text += L"  —  IPv4: ";
            text += a.ipv4;
            SendMessageW(s.adapterCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        if (!s.adapters.empty()) SendMessageW(s.adapterCombo, CB_SETCURSEL, 0, 0);
    }

    void ApplyPreset(AppState& s, int index)
    {
        std::scoped_lock guard(s.filterMutex);
        s.filter.protocol = WS_PROTOCOL_ANY;
        s.filter.discord_only = 0;
        s.filter.port_count = 0;
        s.filter.invert_ports = 0;
        switch (index)
        {
        case 1: s.filter.protocol = WS_PROTOCOL_TCP; break;
        case 2: s.filter.protocol = WS_PROTOCOL_UDP; break;
        case 3:
            s.filter.protocol = WS_PROTOCOL_UDP;
            s.filter.discord_only = 1;
            break;
        case 4:
            s.filter.protocol = WS_PROTOCOL_TCP;
            s.filter.port_count = 1;
            s.filter.ports[0] = {80, 80};
            break;
        case 5:
            s.filter.protocol = WS_PROTOCOL_TCP;
            s.filter.port_count = 1;
            s.filter.ports[0] = {443, 443};
            break;
        case 6:
            s.filter.protocol = WS_PROTOCOL_ANY;
            s.filter.port_count = 1;
            s.filter.ports[0] = {53, 53};
            break;
        case 7:
            s.filter.protocol = WS_PROTOCOL_UDP;
            s.filter.port_count = 1;
            s.filter.ports[0] = {443, 443};
            break;
        default: break;
        }
    }

    bool ParsePortRules(const std::wstring& text, WS_FILTER_CONFIG& config)
    {
        config.port_count = 0;
        std::wstringstream stream(text);
        std::wstring token;
        while (std::getline(stream, token, L','))
        {
            token.erase(std::remove_if(token.begin(), token.end(), [](wchar_t c) { return iswspace(c) != 0; }), token.end());
            if (token.empty()) continue;
            if (config.port_count >= WS_MAX_PORT_RANGES) return false;
            const auto dash = token.find(L'-');
            try
            {
                int start = 0, end = 0;
                if (dash == std::wstring::npos)
                    start = end = std::stoi(token);
                else
                {
                    start = std::stoi(token.substr(0, dash));
                    end = std::stoi(token.substr(dash + 1));
                }
                if (start < 1 || end < 1 || start > 65535 || end > 65535 || start > end) return false;
                config.ports[config.port_count++] = {static_cast<std::uint16_t>(start), static_cast<std::uint16_t>(end)};
            }
            catch (...) { return false; }
        }
        return true;
    }

    std::wstring PortRulesText(const WS_FILTER_CONFIG& config)
    {
        std::wstring out;
        for (std::uint32_t i = 0; i < config.port_count && i < WS_MAX_PORT_RANGES; ++i)
        {
            if (!out.empty()) out += L", ";
            out += std::to_wstring(config.ports[i].start);
            if (config.ports[i].end != config.ports[i].start)
            {
                out += L"-";
                out += std::to_wstring(config.ports[i].end);
            }
        }
        return out;
    }

    void ClipboardText(HWND hwnd, const std::wstring& text)
    {
        if (!OpenClipboard(hwnd)) return;
        EmptyClipboard();
        const auto bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem)
        {
            auto* dest = static_cast<wchar_t*>(GlobalLock(mem));
            memcpy(dest, text.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        CloseClipboard();
    }

    std::string Csv(const std::wstring& text)
    {
        std::wstring escaped = text;
        std::size_t pos = 0;
        while ((pos = escaped.find(L'"', pos)) != std::wstring::npos)
        {
            escaped.insert(pos, 1, L'"');
            pos += 2;
        }
        return '"' + ToUtf8(escaped) + '"';
    }

    void ExportCsv(AppState& s)
    {
        wchar_t file[MAX_PATH]{L"WireScope.csv"};
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = s.hwnd;
        ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = static_cast<DWORD>(std::size(file));
        ofn.lpstrDefExt = L"csv";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&ofn)) return;

        std::ofstream out(std::filesystem::path(file), std::ios::binary);
        if (!out) return;
        out << "Remote IP,Type,Protocol,Location,Owner/ASN,Remote Port,Packets,Bytes,First Seen,Last Seen\r\n";
        for (const auto& p : s.rows)
        {
            const auto& row = *p;
            out << Csv(ToWide(row.ip)) << ','
                << Csv(WSF_IsPublicIp(row.ip.c_str()) ? L"PUBLIC" : L"LAN") << ','
                << Csv(ProtocolText(row.protocol)) << ','
                << Csv(row.geoReady ? LocationText(row.geo) : L"") << ','
                << Csv(row.geoReady ? OwnerText(row.geo) : L"") << ','
                << row.remotePort << ',' << row.packets << ',' << row.bytes << ','
                << Csv(FormatTime(row.firstSeen)) << ',' << Csv(FormatTime(row.lastSeen)) << "\r\n";
        }
    }

    COLORREF PickColor(HWND owner, COLORREF initial)
    {
        static COLORREF custom[16]{};
        CHOOSECOLORW cc{sizeof(cc)};
        cc.hwndOwner = owner;
        cc.rgbResult = initial;
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        return ChooseColorW(&cc) ? cc.rgbResult : initial;
    }

    void PickFont(HWND owner, WS_SETTINGS& settings)
    {
        LOGFONTW lf{};
        wcsncpy_s(lf.lfFaceName, settings.font_name, _TRUNCATE);
        HDC dc = GetDC(owner);
        lf.lfHeight = -MulDiv(settings.font_size, GetDeviceCaps(dc, LOGPIXELSY), 72);
        ReleaseDC(owner, dc);

        CHOOSEFONTW cf{sizeof(cf)};
        cf.hwndOwner = owner;
        cf.lpLogFont = &lf;
        cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;
        cf.iPointSize = settings.font_size * 10;
        if (ChooseFontW(&cf))
        {
            wcsncpy_s(settings.font_name, lf.lfFaceName, _TRUNCATE);
            settings.font_size = (std::max)(8, cf.iPointSize / 10);
        }
    }

    void PickImage(HWND owner, WS_SETTINGS& settings)
    {
        wchar_t file[WS_MAX_PATH_CHARS]{};
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All files (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = static_cast<DWORD>(std::size(file));
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) wcsncpy_s(settings.background_image, file, _TRUNCATE);
    }

    void PaintWindowBackground(AppState& s, HDC dc, const RECT& client)
    {
        HBRUSH brush = s.bgBrush ? s.bgBrush : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        FillRect(dc, &client, brush);

        SetBkMode(dc, TRANSPARENT);
        const auto oldFont = SelectObject(dc, s.font);
        SetTextColor(dc, s.settings.text_color);
        RECT title{20, 14, client.right - 20, 52};
        DrawTextW(dc, L"WireScope", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(dc, Mix(s.settings.text_color, s.settings.background_color, 0.45));
        RECT subtitle{20, 48, client.right - 20, 76};
        DrawTextW(dc, L"Incoming IP Monitor  •  Native C++", -1, &subtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (s.backgroundArt)
        {
            Graphics g(dc);
            const int w = 320, h = 72;
            const int x = (std::max)(20, client.right - w - 20);
            ImageAttributes attrs;
            ColorMatrix matrix = {{1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,s.settings.image_opacity / 100.0f,0},{0,0,0,0,1}};
            attrs.SetColorMatrix(&matrix);
            g.DrawImage(s.backgroundArt.get(), Rect(x, 8, w, h), 0, 0,
                static_cast<INT>(s.backgroundArt->GetWidth()), static_cast<INT>(s.backgroundArt->GetHeight()), UnitPixel, &attrs);
        }

        SetTextColor(dc, Mix(s.settings.text_color, s.settings.background_color, 0.42));
        RECT footer{20, client.bottom - 34, client.right - 20, client.bottom - 8};
        const std::wstring footerText = L"Made by Exbadr  •  Discord: 5c5b     |     " + s.status +
            L"     |     " + std::to_wstring(s.rows.size()) + L" rows     |     " + std::to_wstring(s.totalPackets) + L" packets";
        DrawTextW(dc, footerText.c_str(), -1, &footer, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
    }

    void LayoutMain(AppState& s, int width, int height)
    {
        const int margin = 18;
        const int y = 90;
        const int h = 38;
        int x = margin;
        const int adapterW = (std::max)(230, width / 5);
        const int presetW = 125;
        const int searchW = (std::max)(150, width / 7);
        MoveWindow(s.adapterCombo, x, y, adapterW, h, TRUE); x += adapterW + 8;
        MoveWindow(s.presetCombo, x, y, presetW, h, TRUE); x += presetW + 8;
        MoveWindow(s.searchEdit, x, y, searchW, h, TRUE); x += searchW + 8;
        const int buttonW = 82;
        const HWND buttons[] = {s.filterButton, s.settingsButton, s.startButton, s.stopButton, s.exportButton, s.clearButton};
        for (HWND button : buttons)
        {
            MoveWindow(button, x, y, buttonW, h, TRUE);
            x += buttonW + 7;
        }
        MoveWindow(s.list, margin, y + h + 12, width - margin * 2,
            (std::max)(80, height - (y + h + 12) - 48), TRUE);
    }

    LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            s = reinterpret_cast<AppState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        }
        if (!s) return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg)
        {
        case WM_CREATE:
        {
            ApplyDwmTheme(hwnd, s->settings);
            auto makeStatic = [&](const wchar_t* text, int x, int y, int w, int h)
            {
                HWND c = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, hwnd, nullptr, s->instance, nullptr);
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(s->font), TRUE);
            };
            makeStatic(L"Appearance", 18, 18, 250, 24);
            MakeButton(hwnd, L"Accent color", IDC_S_ACCENT);
            MakeButton(hwnd, L"Text color", IDC_S_TEXT);
            MakeButton(hwnd, L"Background", IDC_S_BG);
            MakeButton(hwnd, L"Choose font", IDC_S_FONT);
            MakeButton(hwnd, L"Choose image", IDC_S_IMAGE);
            MakeButton(hwnd, L"Remove image", IDC_S_REMOVE_IMAGE);
            makeStatic(L"Image opacity (0-100)", 18, 208, 180, 22);
            HWND opacity = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(s->settings.image_opacity).c_str(),
                WS_CHILD | WS_VISIBLE | ES_NUMBER, 205, 204, 80, 28, hwnd, reinterpret_cast<HMENU>(IDC_S_OPACITY), s->instance, nullptr);
            SendMessageW(opacity, WM_SETFONT, reinterpret_cast<WPARAM>(s->font), TRUE);
            HWND geo = CreateWindowW(L"BUTTON", L"Enable IP location / ASN lookup", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                18, 246, 285, 28, hwnd, reinterpret_cast<HMENU>(IDC_S_GEO), s->instance, nullptr);
            SendMessageW(geo, BM_SETCHECK, s->settings.geo_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(geo, WM_SETFONT, reinterpret_cast<WPARAM>(s->font), TRUE);
            makeStatic(L"About", 18, 294, 250, 24);
            makeStatic(L"Made by Exbadr", 18, 323, 250, 22);
            makeStatic(L"Discord: 5c5b", 18, 347, 250, 22);
            MakeButton(hwnd, L"Reset", IDC_S_RESET);
            MakeButton(hwnd, L"Save", IDC_S_SAVE);

            MoveWindow(GetDlgItem(hwnd, IDC_S_ACCENT), 18, 54, 125, 36, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_S_TEXT), 151, 54, 125, 36, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_S_BG), 284, 54, 125, 36, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_S_FONT), 18, 102, 125, 36, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_S_IMAGE), 151, 102, 125, 36, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_S_REMOVE_IMAGE), 284, 102, 125, 36, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_S_RESET), 233, 394, 84, 36, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_S_SAVE), 325, 394, 84, 36, TRUE);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case IDC_S_ACCENT: s->settings.accent_color = PickColor(hwnd, s->settings.accent_color); InvalidateRect(hwnd, nullptr, TRUE); break;
            case IDC_S_TEXT: s->settings.text_color = PickColor(hwnd, s->settings.text_color); InvalidateRect(hwnd, nullptr, TRUE); break;
            case IDC_S_BG: s->settings.background_color = PickColor(hwnd, s->settings.background_color); InvalidateRect(hwnd, nullptr, TRUE); break;
            case IDC_S_FONT: PickFont(hwnd, s->settings); ApplyTheme(*s); break;
            case IDC_S_IMAGE: PickImage(hwnd, s->settings); LoadBackgroundArt(*s); InvalidateRect(s->hwnd, nullptr, TRUE); break;
            case IDC_S_REMOVE_IMAGE: s->settings.background_image[0] = 0; LoadBackgroundArt(*s); InvalidateRect(s->hwnd, nullptr, TRUE); break;
            case IDC_S_RESET: s->settings = WS_SETTINGS{}; SetWindowTextW(GetDlgItem(hwnd, IDC_S_OPACITY), L"35"); SendMessageW(GetDlgItem(hwnd, IDC_S_GEO), BM_SETCHECK, BST_CHECKED, 0); ApplyTheme(*s); break;
            case IDC_S_SAVE:
            {
                wchar_t opacity[16]{};
                GetWindowTextW(GetDlgItem(hwnd, IDC_S_OPACITY), opacity, static_cast<int>(std::size(opacity)));
                s->settings.image_opacity = (std::clamp)(_wtoi(opacity), 0, 100);
                s->settings.geo_enabled = SendMessageW(GetDlgItem(hwnd, IDC_S_GEO), BM_GETCHECK, 0, 0) == BST_CHECKED;
                WSC_SaveSettings(&s->settings);
                ApplyTheme(*s);
                DestroyWindow(hwnd);
                break;
            }
            }
            return 0;
        case WM_DRAWITEM:
        {
            const auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis->CtlType == ODT_BUTTON) DrawOwnerButton(dis, *s);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wp), s->settings.text_color);
            return reinterpret_cast<LRESULT>(s->bgBrush);
        case WM_CTLCOLOREDIT:
            SetBkColor(reinterpret_cast<HDC>(wp), Mix(s->settings.background_color, RGB(255,255,255), 0.06));
            SetTextColor(reinterpret_cast<HDC>(wp), s->settings.text_color);
            return reinterpret_cast<LRESULT>(s->editBrush);
        case WM_ERASEBKGND:
        {
            RECT r{}; GetClientRect(hwnd, &r); FillRect(reinterpret_cast<HDC>(wp), &r, s->bgBrush); return 1;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: s->settingsWindow = nullptr; return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT CALLBACK FilterProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            s = reinterpret_cast<AppState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        }
        if (!s) return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg)
        {
        case WM_CREATE:
        {
            ApplyDwmTheme(hwnd, s->settings);
            WS_FILTER_CONFIG current{};
            { std::scoped_lock guard(s->filterMutex); current = s->filter; }
            auto checkbox = [&](const wchar_t* text, int id, int y, bool checked)
            {
                HWND c = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    18, y, 310, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), s->instance, nullptr);
                SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(s->font), TRUE);
                SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
            };
            checkbox(L"Incoming only", IDC_F_INCOMING, 22, current.incoming_only != 0);
            checkbox(L"Public IPs only", IDC_F_PUBLIC, 56, current.public_only != 0);
            checkbox(L"Hide loopback / multicast / link-local noise", IDC_F_NOISE, 90, current.hide_noise != 0);
            checkbox(L"Invert port filter", IDC_F_INVERT, 124, current.invert_ports != 0);
            HWND label = CreateWindowW(L"STATIC", L"Remote ports (example: 443, 50000-65535)", WS_CHILD | WS_VISIBLE,
                18, 166, 340, 22, hwnd, nullptr, s->instance, nullptr);
            SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(s->font), TRUE);
            HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", PortRulesText(current).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 18, 192, 350, 32, hwnd, reinterpret_cast<HMENU>(IDC_F_PORTS), s->instance, nullptr);
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(s->font), TRUE);
            MakeButton(hwnd, L"Reset", IDC_F_RESET);
            MakeButton(hwnd, L"Save", IDC_F_SAVE);
            MoveWindow(GetDlgItem(hwnd, IDC_F_RESET), 190, 250, 84, 36, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_F_SAVE), 284, 250, 84, 36, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDC_F_RESET)
            {
                SendMessageW(GetDlgItem(hwnd, IDC_F_INCOMING), BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(GetDlgItem(hwnd, IDC_F_PUBLIC), BM_SETCHECK, BST_UNCHECKED, 0);
                SendMessageW(GetDlgItem(hwnd, IDC_F_NOISE), BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(GetDlgItem(hwnd, IDC_F_INVERT), BM_SETCHECK, BST_UNCHECKED, 0);
                SetWindowTextW(GetDlgItem(hwnd, IDC_F_PORTS), L"");
            }
            else if (LOWORD(wp) == IDC_F_SAVE)
            {
                WS_FILTER_CONFIG config{};
                { std::scoped_lock guard(s->filterMutex); config = s->filter; }
                config.incoming_only = SendMessageW(GetDlgItem(hwnd, IDC_F_INCOMING), BM_GETCHECK, 0, 0) == BST_CHECKED;
                config.public_only = SendMessageW(GetDlgItem(hwnd, IDC_F_PUBLIC), BM_GETCHECK, 0, 0) == BST_CHECKED;
                config.hide_noise = SendMessageW(GetDlgItem(hwnd, IDC_F_NOISE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                config.invert_ports = SendMessageW(GetDlgItem(hwnd, IDC_F_INVERT), BM_GETCHECK, 0, 0) == BST_CHECKED;
                wchar_t ports[512]{};
                GetWindowTextW(GetDlgItem(hwnd, IDC_F_PORTS), ports, static_cast<int>(std::size(ports)));
                if (!ParsePortRules(ports, config))
                {
                    MessageBoxW(hwnd, L"Invalid port rule. Use values like 443 or 50000-65535 separated by commas.", L"WireScope", MB_ICONWARNING);
                    return 0;
                }
                { std::scoped_lock guard(s->filterMutex); s->filter = config; }
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DRAWITEM:
        {
            const auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis->CtlType == ODT_BUTTON) DrawOwnerButton(dis, *s);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wp), s->settings.text_color);
            return reinterpret_cast<LRESULT>(s->bgBrush);
        case WM_CTLCOLOREDIT:
            SetBkColor(reinterpret_cast<HDC>(wp), Mix(s->settings.background_color, RGB(255,255,255), 0.06));
            SetTextColor(reinterpret_cast<HDC>(wp), s->settings.text_color);
            return reinterpret_cast<LRESULT>(s->editBrush);
        case WM_ERASEBKGND:
        {
            RECT r{}; GetClientRect(hwnd, &r); FillRect(reinterpret_cast<HDC>(wp), &r, s->bgBrush); return 1;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: s->filterWindow = nullptr; return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void OpenSettings(AppState& s)
    {
        if (s.settingsWindow && IsWindow(s.settingsWindow)) { SetForegroundWindow(s.settingsWindow); return; }
        s.settingsWindow = CreateWindowExW(WS_EX_TOOLWINDOW, SETTINGS_CLASS, L"WireScope Settings",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 445, 480,
            s.hwnd, nullptr, s.instance, &s);
        ShowWindow(s.settingsWindow, SW_SHOW);
    }

    void OpenFilters(AppState& s)
    {
        if (s.filterWindow && IsWindow(s.filterWindow)) { SetForegroundWindow(s.filterWindow); return; }
        s.filterWindow = CreateWindowExW(WS_EX_TOOLWINDOW, FILTER_CLASS, L"WireScope Filters",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 410, 340,
            s.hwnd, nullptr, s.instance, &s);
        ShowWindow(s.filterWindow, SW_SHOW);
    }

    LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            s = reinterpret_cast<AppState*>(cs->lpCreateParams);
            s->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        }
        if (!s) return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg)
        {
        case WM_CREATE:
        {
            SetDarkTitleBar(hwnd);
            WSC_LoadSettings(&s->settings);
            s->filter = WS_FILTER_CONFIG{};

            s->adapterCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | WS_VSCROLL,
                0,0,100,100, hwnd, reinterpret_cast<HMENU>(IDC_ADAPTER), s->instance, nullptr);
            s->presetCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | WS_VSCROLL,
                0,0,100,100, hwnd, reinterpret_cast<HMENU>(IDC_PRESET), s->instance, nullptr);
            SendMessageW(s->adapterCombo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 30);
            SendMessageW(s->adapterCombo, CB_SETITEMHEIGHT, 0, 30);
            SendMessageW(s->presetCombo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 30);
            SendMessageW(s->presetCombo, CB_SETITEMHEIGHT, 0, 30);
            s->searchEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                0,0,100,32, hwnd, reinterpret_cast<HMENU>(IDC_SEARCH), s->instance, nullptr);
            SendMessageW(s->searchEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search IP, location, owner…"));

            s->filterButton = MakeButton(hwnd, L"Filters", IDC_FILTERS);
            s->settingsButton = MakeButton(hwnd, L"Settings", IDC_SETTINGS);
            s->startButton = MakeButton(hwnd, L"Start", IDC_START);
            s->stopButton = MakeButton(hwnd, L"Stop", IDC_STOP);
            s->exportButton = MakeButton(hwnd, L"Export", IDC_EXPORT);
            s->clearButton = MakeButton(hwnd, L"Clear", IDC_CLEAR);
            EnableWindow(s->stopButton, FALSE);

            s->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                0,0,100,100, hwnd, reinterpret_cast<HMENU>(IDC_LIST), s->instance, nullptr);
            ListView_SetExtendedListViewStyle(s->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
            AddColumn(s->list, 0, L"Flag", 48);
            AddColumn(s->list, 1, L"Remote IP", 145);
            AddColumn(s->list, 2, L"Type", 72);
            AddColumn(s->list, 3, L"Protocol", 78);
            AddColumn(s->list, 4, L"Location", 240);
            AddColumn(s->list, 5, L"Owner / ASN", 230);
            AddColumn(s->list, 6, L"Port", 78);
            AddColumn(s->list, 7, L"Packets", 82);
            AddColumn(s->list, 8, L"Bytes", 92);
            AddColumn(s->list, 9, L"First seen", 92);
            AddColumn(s->list, 10, L"Last seen", 92);

            s->flagImages = ImageList_Create(24, 18, ILC_COLOR32, 8, 8);
            std::vector<std::uint32_t> blankPixels(24 * 18, 0);
            HBITMAP blank = CreateBitmap(24, 18, 1, 32, blankPixels.data());
            ImageList_Add(s->flagImages, blank, nullptr);
            DeleteObject(blank);
            ListView_SetImageList(s->list, s->flagImages, LVSIL_SMALL);

            const wchar_t* presets[] = {L"None", L"TCP", L"UDP", L"Discord", L"HTTP", L"HTTPS", L"DNS", L"QUIC"};
            for (const auto* p : presets) SendMessageW(s->presetCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p));
            SendMessageW(s->presetCombo, CB_SETCURSEL, 0, 0);

            ApplyTheme(*s);
            if (!WSN_Initialize())
            {
                wchar_t error[512]{};
                WSN_GetLastError(error, static_cast<int>(std::size(error)));
                MessageBoxW(hwnd, error, L"WireScope", MB_ICONERROR);
            }
            LoadAdapters(*s);
            SetTimer(hwnd, TIMER_FLUSH, 250, nullptr);
            return 0;
        }
        case WM_SIZE:
            LayoutMain(*s, LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_COMMAND:
        {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_PRESET && code == CBN_SELCHANGE)
            {
                const int index = static_cast<int>(SendMessageW(s->presetCombo, CB_GETCURSEL, 0, 0));
                ApplyPreset(*s, index);
            }
            else if (id == IDC_SEARCH && code == EN_CHANGE) RefreshList(*s);
            else if (id == IDC_START)
            {
                const int index = static_cast<int>(SendMessageW(s->adapterCombo, CB_GETCURSEL, 0, 0));
                if (index < 0 || index >= static_cast<int>(s->adapters.size()))
                {
                    MessageBoxW(hwnd, L"Choose a capture adapter first.", L"WireScope", MB_ICONINFORMATION);
                    break;
                }
                if (!WSN_StartCapture(s->adapters[static_cast<std::size_t>(index)].id, CaptureCallback, s))
                {
                    wchar_t error[512]{}; WSN_GetLastError(error, static_cast<int>(std::size(error)));
                    MessageBoxW(hwnd, error, L"WireScope", MB_ICONERROR);
                    break;
                }
                s->capturing = true;
                s->status = L"Listening";
                EnableWindow(s->startButton, FALSE);
                EnableWindow(s->stopButton, TRUE);
                EnableWindow(s->adapterCombo, FALSE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else if (id == IDC_STOP)
            {
                WSN_StopCapture();
                s->capturing = false;
                s->status = L"Idle";
                EnableWindow(s->startButton, TRUE);
                EnableWindow(s->stopButton, FALSE);
                EnableWindow(s->adapterCombo, TRUE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else if (id == IDC_FILTERS) OpenFilters(*s);
            else if (id == IDC_SETTINGS) OpenSettings(*s);
            else if (id == IDC_EXPORT) ExportCsv(*s);
            else if (id == IDC_CLEAR)
            {
                { std::scoped_lock guard(s->pendingMutex); s->pending.clear(); }
                ++s->generation;
                s->rows.clear(); s->rowIndex.clear(); s->totalPackets = 0;
                RefreshList(*s);
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_FLUSH) DrainPending(*s);
            return 0;
        case WM_APP_GEO:
        {
            std::unique_ptr<GeoMessage> message(reinterpret_cast<GeoMessage*>(lp));
            if (!message || message->generation != s->generation || message->rowIndex >= s->rows.size()) return 0;
            auto& row = *s->rows[message->rowIndex];
            row.geoPending = false;
            if (message->ok)
            {
                row.geo = message->info;
                row.geoReady = true;
            }
            RefreshList(*s);
            return 0;
        }
        case WM_NOTIFY:
        {
            const auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->hwndFrom == ListView_GetHeader(s->list) && hdr->code == NM_CUSTOMDRAW)
            {
                auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
                if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
                {
                    HBRUSH fill = CreateSolidBrush(Mix(s->settings.background_color, RGB(255,255,255), 0.075));
                    FillRect(cd->hdc, &cd->rc, fill);
                    DeleteObject(fill);
                    const int index = static_cast<int>(cd->dwItemSpec);
                    wchar_t text[128]{};
                    HDITEMW item{}; item.mask = HDI_TEXT; item.pszText = text; item.cchTextMax = static_cast<int>(std::size(text));
                    Header_GetItem(hdr->hwndFrom, index, &item);
                    RECT r = cd->rc; r.left += 8;
                    SetBkMode(cd->hdc, TRANSPARENT);
                    SetTextColor(cd->hdc, s->settings.text_color);
                    const auto oldFont = SelectObject(cd->hdc, s->font);
                    DrawTextW(cd->hdc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    SelectObject(cd->hdc, oldFont);
                    return CDRF_SKIPDEFAULT;
                }
            }
            if (hdr->idFrom == IDC_LIST && hdr->code == NM_DBLCLK)
            {
                const int selected = ListView_GetNextItem(s->list, -1, LVNI_SELECTED);
                if (selected >= 0)
                {
                    LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = selected;
                    if (ListView_GetItem(s->list, &item))
                    {
                        const auto rowIndex = static_cast<std::size_t>(item.lParam);
                        if (rowIndex < s->rows.size()) ClipboardText(hwnd, ToWide(s->rows[rowIndex]->ip));
                    }
                }
            }
            return 0;
        }
        case WM_DRAWITEM:
        {
            const auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis->CtlType == ODT_BUTTON) DrawOwnerButton(dis, *s);
            else if (dis->CtlType == ODT_COMBOBOX) DrawOwnerCombo(dis, *s);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wp), s->settings.text_color);
            return reinterpret_cast<LRESULT>(s->bgBrush);
        case WM_CTLCOLOREDIT:
            SetBkColor(reinterpret_cast<HDC>(wp), Mix(s->settings.background_color, RGB(255,255,255), 0.06));
            SetTextColor(reinterpret_cast<HDC>(wp), s->settings.text_color);
            return reinterpret_cast<LRESULT>(s->editBrush);
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{}; GetClientRect(hwnd, &client);
            PaintWindowBackground(*s, dc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_FLUSH);
            WSN_StopCapture();
            WSN_Shutdown();
            WSC_SaveSettings(&s->settings);
            if (s->flagImages) { ImageList_Destroy(s->flagImages); s->flagImages = nullptr; }
            DeleteUiObjects(*s);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool RegisterClasses(HINSTANCE instance)
    {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(instance, MAKEINTRESOURCE(101));
        wc.hIconSm = wc.hIcon;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MainProc;
        wc.lpszClassName = MAIN_CLASS;
        if (!RegisterClassExW(&wc)) return false;

        wc.hIcon = nullptr;
        wc.hIconSm = nullptr;
        wc.lpfnWndProc = SettingsProc;
        wc.lpszClassName = SETTINGS_CLASS;
        if (!RegisterClassExW(&wc)) return false;
        wc.lpfnWndProc = FilterProc;
        wc.lpszClassName = FILTER_CLASS;
        return RegisterClassExW(&wc) != 0;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    GdiplusStartupInput gdiplusInput;
    if (GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) != Ok) return 1;

    if (!RegisterClasses(instance))
    {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    AppState state{};
    state.instance = instance;
    HWND hwnd = CreateWindowExW(0, MAIN_CLASS, L"WireScope",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1380, 780,
        nullptr, nullptr, instance, &state);
    if (!hwnd)
    {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return static_cast<int>(msg.wParam);
}
