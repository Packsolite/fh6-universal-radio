#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fh6::net {

// Blocking in-memory HTTP(S) GET. Body on HTTP 200, else nullopt. 5 s timeouts.
// extra_header is an optional raw header line (e.g. "Authorization: ...").
std::optional<std::string> http_get(std::string_view url, std::string_view extra_header = {});

} // namespace fh6::net
