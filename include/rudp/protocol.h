#pragma once

// Wire format of the reliable-UDP file transfer protocol.
//
// Every datagram = 16-byte header + payload. All integers are big-endian.
//
//   0       1       2               4                               8
//   +-------+-------+---------------+-------------------------------+
//   |version| type  |  payload_len  |        transaction_id         |
//   +-------+-------+---------------+-------------------------------+
//   |          sequence             |       crc32(header+payload)   |
//   +-------------------------------+-------------------------------+
//   8                               12                              16
//
// Session flow:
//   client                                   server
//   START(file_size, chunks, chunk, win, name) ->
//                                         <- START_ACK(status, accepted window)
//   DATA(seq=i, bytes)  ... (Selective Repeat window) ...
//                                         <- ACK(seq=i)      (one per DATA)
//   FIN(file_crc, file_size)              ->
//                                         <- FIN_ACK(status)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rudp {

constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 16;
constexpr size_t kMaxDatagram = 65507;  // max IPv4 UDP payload
constexpr size_t kMaxPayload = kMaxDatagram - kHeaderSize;
// 1500 MTU - 20 IP - 8 UDP - 16 header = 1456; use a round number below it
// so DATA packets never need IP fragmentation on a real Ethernet link.
constexpr uint16_t kDefaultChunkSize = 1400;

enum class PacketType : uint8_t {
    Start = 1,
    StartAck = 2,
    Data = 3,
    Ack = 4,
    Fin = 5,
    FinAck = 6,
    Error = 7,
};

enum class Status : uint8_t {
    Ok = 0,
    Rejected = 1,          // server refused the transfer (bad name, I/O error...)
    ChecksumMismatch = 2,  // reassembled file does not match sender's CRC
    UnknownTransaction = 3,
};

const char* to_string(PacketType type);
const char* to_string(Status status);

struct Header {
    PacketType type = PacketType::Data;
    uint32_t transaction_id = 0;
    uint32_t seq = 0;
};

// A decoded datagram. `payload` points into the buffer passed to decode().
struct PacketView {
    Header header;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
};

// Serialises header + payload into `out` (which must hold kHeaderSize + len
// bytes) and fills in the CRC. Returns the datagram length.
size_t encode_packet(const Header& header, const uint8_t* payload, size_t len, uint8_t* out);

std::vector<uint8_t> encode_packet(const Header& header, const std::vector<uint8_t>& payload = {});

// Validates version, length and CRC. Returns nullopt for anything malformed
// or corrupted, which the caller treats exactly like a lost packet.
std::optional<PacketView> decode_packet(const uint8_t* data, size_t len);

// ---- control payloads ---------------------------------------------------

struct StartPayload {
    uint64_t file_size = 0;
    uint32_t total_chunks = 0;
    uint16_t chunk_size = kDefaultChunkSize;
    uint16_t window = 0;  // sender's requested window (packets)
    std::string filename;

    std::vector<uint8_t> encode() const;
    static std::optional<StartPayload> decode(const uint8_t* p, size_t len);
};

struct StartAckPayload {
    Status status = Status::Ok;
    uint16_t window = 0;  // receiver-granted window (flow control)

    std::vector<uint8_t> encode() const;
    static std::optional<StartAckPayload> decode(const uint8_t* p, size_t len);
};

struct FinPayload {
    uint32_t file_crc = 0;
    uint64_t file_size = 0;

    std::vector<uint8_t> encode() const;
    static std::optional<FinPayload> decode(const uint8_t* p, size_t len);
};

struct StatusPayload {  // FIN_ACK and ERROR
    Status status = Status::Ok;

    std::vector<uint8_t> encode() const;
    static std::optional<StatusPayload> decode(const uint8_t* p, size_t len);
};

// Number of chunks needed for a file (an empty file has zero chunks).
inline uint32_t chunk_count(uint64_t file_size, uint16_t chunk_size) {
    return static_cast<uint32_t>((file_size + chunk_size - 1) / chunk_size);
}

// Expected payload length of chunk `seq`.
inline size_t chunk_length(uint32_t seq, uint64_t file_size, uint16_t chunk_size) {
    uint64_t offset = static_cast<uint64_t>(seq) * chunk_size;
    uint64_t remaining = file_size > offset ? file_size - offset : 0;
    return static_cast<size_t>(remaining < chunk_size ? remaining : chunk_size);
}

}  // namespace rudp
