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

    spdlog::info("generated new keypair at {}", key_path.string());
    return kp;
}

} // namespace chromatin::config
