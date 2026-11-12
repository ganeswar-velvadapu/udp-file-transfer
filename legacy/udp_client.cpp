#include <iostream>
#include <fstream>
#include <vector>
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

struct Chunk {
    vector<char> data;
    bool acked = false;
    chrono::steady_clock::time_point send_time;
};

int main() {

    const int CHUNK_SIZE = 10;
    const int WINDOW_SIZE = 4;
    const int TIMEOUT_MS = 500;

    const char* filename = "readme.md";

    // read file into chunks 

    ifstream file(filename, ios::binary);

    if (!file) {
        cout << "Could not open file\n";
        return 1;
    }

    vector<Chunk> chunks;

    char buffer[CHUNK_SIZE];

    while (file.read(buffer, CHUNK_SIZE) ||
           file.gcount() > 0) {

        int bytes_read = file.gcount();

        Chunk chunk;

        chunk.data.assign(
            buffer,
            buffer + bytes_read
        );

        chunks.push_back(chunk);
    }

    file.close();

    int total_chunks = chunks.size();

    cout << "Total chunks: "
         << total_chunks << "\n";

    // create socket 
    
    int socket_fd = socket(
        AF_INET,
        SOCK_DGRAM,
        0
    );

    if (socket_fd < 0) {
        cout << "Could not create socket\n";
        return 1;
    }


    sockaddr_in server;

    server.sin_family = AF_INET;
    server.sin_port = htons(9000);
    server.sin_addr.s_addr =
        inet_addr("127.0.0.1");


 
    uint32_t transaction_id =
        static_cast<uint32_t>(
            chrono::system_clock::now()
                .time_since_epoch()
                .count()
        );


    // send start packet 
    
    PacketHeader start_header;

    start_header.transaction_id =
        htonl(transaction_id);

    start_header.chunk_number =
        htonl(0);

    start_header.packet_type =
        START;

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

    memcpy(
        start_packet +
        sizeof(PacketHeader) +
        filename_size,

        &total_chunks,
        sizeof(total_chunks)
    );

    int start_packet_size =
        sizeof(PacketHeader) +
        filename_size +
        sizeof(total_chunks);

    sendto(
        socket_fd,
        start_packet,
        start_packet_size,
        0,
        (sockaddr*)&server,
        sizeof(server)
    );

    cout << "Sent START\n";


    // --------------------------------
    // SELECTIVE REPEAT
    // --------------------------------

    uint32_t base = 0;
    uint32_t next_seq = 0;

    while (base < total_chunks) {

        while (
            next_seq < base + WINDOW_SIZE &&
            next_seq < total_chunks
        ) {

            PacketHeader header;

            header.transaction_id =
                htonl(transaction_id);

            header.chunk_number =
                htonl(next_seq);

            header.packet_type =
                DATA;


            char packet[
                sizeof(PacketHeader) +
                CHUNK_SIZE
            ];


            memcpy(
                packet,
                &header,
                sizeof(PacketHeader)
            );


            memcpy(
                packet + sizeof(PacketHeader),
                chunks[next_seq].data.data(),
                chunks[next_seq].data.size()
            );


            int packet_size =
                sizeof(PacketHeader) +
                chunks[next_seq].data.size();


            sendto(
                socket_fd,
                packet,
                packet_size,
                0,
                (sockaddr*)&server,
                sizeof(server)
            );


            chunks[next_seq].send_time =
                chrono::steady_clock::now();


            cout << "Sent DATA #"
                 << next_seq
                 << "\n";


            next_seq++;
        }


        fd_set readfds;

        FD_ZERO(&readfds);

        FD_SET(socket_fd, &readfds);


        timeval tv;

        tv.tv_sec = 0;
        tv.tv_usec = 100000;


        int ready = select(
            socket_fd + 1,
            &readfds,
            nullptr,
            nullptr,
            &tv
        );


        if (ready > 0) {

            char ack_packet[
                sizeof(PacketHeader)
            ];

            sockaddr_in ack_server;

            socklen_t server_size =
                sizeof(ack_server);


            int ack_bytes = recvfrom(
                socket_fd,
                ack_packet,
                sizeof(ack_packet),
                0,
                (sockaddr*)&ack_server,
                &server_size
            );


            if (
                ack_bytes >=
                sizeof(PacketHeader)
            ) {

                PacketHeader ack_header;

                memcpy(
                    &ack_header,
                    ack_packet,
                    sizeof(PacketHeader)
                );


                uint32_t ack_transaction_id =
                    ntohl(
                        ack_header.transaction_id
                    );


                uint32_t ack_chunk_number =
                    ntohl(
                        ack_header.chunk_number
                    );


                if (
                    ack_header.packet_type == ACK &&
                    ack_transaction_id == transaction_id &&
                    ack_chunk_number < total_chunks
                ) {

                    if (
                        !chunks[
                            ack_chunk_number
                        ].acked
                    ) {

                        chunks[
                            ack_chunk_number
                        ].acked = true;


                        cout << "Received ACK #"
                             << ack_chunk_number
                             << "\n";
                    }


                    // Move window forward

                    while (
                        base < total_chunks &&
                        chunks[base].acked
                    ) {

                        base++;
                    }
                }
            }
        }



        auto now =
            chrono::steady_clock::now();


        for (
            uint32_t i = base;
            i < next_seq;
            i++
        ) {

            if (chunks[i].acked) {
                continue;
            }


            auto elapsed =
                chrono::duration_cast<
                    chrono::milliseconds
                >(
                    now -
                    chunks[i].send_time
                ).count();


            if (elapsed >= TIMEOUT_MS) {

                PacketHeader header;

                header.transaction_id =
                    htonl(transaction_id);

                header.chunk_number =
                    htonl(i);

                header.packet_type =
                    DATA;


                char packet[
                    sizeof(PacketHeader) +
                    CHUNK_SIZE
                ];


                memcpy(
                    packet,
                    &header,
                    sizeof(PacketHeader)
                );


                memcpy(
                    packet + sizeof(PacketHeader),
                    chunks[i].data.data(),
                    chunks[i].data.size()
                );


                int packet_size =
                    sizeof(PacketHeader) +
                    chunks[i].data.size();


                sendto(
                    socket_fd,
                    packet,
                    packet_size,
                    0,
                    (sockaddr*)&server,
                    sizeof(server)
                );


                chunks[i].send_time =
                    chrono::steady_clock::now();


                cout << "TIMEOUT DATA #"
                     << i
                     << " -> retransmitting\n";
            }
        }
    }


    cout << "File transfer completed\n";

    close(socket_fd);

    return 0;
}