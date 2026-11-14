// Bunbun — the parts of the update check that don't depend on the platform: the release
// type, version comparison, and reading GitHub's reply. update.h (Windows, WinHTTP) and
// the Linux settings host both use these, so they agree on what "newer" means.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "json.h"

#ifndef BUNBUN_REPO_STR
#define BUNBUN_REPO_STR "iaryasharma/Bunbun"
#endif

namespace bb::update {

struct Release {
    bool ok = false;
    std::string version;  // "0.2.0" (tag without the leading v)
    std::string url;      // release page
    std::string name;     // release title
    std::string setupUrl; // Bunbun-<version>-setup.exe asset, "" if the release has none
    std::string sumsUrl;  // SHA256SUMS.txt asset
    std::string error;    // why it failed, for the settings app
};

// Numeric compare of dotted versions; "1.2" == "1.2.0". A pre-release suffix
// ("0.2.0-beta") sorts before the plain version. <0, 0, >0 like strcmp.
inline int CompareVersions(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& v, std::vector<long>* parts, bool* pre) {
        std::string s = v;
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        const size_t dash = s.find_first_of("-+");
        *pre = dash != std::string::npos && s[dash] == '-';
        if (dash != std::string::npos) s.resize(dash);
        size_t pos = 0;
        while (pos <= s.size()) {
            const size_t dot = s.find('.', pos);
            const std::string piece = s.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
            parts->push_back(piece.empty() ? 0 : std::strtol(piece.c_str(), nullptr, 10));
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
    };
    std::vector<long> pa, pb;
    bool preA = false, preB = false;
    parse(a, &pa, &preA);
    parse(b, &pb, &preB);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const long x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (preA != preB) return preA ? -1 : 1;
    return 0;
}

// Packs "1.2.3" into one int (1002003) so it fits Bunbun's int-only state store.
inline int PackVersion(const std::string& v) {
    std::string s = v;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    long part[3] = {0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 3 && pos <= s.size(); ++i) {
        part[i] = std::strtol(s.c_str() + pos, nullptr, 10);
        const size_t dot = s.find('.', pos);
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return static_cast<int>(part[0] * 1000000 + part[1] * 1000 + part[2]);
}

// Reads GitHub's reply for /releases/latest (the body of a 200 answer).
inline Release ParseLatestRelease(const std::string& body) {
    Release r;
    bb::json::Value root;
    if (!bb::json::Parse(body, &root) || !root.isObject()) { r.error = "unexpected reply from GitHub"; return r; }
    std::string tag = root.getString("tag_name");
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    if (tag.empty()) { r.error = "the latest release has no version tag"; return r; }
    r.version = tag;
    r.url = root.getString("html_url");
    r.name = root.getString("name");
    // Only ever open a GitHub page from this reply.
    if (r.url.rfind("https://github.com/", 0) != 0) r.url = "https://github.com/" BUNBUN_REPO_STR "/releases/latest";
    // The installer and its checksums, for the one-click updater. Only GitHub
    // download links of this repository are taken.
    if (const bb::json::Value* assets = root.find("assets")) {
        const std::string prefix = "https://github.com/" BUNBUN_REPO_STR "/releases/download/";
        for (const bb::json::Value& a : assets->arr) {
            const std::string name = a.getString("name"), link = a.getString("browser_download_url");
            if (link.rfind(prefix, 0) != 0) continue;
            if (name == "Bunbun-" + tag + "-setup.exe") r.setupUrl = link;
            else if (name == "SHA256SUMS.txt") r.sumsUrl = link;
        }
    }
    r.ok = true;
    return r;
    return r;
}

}  // namespace bb::update
