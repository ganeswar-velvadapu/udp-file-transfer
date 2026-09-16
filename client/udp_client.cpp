#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <chrono>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

enum PacketType {
    START = 1,
    DATA = 2,
    ACK = 3
};

struct PacketHeader {
    uint32_t transaction_id;
    uint32_t chunk_number;
    uint8_t packet_type;
};

int main() {

    // File to send
    const char* filename = "testing_file.md";

    ifstream file(filename, ios::binary);

    if (!file) {
        cout << "Could not open file\n";
        return 1;
    }

    const int CHUNK_SIZE = 10;
    char buffer[CHUNK_SIZE];

    // Generate a transaction ID
    uint32_t transaction_id =
        static_cast<uint32_t>(
            chrono::system_clock::now()
                .time_since_epoch()
                .count()
        );

    // UDP socket
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (socket_fd < 0) {
        cout << "Could not create socket\n";
        return 1;
    }

    // Server address
    sockaddr_in server;

    server.sin_family = AF_INET;
    server.sin_port = htons(9000);
    server.sin_addr.s_addr = inet_addr("127.0.0.1");

    /*
        ------------------------------------------------
        Send START packet
        ------------------------------------------------
    */

    PacketHeader start_header;

    start_header.transaction_id = htonl(transaction_id);
    start_header.chunk_number = htonl(0);
    start_header.packet_type = START;

    char start_packet[
        sizeof(PacketHeader) + 256
    ];

    memcpy(
        start_packet,
        &start_header,
        sizeof(PacketHeader)
    );

    int filename_size = strlen(filename);

    memcpy(
        start_packet + sizeof(PacketHeader),
        filename,
        filename_size
    );

    int start_packet_size =
        sizeof(PacketHeader) + filename_size;

    sendto(
        socket_fd,
        start_packet,
        start_packet_size,
        0,
        (sockaddr*)&server,
        sizeof(server)
    );

    cout << "Started transfer\n";
    cout << "Transaction ID: "
         << transaction_id << "\n";
    cout << "Filename: "
         << filename << "\n";


    /*
        ------------------------------------------------
        Send DATA packets
        ------------------------------------------------
    */

    uint32_t chunk_number = 0;

    while (file.read(buffer, CHUNK_SIZE) ||
           file.gcount() > 0) {

        int bytes_read = file.gcount();

        cout << "Sending chunk "
             << chunk_number
             << " : "
             << bytes_read
             << " bytes\n";

        PacketHeader header;

        header.transaction_id =
            htonl(transaction_id);

        header.chunk_number =
            htonl(chunk_number);

        header.packet_type = DATA;

        char packet[
            sizeof(PacketHeader) + CHUNK_SIZE
        ];

        memcpy(
            packet,
            &header,
            sizeof(PacketHeader)
        );

        memcpy(
            packet + sizeof(PacketHeader),
            buffer,
            bytes_read
        );

        int packet_size =
            sizeof(PacketHeader) + bytes_read;

        sendto(
            socket_fd,
            packet,
            packet_size,
            0,
            (sockaddr*)&server,
            sizeof(server)
        );

        chunk_number++;
    }

    file.close();
    close(socket_fd);

    cout << "File transfer finished\n";

    return 0;
}