#include "relay/http/sse_writer.h"

#include <spdlog/spdlog.h>

namespace chromatindb::relay::http {

SseWriter::SseWriter(asio::any_io_executor executor, WriteFn write_fn)
    : write_fn_(std::move(write_fn))
    , signal_(executor) {
}

void SseWriter::push_event(const nlohmann::json& /*data*/, uint64_t /*event_id*/) {
    // STUB: not implemented yet
}

void SseWriter::push_broadcast(const nlohmann::json& /*data*/) {
    // STUB: not implemented yet
}

asio::awaitable<void> SseWriter::run() {
    // STUB: not implemented yet
    co_return;
}

void SseWriter::close() {
    // STUB: not implemented yet
}

std::string SseWriter::format_event(const nlohmann::json& /*data*/, uint64_t /*event_id*/) {
    return "";  // STUB
}

std::string SseWriter::format_heartbeat() {
    return "";  // STUB
}

std::string SseWriter::format_broadcast(const nlohmann::json& /*data*/) {
    return "";  // STUB
}

} // namespace chromatindb::relay::http
