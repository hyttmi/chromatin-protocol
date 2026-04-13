#include "relay/http/http_parser.h"

namespace chromatindb::relay::http {

bool parse_request_line(std::string_view /*line*/, HttpRequest& /*req*/) {
    return false;  // Stub -- tests will fail
}

bool parse_header_line(std::string_view /*line*/, HttpRequest& /*req*/) {
    return false;  // Stub -- tests will fail
}

bool finalize_headers(HttpRequest& /*req*/) {
    return false;  // Stub -- tests will fail
}

} // namespace chromatindb::relay::http
