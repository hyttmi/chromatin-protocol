#include <catch2/catch_test_macros.hpp>

#include "relay/ws/ws_frame.h"

#include <cstring>
#include <vector>

using namespace chromatindb::relay::ws;

// ============================================================================
// encode_frame tests
// ============================================================================

TEST_CASE("WS Frame: encode text frame with empty payload", "[ws_frame]") {
    std::string frame = encode_frame(OPCODE_TEXT, {});
    REQUIRE(frame.size() == 2);
    auto* b = reinterpret_cast<const uint8_t*>(frame.data());
    REQUIRE(b[0] == 0x81);  // FIN=1, opcode=text
    REQUIRE(b[1] == 0x00);  // No mask, length=0
}

TEST_CASE("WS Frame: encode text frame with 5-byte payload", "[ws_frame]") {
    std::vector<uint8_t> payload = {'h', 'e', 'l', 'l', 'o'};
    std::string frame = encode_frame(OPCODE_TEXT, payload);
    REQUIRE(frame.size() == 7);
    auto* b = reinterpret_cast<const uint8_t*>(frame.data());
    REQUIRE(b[0] == 0x81);  // FIN=1, opcode=text
    REQUIRE(b[1] == 0x05);  // length=5, no mask
    REQUIRE(b[2] == 'h');
    REQUIRE(b[3] == 'e');
    REQUIRE(b[4] == 'l');
    REQUIRE(b[5] == 'l');
    REQUIRE(b[6] == 'o');
}

TEST_CASE("WS Frame: encode binary frame with 200-byte payload uses 16-bit length", "[ws_frame]") {
    std::vector<uint8_t> payload(200, 0xAB);
    std::string frame = encode_frame(OPCODE_BINARY, payload);
    REQUIRE(frame.size() == 4 + 200);  // 2 header + 2 extended length + payload
    auto* b = reinterpret_cast<const uint8_t*>(frame.data());
    REQUIRE(b[0] == 0x82);  // FIN=1, opcode=binary
    REQUIRE(b[1] == 126);   // 16-bit length follows
    // Big-endian 200
    REQUIRE(b[2] == 0x00);
    REQUIRE(b[3] == 200);
}

TEST_CASE("WS Frame: encode frame with 70000-byte payload uses 64-bit length", "[ws_frame]") {
    std::vector<uint8_t> payload(70000, 0xCD);
    std::string frame = encode_frame(OPCODE_BINARY, payload);
    REQUIRE(frame.size() == 10 + 70000);  // 2 header + 8 extended length + payload
    auto* b = reinterpret_cast<const uint8_t*>(frame.data());
    REQUIRE(b[0] == 0x82);  // FIN=1, opcode=binary
    REQUIRE(b[1] == 127);   // 64-bit length follows
    // Big-endian 70000
    uint64_t decoded_len = 0;
    for (int i = 0; i < 8; ++i) {
        decoded_len = (decoded_len << 8) | b[2 + i];
    }
    REQUIRE(decoded_len == 70000);
}

TEST_CASE("WS Frame: encode continuation frame with fin=false", "[ws_frame]") {
    std::vector<uint8_t> payload = {'d', 'a', 't', 'a'};
    std::string frame = encode_frame(OPCODE_CONTINUATION, payload, false);
    auto* b = reinterpret_cast<const uint8_t*>(frame.data());
    REQUIRE((b[0] & 0x80) == 0x00);  // FIN=0
    REQUIRE((b[0] & 0x0F) == OPCODE_CONTINUATION);
}

TEST_CASE("WS Frame: encode ping frame with payload", "[ws_frame]") {
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    std::string frame = encode_frame(OPCODE_PING, payload);
    auto* b = reinterpret_cast<const uint8_t*>(frame.data());
    REQUIRE(b[0] == 0x89);  // FIN=1, opcode=ping
    REQUIRE(b[1] == 3);
}

TEST_CASE("WS Frame: encode close frame with status code and reason", "[ws_frame]") {
    std::string frame = encode_close_frame(CLOSE_NORMAL, "goodbye");
    auto* b = reinterpret_cast<const uint8_t*>(frame.data());
    REQUIRE(b[0] == 0x88);  // FIN=1, opcode=close
    REQUIRE(b[1] == 9);     // 2 bytes status + 7 bytes reason
    // Big-endian 1000
    REQUIRE(b[2] == 0x03);
    REQUIRE(b[3] == 0xE8);
    // Reason text
    REQUIRE(std::string(frame.data() + 4, 7) == "goodbye");
}

TEST_CASE("WS Frame: server frames never have MASK bit set", "[ws_frame]") {
    std::vector<uint8_t> payload(50, 'x');
    std::string frame = encode_frame(OPCODE_TEXT, payload);
    auto* b = reinterpret_cast<const uint8_t*>(frame.data());
    REQUIRE((b[1] & 0x80) == 0x00);  // MASK bit must be 0
}

// ============================================================================
// parse_frame_header tests
// ============================================================================

TEST_CASE("WS Frame: parse 7-bit length frame header", "[ws_frame]") {
    // Masked client frame: FIN=1, opcode=text, MASK=1, length=5
    uint8_t buf[] = {
        0x81,  // FIN=1, opcode=text
        0x85,  // MASK=1, length=5
        0x37, 0xFA, 0x21, 0x3D  // mask key
    };
    auto hdr = parse_frame_header(buf);
    REQUIRE(hdr.has_value());
    REQUIRE(hdr->fin == true);
    REQUIRE(hdr->opcode == OPCODE_TEXT);
    REQUIRE(hdr->masked == true);
    REQUIRE(hdr->payload_length == 5);
    REQUIRE(hdr->header_size == 6);  // 2 base + 4 mask
    REQUIRE(hdr->mask_key[0] == 0x37);
    REQUIRE(hdr->mask_key[1] == 0xFA);
    REQUIRE(hdr->mask_key[2] == 0x21);
    REQUIRE(hdr->mask_key[3] == 0x3D);
}

TEST_CASE("WS Frame: parse 16-bit length frame header", "[ws_frame]") {
    // Masked client frame: FIN=1, opcode=text, MASK=1, length=300
    uint8_t buf[] = {
        0x81,        // FIN=1, opcode=text
        0xFE,        // MASK=1, length=126 (16-bit follows)
        0x01, 0x2C,  // Big-endian 300
        0xAA, 0xBB, 0xCC, 0xDD  // mask key
    };
    auto hdr = parse_frame_header(buf);
    REQUIRE(hdr.has_value());
    REQUIRE(hdr->payload_length == 300);
    REQUIRE(hdr->header_size == 8);  // 2 base + 2 extended + 4 mask
}

TEST_CASE("WS Frame: parse 64-bit length frame header", "[ws_frame]") {
    // Masked client frame: FIN=1, opcode=binary, MASK=1, length=70000
    uint8_t buf[14] = {};
    buf[0] = 0x82;  // FIN=1, opcode=binary
    buf[1] = 0xFF;  // MASK=1, length=127 (64-bit follows)
    // Big-endian 70000 in bytes 2-9
    uint64_t len = 70000;
    for (int i = 7; i >= 0; --i) {
        buf[2 + (7 - i)] = static_cast<uint8_t>((len >> (i * 8)) & 0xFF);
    }
    buf[10] = 0x11; buf[11] = 0x22; buf[12] = 0x33; buf[13] = 0x44;  // mask key

    auto hdr = parse_frame_header(buf);
    REQUIRE(hdr.has_value());
    REQUIRE(hdr->payload_length == 70000);
    REQUIRE(hdr->header_size == 14);  // 2 base + 8 extended + 4 mask
}

TEST_CASE("WS Frame: parse masked frame extracts correct mask key", "[ws_frame]") {
    uint8_t buf[] = {
        0x81,  // FIN=1, text
        0x83,  // MASK=1, length=3
        0xDE, 0xAD, 0xBE, 0xEF  // mask key
    };
    auto hdr = parse_frame_header(buf);
    REQUIRE(hdr.has_value());
    REQUIRE(hdr->masked == true);
    REQUIRE(hdr->mask_key[0] == 0xDE);
    REQUIRE(hdr->mask_key[1] == 0xAD);
    REQUIRE(hdr->mask_key[2] == 0xBE);
    REQUIRE(hdr->mask_key[3] == 0xEF);
}

TEST_CASE("WS Frame: parse unmasked frame detected as unmasked", "[ws_frame]") {
    // Server-to-client frame (no mask)
    uint8_t buf[] = {
        0x81,  // FIN=1, text
        0x05   // MASK=0, length=5
    };
    auto hdr = parse_frame_header(buf);
    REQUIRE(hdr.has_value());
    REQUIRE(hdr->masked == false);
    REQUIRE(hdr->payload_length == 5);
    REQUIRE(hdr->header_size == 2);  // No mask key
}

TEST_CASE("WS Frame: parse insufficient buffer returns nullopt", "[ws_frame]") {
    // Only 1 byte -- need at least 2
    uint8_t buf[] = {0x81};
    auto hdr = parse_frame_header(std::span<const uint8_t>(buf, 1));
    REQUIRE_FALSE(hdr.has_value());
}

TEST_CASE("WS Frame: parse insufficient buffer for 16-bit length returns nullopt", "[ws_frame]") {
    // Has base header but missing extended length bytes
    uint8_t buf[] = {0x81, 0xFE, 0x01};  // Need 2 more bytes for full 16-bit length + mask
    auto hdr = parse_frame_header(std::span<const uint8_t>(buf, 3));
    REQUIRE_FALSE(hdr.has_value());
}

TEST_CASE("WS Frame: parse FIN=0 continuation frame", "[ws_frame]") {
    uint8_t buf[] = {
        0x00,  // FIN=0, opcode=continuation
        0x83,  // MASK=1, length=3
        0x01, 0x02, 0x03, 0x04
    };
    auto hdr = parse_frame_header(buf);
    REQUIRE(hdr.has_value());
    REQUIRE(hdr->fin == false);
    REQUIRE(hdr->opcode == OPCODE_CONTINUATION);
}

// ============================================================================
// apply_mask tests
// ============================================================================

TEST_CASE("WS Frame: apply_mask unmasks hello with known key", "[ws_frame]") {
    // "hello" masked with key [0x37, 0xFA, 0x21, 0x3D]
    // h=0x68 ^ 0x37 = 0x5F, e=0x65 ^ 0xFA = 0x9F, l=0x6C ^ 0x21 = 0x4D,
    // l=0x6C ^ 0x3D = 0x51, o=0x6F ^ 0x37 = 0x58
    uint8_t masked[] = {0x5F, 0x9F, 0x4D, 0x51, 0x58};
    uint8_t mask_key[] = {0x37, 0xFA, 0x21, 0x3D};
    apply_mask(masked, mask_key);
    REQUIRE(masked[0] == 'h');
    REQUIRE(masked[1] == 'e');
    REQUIRE(masked[2] == 'l');
    REQUIRE(masked[3] == 'l');
    REQUIRE(masked[4] == 'o');
}

TEST_CASE("WS Frame: apply_mask twice restores original (round-trip)", "[ws_frame]") {
    uint8_t original[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x21};
    uint8_t data[6];
    std::memcpy(data, original, 6);
    uint8_t mask_key[] = {0xAA, 0xBB, 0xCC, 0xDD};

    apply_mask(data, mask_key);
    // After first mask: data should be different
    bool changed = false;
    for (int i = 0; i < 6; ++i) {
        if (data[i] != original[i]) changed = true;
    }
    REQUIRE(changed);

    apply_mask(data, mask_key);
    // After second mask: data should be restored
    for (int i = 0; i < 6; ++i) {
        REQUIRE(data[i] == original[i]);
    }
}

TEST_CASE("WS Frame: apply_mask on empty payload is no-op", "[ws_frame]") {
    uint8_t mask_key[] = {0x01, 0x02, 0x03, 0x04};
    apply_mask(std::span<uint8_t>{}, mask_key);
    // No crash, no-op
}

// ============================================================================
// is_control_opcode tests
// ============================================================================

TEST_CASE("WS Frame: is_control_opcode identifies control frames", "[ws_frame]") {
    REQUIRE(is_control_opcode(OPCODE_CLOSE));
    REQUIRE(is_control_opcode(OPCODE_PING));
    REQUIRE(is_control_opcode(OPCODE_PONG));
    REQUIRE_FALSE(is_control_opcode(OPCODE_TEXT));
    REQUIRE_FALSE(is_control_opcode(OPCODE_BINARY));
    REQUIRE_FALSE(is_control_opcode(OPCODE_CONTINUATION));
}

// ============================================================================
// FragmentAssembler tests
// ============================================================================

TEST_CASE("WS Frame: FragmentAssembler single FIN=1 text frame completes immediately", "[ws_frame]") {
    FragmentAssembler asm_;
    std::vector<uint8_t> payload = {'h', 'i'};
    auto msg = asm_.feed(OPCODE_TEXT, true, payload);
    REQUIRE(msg.result == AssemblyResult::COMPLETE);
    REQUIRE(msg.opcode == OPCODE_TEXT);
    REQUIRE(msg.data == payload);
}

TEST_CASE("WS Frame: FragmentAssembler multi-fragment reassembly", "[ws_frame]") {
    FragmentAssembler asm_;

    // Fragment 1: FIN=0, opcode=text, payload="hel"
    std::vector<uint8_t> p1 = {'h', 'e', 'l'};
    auto r1 = asm_.feed(OPCODE_TEXT, false, p1);
    REQUIRE(r1.result == AssemblyResult::INCOMPLETE);

    // Fragment 2: FIN=0, opcode=continuation, payload="lo"
    std::vector<uint8_t> p2 = {'l', 'o'};
    auto r2 = asm_.feed(OPCODE_CONTINUATION, false, p2);
    REQUIRE(r2.result == AssemblyResult::INCOMPLETE);

    // Fragment 3: FIN=1, opcode=continuation, payload=" world"
    std::vector<uint8_t> p3 = {' ', 'w', 'o', 'r', 'l', 'd'};
    auto r3 = asm_.feed(OPCODE_CONTINUATION, true, p3);
    REQUIRE(r3.result == AssemblyResult::COMPLETE);
    REQUIRE(r3.opcode == OPCODE_TEXT);
    std::vector<uint8_t> expected = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    REQUIRE(r3.data == expected);
}

TEST_CASE("WS Frame: FragmentAssembler rejects oversized message", "[ws_frame]") {
    FragmentAssembler asm_;

    // Start a text message fragment that exceeds MAX_TEXT_MESSAGE_SIZE
    std::vector<uint8_t> huge(MAX_TEXT_MESSAGE_SIZE + 1, 'x');
    auto r = asm_.feed(OPCODE_TEXT, true, huge);
    REQUIRE(r.result == AssemblyResult::ERROR);
}

TEST_CASE("WS Frame: FragmentAssembler control frame interleaved in fragments", "[ws_frame]") {
    FragmentAssembler asm_;

    // Start fragmented text message
    std::vector<uint8_t> p1 = {'h', 'e', 'l'};
    auto r1 = asm_.feed(OPCODE_TEXT, false, p1);
    REQUIRE(r1.result == AssemblyResult::INCOMPLETE);

    // Interleaved ping (control frame)
    std::vector<uint8_t> ping_data = {0x01, 0x02};
    auto r_ping = asm_.feed(OPCODE_PING, true, ping_data);
    REQUIRE(r_ping.result == AssemblyResult::CONTROL);
    REQUIRE(r_ping.opcode == OPCODE_PING);
    REQUIRE(r_ping.data == ping_data);

    // Continue with final fragment
    std::vector<uint8_t> p2 = {'l', 'o'};
    auto r2 = asm_.feed(OPCODE_CONTINUATION, true, p2);
    REQUIRE(r2.result == AssemblyResult::COMPLETE);
    REQUIRE(r2.opcode == OPCODE_TEXT);
    std::vector<uint8_t> expected = {'h', 'e', 'l', 'l', 'o'};
    REQUIRE(r2.data == expected);
}

TEST_CASE("WS Frame: FragmentAssembler unexpected continuation without start is error", "[ws_frame]") {
    FragmentAssembler asm_;

    std::vector<uint8_t> payload = {'d', 'a', 't', 'a'};
    auto r = asm_.feed(OPCODE_CONTINUATION, true, payload);
    REQUIRE(r.result == AssemblyResult::ERROR);
}

TEST_CASE("WS Frame: FragmentAssembler non-control frame during fragment is error", "[ws_frame]") {
    FragmentAssembler asm_;

    // Start a text fragment
    std::vector<uint8_t> p1 = {'a'};
    auto r1 = asm_.feed(OPCODE_TEXT, false, p1);
    REQUIRE(r1.result == AssemblyResult::INCOMPLETE);

    // Try to start another text message while fragment in progress
    std::vector<uint8_t> p2 = {'b'};
    auto r2 = asm_.feed(OPCODE_TEXT, false, p2);
    REQUIRE(r2.result == AssemblyResult::ERROR);
}
