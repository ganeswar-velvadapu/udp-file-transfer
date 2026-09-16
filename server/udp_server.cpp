#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

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

    // Map:
    // transaction ID -> filename

    map<uint32_t, string> transfers;

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

    // Bind
    if (bind(
            socket_fd,
            (sockaddr*)&server,
            sizeof(server)) < 0) {

        cout << "Could not bind socket\n";
        close(socket_fd);
        return 1;
    }

    cout << "Server listening on port 9000...\n";


    while (true) {

        char packet[
            sizeof(PacketHeader) + 256
        ];

        sockaddr_in client;
        socklen_t client_size =
            sizeof(client);

        int bytes_received = recvfrom(
            socket_fd,
            packet,
            sizeof(packet),
            0,
            (sockaddr*)&client,
            &client_size
        );

        if (bytes_received < 0) {
            cout << "Error receiving packet\n";
            break;
        }


        /*
            ----------------------------------------
            Extract header
            ----------------------------------------
        */

        PacketHeader header;

        memcpy(
            &header,
            packet,
            sizeof(PacketHeader)
        );

        uint32_t transaction_id =
            ntohl(header.transaction_id);

        uint32_t chunk_number =
            ntohl(header.chunk_number);


        /*
            ----------------------------------------
            START packet
            ----------------------------------------
        */

        if (header.packet_type == START) {

            int filename_size =
                bytes_received -
                sizeof(PacketHeader);

            string filename(
                packet + sizeof(PacketHeader),
                filename_size
            );

            transfers[transaction_id] =
                filename;

            cout << "\nNew transfer\n";

            cout << "Transaction ID: "
                 << transaction_id
                 << "\n";

            cout << "Filename: "
                 << filename
                 << "\n";

            continue;
        }


        /*
            ----------------------------------------
            DATA packet
            ----------------------------------------
        */

        if (header.packet_type == DATA) {

            // Check whether this transaction exists

            if (transfers.find(transaction_id)
                == transfers.end()) {

                cout << "Unknown transaction ID: "
                     << transaction_id
                     << "\n";

                continue;
            }

            string filename =
                transfers[transaction_id];

            int data_size =
                bytes_received -
                sizeof(PacketHeader);

            cout << "Received chunk "
                 << chunk_number
                 << " | Transaction ID: "
                 << transaction_id
                 << " | "
                 << data_size
                 << " bytes\n";


            /*
                For now we simply append the
                received data.

                Later we will replace this with
                chunk buffering for Selective Repeat.
            */

            ofstream output_file(
                filename,
                ios::binary | ios::app
            );

            if (!output_file) {
                cout << "Could not open output file\n";
                continue;
            }

            output_file.write(
                packet + sizeof(PacketHeader),
                data_size
            );

            output_file.close();

            continue;
        }
    }

    close(socket_fd);

    return 0;
}