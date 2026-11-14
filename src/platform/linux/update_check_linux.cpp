// The update check on Linux. The same request as bb::update::FetchLatest (Windows, WinHTTP):
// GET /repos/<repo>/releases/latest, the same status handling, the same reply parsing.
#include "update_check_linux.h"

#include <libsoup/soup.h>

#include <string>

#include "../../common/update_parse.h"

namespace bb::update {

Release FetchLatestLinux(const std::string& userAgent) {
    Release r;
    SoupSession* session = soup_session_new();
    soup_session_set_timeout(session, 8);
    soup_session_set_user_agent(session, userAgent.c_str());
    const std::string url = "https://api.github.com/repos/" BUNBUN_REPO_STR "/releases/latest";
    SoupMessage* msg = soup_message_new("GET", url.c_str());
    if (!msg) {
        g_object_unref(session);
        r.error = "couldn't start a web request";
        return r;
    }
    soup_message_headers_append(soup_message_get_request_headers(msg), "Accept", "application/vnd.github+json");

    GError* error = nullptr;
    GBytes* bytes = soup_session_send_and_read(session, msg, nullptr, &error);
    if (!bytes) {
        r.error = "couldn't reach GitHub (offline?)";
        if (error) g_error_free(error);
        g_object_unref(msg);
        g_object_unref(session);
        return r;
    }
    // A release JSON is a few KB; anything much bigger isn't a release.
    gsize size = 0;
    const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    const std::string body = size <= 512 * 1024 && data ? std::string(data, size) : std::string();
    const unsigned status = soup_message_get_status(msg);
    g_bytes_unref(bytes);
    g_object_unref(msg);
    g_object_unref(session);

    if (status == 404) { r.error = "no published release yet"; return r; }
    if (status == 403 || status == 429) { r.error = "GitHub is rate-limiting checks; try again later"; return r; }
    if (status != 200) { r.error = "GitHub answered HTTP " + std::to_string(status); return r; }
    if (body.empty()) { r.error = "unexpected reply from GitHub"; return r; }
    return ParseLatestRelease(body);
}

}  // namespace bb::update
