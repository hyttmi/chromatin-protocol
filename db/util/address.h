#pragma once

#include <cctype>
#include <cstdlib>
#include <string>

namespace chromatindb::util {

/// Validate a network address of the form "host:port".
/// Rejects empty host, missing colon, non-numeric port, port out of range,
/// and any whitespace in the host portion.
inline bool is_valid_host_port(const std::string& s) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon == s.size() - 1) {
        return false;
    }
    std::string port_str = s.substr(colon + 1);
    for (char c : port_str) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    int port = std::atoi(port_str.c_str());
    if (port < 1 || port > 65535) return false;
    std::string host = s.substr(0, colon);
    for (char c : host) {
        if (std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

} // namespace chromatindb::util
