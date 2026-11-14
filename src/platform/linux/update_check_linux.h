// The update check on Linux: asks GitHub for the latest release (libsoup, the HTTPS client
// Linux already uses), then reads the reply with the same parser as Windows.
#pragma once

#include <string>

#include "../../common/update_parse.h"

namespace bb::update {

// Blocking, with short timeouts: call it off the UI thread.
Release FetchLatestLinux(const std::string& userAgent);

}  // namespace bb::update
