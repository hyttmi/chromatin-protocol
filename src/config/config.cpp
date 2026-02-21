#include "config/config.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include <json/json.h>
#include <spdlog/spdlog.h>

namespace chromatin::config {

namespace {

// Parse "host:port" into a pair. Throws on bad format.
std::pair<std::string, uint16_t> parse_endpoint(const std::string& s) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon == s.size() - 1) {
        throw std::runtime_error("invalid bootstrap endpoint: " + s);
    }
    auto host = s.substr(0, colon);
    int port = std::stoi(s.substr(colon + 1));
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("invalid port in bootstrap endpoint: " + s);
    }
    return {host, static_cast<uint16_t>(port)};
}

} // namespace

Config load_config(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("cannot open config file: " + path.string());
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, ifs, &root, &errors)) {
        throw std::runtime_error("config parse error: " + errors);
    }

    Config cfg;

    if (root.isMember("data_dir")) {
        cfg.data_dir = root["data_dir"].asString();
    }
    if (root.isMember("bind")) {
        cfg.bind = root["bind"].asString();
    }
    if (root.isMember("tcp_port")) {
        cfg.tcp_port = static_cast<uint16_t>(root["tcp_port"].asUInt());
    }
    if (root.isMember("ws_port")) {
        cfg.ws_port = static_cast<uint16_t>(root["ws_port"].asUInt());
    }
    if (root.isMember("bootstrap") && root["bootstrap"].isArray()) {
        for (const auto& entry : root["bootstrap"]) {
            cfg.bootstrap.push_back(parse_endpoint(entry.asString()));
        }
    }
    if (root.isMember("tls_cert_path")) {
        cfg.tls_cert_path = root["tls_cert_path"].asString();
    }
    if (root.isMember("tls_key_path")) {
        cfg.tls_key_path = root["tls_key_path"].asString();
    }

    // Network
    if (root.isMember("external_address")) {
        cfg.external_address = root["external_address"].asString();
    }
    if (root.isMember("replication_factor")) {
        cfg.replication_factor = static_cast<uint16_t>(root["replication_factor"].asUInt());
    }
    if (root.isMember("max_routing_table_size")) {
        cfg.max_routing_table_size = static_cast<uint16_t>(root["max_routing_table_size"].asUInt());
    }
    if (root.isMember("max_nodes_per_subnet")) {
        cfg.max_nodes_per_subnet = static_cast<uint16_t>(root["max_nodes_per_subnet"].asUInt());
    }

    // Timeouts
    if (root.isMember("tcp_connect_timeout")) {
        cfg.tcp_connect_timeout = static_cast<uint16_t>(root["tcp_connect_timeout"].asUInt());
    }
    if (root.isMember("tcp_read_timeout")) {
        cfg.tcp_read_timeout = static_cast<uint16_t>(root["tcp_read_timeout"].asUInt());
    }
    if (root.isMember("ws_idle_timeout")) {
        cfg.ws_idle_timeout = static_cast<uint16_t>(root["ws_idle_timeout"].asUInt());
    }
    if (root.isMember("upload_timeout")) {
        cfg.upload_timeout = static_cast<uint16_t>(root["upload_timeout"].asUInt());
    }

    // Worker pool
    if (root.isMember("worker_pool_threads")) {
        cfg.worker_pool_threads = static_cast<uint16_t>(root["worker_pool_threads"].asUInt());
    }
    if (root.isMember("worker_pool_queue_max")) {
        cfg.worker_pool_queue_max = static_cast<uint16_t>(root["worker_pool_queue_max"].asUInt());
    }

    // Rate limiter
    if (root.isMember("rate_limit_tokens")) {
        cfg.rate_limit_tokens = root["rate_limit_tokens"].asDouble();
    }
    if (root.isMember("rate_limit_max")) {
        cfg.rate_limit_max = root["rate_limit_max"].asDouble();
    }
    if (root.isMember("rate_limit_refill")) {
        cfg.rate_limit_refill = root["rate_limit_refill"].asDouble();
    }

    // Size limits
    if (root.isMember("max_message_size")) {
        cfg.max_message_size = root["max_message_size"].asUInt64();
    }
    if (root.isMember("max_profile_size")) {
        cfg.max_profile_size = root["max_profile_size"].asUInt();
    }
    if (root.isMember("max_request_blob_size")) {
        cfg.max_request_blob_size = root["max_request_blob_size"].asUInt();
    }

    // TTL & maintenance
    if (root.isMember("ttl_days")) {
        cfg.ttl_days = root["ttl_days"].asUInt();
    }
    if (root.isMember("compact_interval_minutes")) {
        cfg.compact_interval_minutes = root["compact_interval_minutes"].asUInt();
    }
    if (root.isMember("compact_keep_entries")) {
        cfg.compact_keep_entries = root["compact_keep_entries"].asUInt();
    }
    if (root.isMember("compact_min_age_hours")) {
        cfg.compact_min_age_hours = root["compact_min_age_hours"].asUInt();
    }

    // PoW
    if (root.isMember("contact_pow_difficulty")) {
        cfg.contact_pow_difficulty = static_cast<uint8_t>(root["contact_pow_difficulty"].asUInt());
    }
    if (root.isMember("name_pow_difficulty")) {
        cfg.name_pow_difficulty = static_cast<uint8_t>(root["name_pow_difficulty"].asUInt());
    }

    // Sync
    if (root.isMember("sync_interval_seconds")) {
        cfg.sync_interval_seconds = static_cast<uint16_t>(root["sync_interval_seconds"].asUInt());
    }
    if (root.isMember("sync_batch_size")) {
        cfg.sync_batch_size = static_cast<uint16_t>(root["sync_batch_size"].asUInt());
    }

    // TCP transport
    if (root.isMember("max_tcp_clients")) {
        cfg.max_tcp_clients = static_cast<uint16_t>(root["max_tcp_clients"].asUInt());
    }

    // Connection pool
    if (root.isMember("conn_pool_max")) {
        cfg.conn_pool_max = static_cast<uint16_t>(root["conn_pool_max"].asUInt());
    }
    if (root.isMember("conn_pool_idle_seconds")) {
        cfg.conn_pool_idle_seconds = static_cast<uint16_t>(root["conn_pool_idle_seconds"].asUInt());
    }

    // Storage
    if (root.isMember("mdbx_max_size")) {
        cfg.mdbx_max_size = root["mdbx_max_size"].asUInt64();
    }

    validate_config(cfg);
    return cfg;
}

void validate_config(const Config& cfg) {
    struct Rule {
        bool (*failed)(const Config&);
        const char* message;
    };

    static constexpr Rule rules[] = {
        {[](const Config& c) { return c.tcp_port == 0;            }, "invalid tcp_port: must be 1-65535"},
        {[](const Config& c) { return c.ws_port == 0;             }, "invalid ws_port: must be 1-65535"},
        {[](const Config& c) { return c.tcp_port == c.ws_port;    }, "tcp_port and ws_port must be different"},
        {[](const Config& c) { return c.bind.empty();             }, "bind address must not be empty"},
        {[](const Config& c) { return c.data_dir.empty();         }, "data_dir must not be empty"},
        {[](const Config& c) { return c.replication_factor == 0;  }, "replication_factor must be >= 1"},
        {[](const Config& c) { return c.max_routing_table_size == 0; }, "max_routing_table_size must be >= 1"},
        {[](const Config& c) { return c.worker_pool_threads == 0; }, "worker_pool_threads must be >= 1"},
        {[](const Config& c) { return c.worker_pool_queue_max == 0; }, "worker_pool_queue_max must be >= 1"},
        {[](const Config& c) { return c.rate_limit_max <= 0.0;    }, "rate_limit_max must be > 0"},
        {[](const Config& c) { return c.rate_limit_refill <= 0.0; }, "rate_limit_refill must be > 0"},
        {[](const Config& c) { return c.max_message_size == 0;    }, "max_message_size must be > 0"},
        {[](const Config& c) { return c.ttl_days == 0;            }, "ttl_days must be >= 1"},
        {[](const Config& c) { return c.mdbx_max_size == 0;       }, "mdbx_max_size must be > 0"},
    };

    for (const auto& rule : rules) {
        if (rule.failed(cfg)) {
            throw std::runtime_error(rule.message);
        }
    }

    // TLS: both fields must be set or both empty
    bool has_cert = !cfg.tls_cert_path.empty();
    bool has_key = !cfg.tls_key_path.empty();
    if (has_cert != has_key) {
        throw std::runtime_error("tls_cert_path and tls_key_path must both be set or both empty");
    }
}

void generate_default_config(const std::filesystem::path& path) {
    Json::Value root;
    root["data_dir"] = ".";
    root["bind"] = "0.0.0.0";
    root["tcp_port"] = 4000;
    root["ws_port"] = 4001;

    Json::Value bootstrap(Json::arrayValue);
    bootstrap.append("0.bootstrap.cpunk.io:4000");
    bootstrap.append("1.bootstrap.cpunk.io:4000");
    bootstrap.append("2.bootstrap.cpunk.io:4000");
    root["bootstrap"] = bootstrap;

    // TLS (empty = plaintext WebSocket)
    root["tls_cert_path"] = "";
    root["tls_key_path"] = "";

    // Network
    root["external_address"] = "";
    root["replication_factor"] = 3;
    root["max_routing_table_size"] = 256;
    root["max_nodes_per_subnet"] = 3;

    // Timeouts (seconds)
    root["tcp_connect_timeout"] = 5;
    root["tcp_read_timeout"] = 5;
    root["ws_idle_timeout"] = 120;
    root["upload_timeout"] = 30;

    // Worker pool
    root["worker_pool_threads"] = 4;
    root["worker_pool_queue_max"] = 1024;

    // Rate limiter
    root["rate_limit_tokens"] = 50.0;
    root["rate_limit_max"] = 50.0;
    root["rate_limit_refill"] = 10.0;

    // Size limits
    root["max_message_size"] = Json::Value::UInt64(50ULL * 1024 * 1024);
    root["max_profile_size"] = 1024 * 1024;
    root["max_request_blob_size"] = 64 * 1024;

    // TTL & maintenance
    root["ttl_days"] = 7;
    root["compact_interval_minutes"] = 60;
    root["compact_keep_entries"] = 10000;
    root["compact_min_age_hours"] = 168;  // 7 days

    // PoW
    root["contact_pow_difficulty"] = 16;
    root["name_pow_difficulty"] = 28;

    // Sync
    root["sync_interval_seconds"] = 120;
    root["sync_batch_size"] = 10;

    // TCP transport
    root["max_tcp_clients"] = 256;

    // Connection pool
    root["conn_pool_max"] = 64;
    root["conn_pool_idle_seconds"] = 60;

    // Storage
    root["mdbx_max_size"] = Json::Value::UInt64(1ULL << 30);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        throw std::runtime_error("cannot write config file: " + path.string());
    }
    ofs << Json::writeString(writer, root) << '\n';
}

crypto::KeyPair load_or_generate_keypair(const std::filesystem::path& data_dir) {
    auto key_path = data_dir / "node.key";

    if (std::filesystem::exists(key_path)) {
        // Load existing keypair
        std::ifstream ifs(key_path, std::ios::binary);
        if (!ifs.is_open()) {
            throw std::runtime_error("cannot open key file: " + key_path.string());
        }

        auto read_u32 = [&]() -> uint32_t {
            uint8_t buf[4];
            ifs.read(reinterpret_cast<char*>(buf), 4);
            if (!ifs) throw std::runtime_error("truncated key file");
            return (static_cast<uint32_t>(buf[0]) << 24) |
                   (static_cast<uint32_t>(buf[1]) << 16) |
                   (static_cast<uint32_t>(buf[2]) << 8) |
                   static_cast<uint32_t>(buf[3]);
        };

        uint32_t pub_len = read_u32();
        uint32_t sec_len = read_u32();

        if (pub_len != crypto::PUBLIC_KEY_SIZE || sec_len != crypto::SECRET_KEY_SIZE) {
            throw std::runtime_error("invalid key file: unexpected key sizes");
        }

        crypto::KeyPair kp;
        kp.public_key.resize(pub_len);
        kp.secret_key.resize(sec_len);
        ifs.read(reinterpret_cast<char*>(kp.public_key.data()), pub_len);
        ifs.read(reinterpret_cast<char*>(kp.secret_key.data()), sec_len);
        if (!ifs) {
            throw std::runtime_error("truncated key file: " + key_path.string());
        }

        spdlog::info("loaded keypair from {}", key_path.string());
        return kp;
    }

    // Generate new keypair
    auto kp = crypto::generate_keypair();

    std::ofstream ofs(key_path, std::ios::binary);
    if (!ofs.is_open()) {
        throw std::runtime_error("cannot write key file: " + key_path.string());
    }

    auto write_u32 = [&](uint32_t val) {
        uint8_t buf[4] = {
            static_cast<uint8_t>(val >> 24),
            static_cast<uint8_t>(val >> 16),
            static_cast<uint8_t>(val >> 8),
            static_cast<uint8_t>(val)
        };
        ofs.write(reinterpret_cast<const char*>(buf), 4);
    };

    write_u32(static_cast<uint32_t>(kp.public_key.size()));
    write_u32(static_cast<uint32_t>(kp.secret_key.size()));
    ofs.write(reinterpret_cast<const char*>(kp.public_key.data()), kp.public_key.size());
    ofs.write(reinterpret_cast<const char*>(kp.secret_key.data()), kp.secret_key.size());

    // Restrict file permissions to owner-only (0600)
    std::filesystem::permissions(key_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);

    spdlog::info("generated new keypair at {}", key_path.string());
    return kp;
}

} // namespace chromatin::config
