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

struct Transfer {
    string filename;
};

int main() {

    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (socket_fd < 0) {
        cout << "Could not create socket\n";
        return 1;
    }


    

    sockaddr_in server;

    server.sin_family = AF_INET;
    server.sin_port = htons(9000);
    server.sin_addr.s_addr = inet_addr("127.0.0.1");


   

    if (bind(socket_fd,(sockaddr*)&server,sizeof(server)) < 0) {

        cout << "Bind failed\n";

        close(socket_fd);
        return 1;
    }

    cout << "Server listening on port 9000\n";

    map<uint32_t, Transfer> transfers;


   // receive packets

    while (true) {

        char packet[sizeof(PacketHeader) + 256];

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
            continue;
        }
        // read header

        PacketHeader header;

        memcpy(
            &header,
            packet,
            sizeof(PacketHeader)
        );

        uint32_t transaction_id = ntohl(header.transaction_id);

        uint32_t chunk_number = ntohl(header.chunk_number);

        // start packet

        if (header.packet_type == START) {

            int filename_size =
                bytes_received - sizeof(PacketHeader);

            string filename(
                packet + sizeof(PacketHeader),
                filename_size
            );

            transfers[transaction_id].filename =
                filename;

            cout << "\n";
            cout << "Received START\n";
            cout << "Transaction ID: "
                 << transaction_id
                 << "\n";
            cout << "Filename: "
                 << filename
                 << "\n";

            continue;
        }

        // data packets
        if (header.packet_type == DATA) {


            if (
                transfers.find(transaction_id)
                == transfers.end()
            ) {

                cout << "Unknown transaction ID\n";
                continue;
            }


            int data_size =
                bytes_received - sizeof(PacketHeader);


            string filename =
                transfers[transaction_id].filename;


            cout << "Received DATA #"
                 << chunk_number
                 << " | "
                 << data_size
                 << " bytes\n";


            ofstream output_file(
                filename,
                ios::binary | ios::app
            );

            output_file.write(
                packet + sizeof(PacketHeader),
                data_size
            );

            output_file.close();
            
            PacketHeader ack_header;

            ack_header.transaction_id =
                htonl(transaction_id);

            ack_header.chunk_number =
                htonl(chunk_number);

            ack_header.packet_type = ACK;


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

            continue;
        }
    }


    close(socket_fd);

    return 0;
}