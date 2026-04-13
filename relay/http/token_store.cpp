#include "relay/http/token_store.h"

namespace chromatindb::relay::http {

std::string TokenStore::create_session(std::vector<uint8_t> /*pubkey*/,
                                       std::array<uint8_t, 32> /*ns_hash*/,
                                       uint32_t /*rate_limit*/) {
    return "";  // Stub -- tests will fail
}

HttpSessionState* TokenStore::lookup(const std::string& /*token*/) {
    return nullptr;  // Stub
}

HttpSessionState* TokenStore::lookup_by_id(uint64_t /*session_id*/) {
    return nullptr;  // Stub
}

const std::string* TokenStore::get_token(uint64_t /*session_id*/) const {
    return nullptr;  // Stub
}

void TokenStore::remove_session(uint64_t /*session_id*/) {
    // Stub
}

void TokenStore::remove_by_token(const std::string& /*token*/) {
    // Stub
}

size_t TokenStore::reap_idle(std::chrono::seconds /*timeout*/) {
    return 0;  // Stub
}

size_t TokenStore::count() const {
    return 0;  // Stub
}

} // namespace chromatindb::relay::http
