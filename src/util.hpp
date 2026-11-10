#pragma once

#include <string>

long long now_ms();

// Opens a URL with the default handler (`/usr/bin/open`), used by --open-links.
bool open_url(const std::string &url);
