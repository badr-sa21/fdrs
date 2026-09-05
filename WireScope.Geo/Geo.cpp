#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Winhttp.h>
#include <ShlObj.h>
#include <algorithm>
#include <cwchar>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <iterator>
#include <unordered_map>
#include <vector>
#include "GeoApi.h"

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Shell32.lib")

namespace
{
    std::mutex g_cache_lock;
    std::unordered_map<std::string, WS_GEO_INFO> g_cache;

    std::wstring ToWide(const std::string& value)
    {
        if (value.empty()) return {};
        const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
        if (needed <= 0) return {};
        std::wstring out(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), needed);
        return out;
    }

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool HttpGet(const wchar_t* host, const std::wstring& path, std::vector<unsigned char>& output)
    {
        HINTERNET session = WinHttpOpen(L"WireScope/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;

        WinHttpSetTimeouts(session, 4000, 4000, 5000, 5000);
        HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection)
        {
            WinHttpCloseHandle(session);
            return false;
        }

        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        const wchar_t* accept = L"Accept: application/json\r\n";
        WinHttpAddRequestHeaders(request, accept, static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);

        bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(request, nullptr);

        if (ok)
        {
            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) || status < 200 || status >= 300)
                ok = false;
        }

        while (ok)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available))
            {
                ok = false;
                break;
            }
            if (available == 0) break;

            const auto old = output.size();
            output.resize(old + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, output.data() + old, available, &read))
            {
                ok = false;
                break;
            }
            output.resize(old + read);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return ok;
    }

    std::string JsonString(const std::string& json, const char* key)
    {
        const std::string needle = std::string("\"") + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return {};
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return {};
        ++pos;
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (pos >= json.size() || json[pos] != '"') return {};
        ++pos;

        std::string out;
        bool escape = false;
        for (; pos < json.size(); ++pos)
        {
            const char c = json[pos];
            if (escape)
            {
                switch (c)
                {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                default: out.push_back(c); break;
                }
                escape = false;
            }
            else if (c == '\\') escape = true;
            else if (c == '"') break;
            else out.push_back(c);
        }
        return out;
    }

    long JsonInteger(const std::string& json, const char* key)
    {
        const std::string needle = std::string("\"") + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return 0;
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return 0;
        ++pos;
        while (pos < json.size() && (std::isspace(static_cast<unsigned char>(json[pos])) || json[pos] == '"')) ++pos;
        char* end = nullptr;
        return std::strtol(json.c_str() + pos, &end, 10);
    }

    std::wstring FlagCacheDirectory()
    {
        wchar_t local[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, local)))
            return L".";
        std::filesystem::path dir(local);
        dir /= L"WireScope";
        dir /= L"flags";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir.wstring();
    }

    std::wstring EnsureFlag(const std::string& countryCode)
    {
        if (countryCode.size() != 2) return {};
        const auto code = ToLower(countryCode);
        std::filesystem::path file = FlagCacheDirectory();
        file /= ToWide(code) + L".png";
        std::error_code ec;
        if (std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > 0)
            return file.wstring();

        std::vector<unsigned char> bytes;
        const auto path = L"/w40/" + ToWide(code) + L".png";
        if (!HttpGet(L"flagcdn.com", path, bytes) || bytes.empty()) return {};

        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        DWORD written = 0;
        const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(h);
        if (!ok || written != bytes.size())
        {
            DeleteFileW(file.c_str());
            return {};
        }
        return file.wstring();
    }

    template <std::size_t N>
    void CopyWide(wchar_t (&dest)[N], const std::wstring& source)
    {
        wcsncpy_s(dest, source.c_str(), _TRUNCATE);
    }
}

extern "C" BOOL __stdcall WSG_Lookup(const char* ip_address, WS_GEO_INFO* info)
{
    if (!ip_address || !*ip_address || !info) return FALSE;

    {
        std::scoped_lock guard(g_cache_lock);
        if (const auto it = g_cache.find(ip_address); it != g_cache.end())
        {
            *info = it->second;
            return TRUE;
        }
    }

    std::vector<unsigned char> body;
    const auto path = L"/" + ToWide(ip_address);
    if (!HttpGet(L"ipwho.is", path, body) || body.empty()) return FALSE;

    const std::string json(reinterpret_cast<const char*>(body.data()), body.size());
    if (json.find("\"success\":false") != std::string::npos) return FALSE;

    WS_GEO_INFO result{};
    const auto code = JsonString(json, "country_code");
    const auto country = JsonString(json, "country");
    const auto region = JsonString(json, "region");
    const auto city = JsonString(json, "city");
    auto owner = JsonString(json, "org");
    if (owner.empty()) owner = JsonString(json, "isp");
    const auto asnNumber = JsonInteger(json, "asn");

    CopyWide(result.country_code, ToWide(code));
    CopyWide(result.country, ToWide(country));
    CopyWide(result.region, ToWide(region));
    CopyWide(result.city, ToWide(city));
    CopyWide(result.owner, ToWide(owner));
    if (asnNumber > 0) CopyWide(result.asn, L"AS" + std::to_wstring(asnNumber));
    CopyWide(result.flag_path, EnsureFlag(code));

    {
        std::scoped_lock guard(g_cache_lock);
        g_cache[ip_address] = result;
    }
    *info = result;
    return TRUE;
}

extern "C" void __stdcall WSG_ClearCache()
{
    std::scoped_lock guard(g_cache_lock);
    g_cache.clear();
}
