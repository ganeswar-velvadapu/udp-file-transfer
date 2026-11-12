#include "rudp/protocol.h"

#include <cstring>
#include <stdexcept>

#include "rudp/crc32.h"

namespace rudp {

namespace {

void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | p[i];
    return v;
}
uint64_t get64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

bool valid_type(uint8_t t) {
    return t >= static_cast<uint8_t>(PacketType::Start) && t <= static_cast<uint8_t>(PacketType::Error);
}

bool valid_status(uint8_t s) { return s <= static_cast<uint8_t>(Status::UnknownTransaction); }

}  // namespace

const char* to_string(PacketType type) {
    switch (type) {
        case PacketType::Start: return "START";
        case PacketType::StartAck: return "START_ACK";
        case PacketType::Data: return "DATA";
        case PacketType::Ack: return "ACK";
        case PacketType::Fin: return "FIN";
        case PacketType::FinAck: return "FIN_ACK";
        case PacketType::Error: return "ERROR";
    }
    return "?";
}

const char* to_string(Status status) {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::Rejected: return "rejected";
        case Status::ChecksumMismatch: return "checksum mismatch";
        case Status::UnknownTransaction: return "unknown transaction";
    }
    return "?";
}

size_t encode_packet(const Header& header, const uint8_t* payload, size_t len, uint8_t* out) {
    if (len > kMaxPayload) throw std::length_error("payload too large");
    out[0] = kProtocolVersion;
    out[1] = static_cast<uint8_t>(header.type);
    put16(out + 2, static_cast<uint16_t>(len));
    put32(out + 4, header.transaction_id);
    put32(out + 8, header.seq);
    put32(out + 12, 0);
    if (len > 0) std::memcpy(out + kHeaderSize, payload, len);
    put32(out + 12, crc32(out, kHeaderSize + len));
    return kHeaderSize + len;
}

std::vector<uint8_t> encode_packet(const Header& header, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out(kHeaderSize + payload.size());
    encode_packet(header, payload.data(), payload.size(), out.data());
    return out;
}

std::optional<PacketView> decode_packet(const uint8_t* data, size_t len) {
    if (len < kHeaderSize) return std::nullopt;
    if (data[0] != kProtocolVersion || !valid_type(data[1])) return std::nullopt;
    size_t payload_len = get16(data + 2);
    if (kHeaderSize + payload_len != len) return std::nullopt;

    uint8_t header[kHeaderSize];
    std::memcpy(header, data, kHeaderSize);
    put32(header + 12, 0);
    uint32_t crc = crc32(header, kHeaderSize);
    crc = crc32_update(crc, data + kHeaderSize, payload_len);
    if (crc != get32(data + 12)) return std::nullopt;

    PacketView view;
    view.header.type = static_cast<PacketType>(data[1]);
    view.header.transaction_id = get32(data + 4);
    view.header.seq = get32(data + 8);
    view.payload = data + kHeaderSize;
    view.payload_len = payload_len;
    return view;
}

// ---- StartPayload: u64 size | u32 chunks | u16 chunk | u16 window | u16 name_len | name

std::vector<uint8_t> StartPayload::encode() const {
    if (filename.size() > 1024) throw std::length_error("filename too long");
    std::vector<uint8_t> out(18 + filename.size());
    put64(out.data(), file_size);
    put32(out.data() + 8, total_chunks);
    put16(out.data() + 12, chunk_size);
    put16(out.data() + 14, window);
    put16(out.data() + 16, static_cast<uint16_t>(filename.size()));
    std::memcpy(out.data() + 18, filename.data(), filename.size());
    return out;
}

std::optional<StartPayload> StartPayload::decode(const uint8_t* p, size_t len) {
    if (len < 18) return std::nullopt;
    StartPayload s;
    s.file_size = get64(p);
    s.total_chunks = get32(p + 8);
    s.chunk_size = get16(p + 12);
    s.window = get16(p + 14);
    size_t name_len = get16(p + 16);
    if (18 + name_len != len) return std::nullopt;
    s.filename.assign(reinterpret_cast<const char*>(p + 18), name_len);
    return s;
}

// ---- StartAckPayload: u8 status | u16 window

std::vector<uint8_t> StartAckPayload::encode() const {
    std::vector<uint8_t> out(3);
    out[0] = static_cast<uint8_t>(status);
    put16(out.data() + 1, window);
    return out;
}

std::optional<StartAckPayload> StartAckPayload::decode(const uint8_t* p, size_t len) {
    if (len != 3 || !valid_status(p[0])) return std::nullopt;
    return StartAckPayload{static_cast<Status>(p[0]), get16(p + 1)};
}

// ---- FinPayload: u32 crc | u64 size

std::vector<uint8_t> FinPayload::encode() const {
    std::vector<uint8_t> out(12);
    put32(out.data(), file_crc);
    put64(out.data() + 4, file_size);
    return out;
}

std::optional<FinPayload> FinPayload::decode(const uint8_t* p, size_t len) {
    if (len != 12) return std::nullopt;
    return FinPayload{get32(p), get64(p + 4)};
}

// ---- StatusPayload: u8 status

std::vector<uint8_t> StatusPayload::encode() const { return {static_cast<uint8_t>(status)}; }

std::optional<StatusPayload> StatusPayload::decode(const uint8_t* p, size_t len) {
    if (len != 1 || !valid_status(p[0])) return std::nullopt;
    return StatusPayload{static_cast<Status>(p[0])};
}

}  // namespace rudp
