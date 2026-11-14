// Bunbun — update check.
//
// Asks GitHub for the latest published release and compares it with the running
// version. Read-only: nothing is downloaded or installed here; the updater
// (src/updater.cpp, `bunbun.exe --update`) uses setupUrl / sumsUrl to install it.
// Shared by bunbun.exe (daily check, tray, updater) and bunbun-settings.exe.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "json.h"
#include "update_parse.h"

#ifndef BUNBUN_REPO_STR
#define BUNBUN_REPO_STR "iaryasharma/Bunbun"
#endif

namespace bb::update {

inline std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// GET https://api.github.com/repos/<repo>/releases/latest. Blocking, with short
// timeouts; call it off the UI thread.
inline Release FetchLatest(const wchar_t* userAgent) {
    Release r;
    HINTERNET session = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { r.error = "couldn't start a web session"; return r; }
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    const std::wstring path = L"/repos/" + Widen(BUNBUN_REPO_STR) + L"/releases/latest";
    HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    std::string body;
    DWORD status = 0;
    if (request &&
        WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1L),
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
            if (body.size() + avail > 512 * 1024) break;  // a release JSON is a few KB
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
            body.append(chunk.data(), read);
        }
    } else {
        r.error = "couldn't reach GitHub (offline?)";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!r.error.empty()) return r;
    if (status == 404) { r.error = "no published release yet"; return r; }
    if (status == 403 || status == 429) { r.error = "GitHub is rate-limiting checks; try again later"; return r; }
    if (status != 200) { r.error = "GitHub answered HTTP " + std::to_string(status); return r; }

    return bb::update::ParseLatestRelease(body);
}

}  // namespace bb::update
