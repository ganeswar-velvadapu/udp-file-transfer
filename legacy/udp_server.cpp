#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <cstring>

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

struct Transfer {
    string filename;
    int total_chunks;

    map<
        uint32_t,
        vector<char>
    > chunks;
};

int main() {

    // --------------------------------
    // CREATE SOCKET
    // --------------------------------

    int socket_fd = socket(
        AF_INET,
        SOCK_DGRAM,
        0
    );

    if (socket_fd < 0) {
        cout << "Could not create socket\n";
        return 1;
    }


    // --------------------------------
    // SERVER ADDRESS
    // --------------------------------

    sockaddr_in server;

    server.sin_family = AF_INET;
    server.sin_port = htons(9000);
    server.sin_addr.s_addr =
        inet_addr("127.0.0.1");


    // --------------------------------
    // BIND
    // --------------------------------

    if (
        bind(
            socket_fd,
            (sockaddr*)&server,
            sizeof(server)
        ) < 0
    ) {

        cout << "Bind failed\n";

        close(socket_fd);
        return 1;
    }


    cout << "Server listening on port 9000\n";


    // --------------------------------
    // STORE TRANSFERS
    // --------------------------------

    map<
        uint32_t,
        Transfer
    > transfers;


    // --------------------------------
    // RECEIVE PACKETS
    // --------------------------------

    while (true) {

        char packet[
            sizeof(PacketHeader) + 256
        ];

        sockaddr_in client;

        socklen_t client_size =
            sizeof(client);


        int bytes_received =
            recvfrom(
                socket_fd,
                packet,
                sizeof(packet),
                0,
                (sockaddr*)&client,
                &client_size
            );


        if (bytes_received < 0) {

            cout << "Error receiving packet\n";
            continue;
        }


        // --------------------------------
        // READ HEADER
        // --------------------------------

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


        // --------------------------------
        // START PACKET
        // --------------------------------

        if (
            header.packet_type == START
        ) {

            int filename_size =
                bytes_received -
                sizeof(PacketHeader) -
                sizeof(int);


            string filename(
                packet + sizeof(PacketHeader),
                filename_size
            );


            int total_chunks;

            memcpy(
                &total_chunks,

                packet +
                sizeof(PacketHeader) +
                filename_size,

                sizeof(total_chunks)
            );


            Transfer transfer;

            transfer.filename =
                filename;

            transfer.total_chunks =
                total_chunks;


            transfers[
                transaction_id
            ] = transfer;


            cout << "\n";
            cout << "START received\n";
            cout << "Filename: "
                 << filename
                 << "\n";
            cout << "Total chunks: "
                 << total_chunks
                 << "\n";

            continue;
        }


        // --------------------------------
        // DATA PACKET
        // --------------------------------

        if (
            header.packet_type == DATA
        ) {

            if (
                transfers.find(
                    transaction_id
                ) == transfers.end()
            ) {

                cout << "Unknown transaction ID\n";
                continue;
            }


            Transfer& transfer =
                transfers[transaction_id];


            int data_size =
                bytes_received -
                sizeof(PacketHeader);


            // --------------------------------
            // DUPLICATE CHECK
            // --------------------------------

            if (
                transfer.chunks.find(
                    chunk_number
                ) ==
                transfer.chunks.end()
            ) {

                vector<char> data(
                    packet +
                    sizeof(PacketHeader),

                    packet +
                    sizeof(PacketHeader) +
                    data_size
                );


                transfer.chunks[
                    chunk_number
                ] = data;


                cout << "Received DATA #"
                     << chunk_number
                     << "\n";

            } else {

                cout << "Duplicate DATA #"
                     << chunk_number
                     << "\n";
            }


            // --------------------------------
            // SEND ACK
            // --------------------------------

            PacketHeader ack_header;

            ack_header.transaction_id =
                htonl(transaction_id);

            ack_header.chunk_number =
                htonl(chunk_number);

            ack_header.packet_type =
                ACK;


            sendto(
                socket_fd,
                &ack_header,
                sizeof(PacketHeader),
                0,
                (sockaddr*)&client,
                client_size
            );


            cout << "Sent ACK #"
                 << chunk_number
                 << "\n";


            // --------------------------------
            // CHECK IF COMPLETE
            // --------------------------------

            if (
                transfer.chunks.size() ==
                transfer.total_chunks
            ) {

                cout << "All chunks received\n";


                string output_filename =
                    "received_" +
                    transfer.filename;


                ofstream output_file(
                    output_filename,
                    ios::binary
                );


                // Write in correct order

                for (
                    uint32_t i = 0;
                    i < transfer.total_chunks;
                    i++
                ) {

                    vector<char>& data =
                        transfer.chunks[i];


                    output_file.write(
                        data.data(),
                        data.size()
                    );
                }


                output_file.close();


                cout << "File reconstructed: "
                     << output_filename
                     << "\n";


                transfers.erase(
                    transaction_id
                );
            }
        }
    }


    close(socket_fd);

    return 0;
}
