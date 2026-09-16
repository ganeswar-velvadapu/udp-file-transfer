#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

enum PacketType { START = 1, DATA = 2, ACK = 3 };

struct PacketHeader {
  uint32_t transaction_id;
  uint32_t chunk_number;
  uint8_t packet_type;
};

int main() {

  const int CHUNK_SIZE = 10;

  const char *filename = "testing_file.md";

  ifstream file(filename, ios::binary);

  if (!file) {
    cout << "Could not open file\n";
    return 1;
  }

  int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

  if (socket_fd < 0) {
    cout << "Could not create socket\n";
    return 1;
  }

  sockaddr_in server;
  server.sin_family = AF_INET;
  server.sin_port = htons(9000);
  server.sin_addr.s_addr = inet_addr("127.0.0.1");

  uint32_t transaction_id = static_cast<uint32_t>(
      chrono::system_clock::now().time_since_epoch().count());

  // send initial start packet

  PacketHeader start_header;

  start_header.transaction_id = htonl(transaction_id);
  start_header.chunk_number = htonl(0);
  start_header.packet_type = START;

  char start_packet[sizeof(PacketHeader) + 256];

  memcpy(start_packet, &start_header, sizeof(PacketHeader));

  int filename_size = strlen(filename);

  memcpy(start_packet + sizeof(PacketHeader), filename, filename_size);

  int start_packet_size = sizeof(PacketHeader) + filename_size;

  sendto(socket_fd, start_packet, start_packet_size, 0, (sockaddr *)&server,
         sizeof(server));

  cout << "Sent START | "
       << "Transaction ID: " << transaction_id << " | Filename: " << filename
       << "\n";

  // start sending data packets

  char buffer[CHUNK_SIZE];

  uint32_t chunk_number = 0;

  while (file.read(buffer, CHUNK_SIZE) || file.gcount() > 0) {

    int bytes_read = file.gcount();

    PacketHeader header;

    header.transaction_id = htonl(transaction_id);
    header.chunk_number = htonl(chunk_number);
    header.packet_type = DATA;

    char packet[sizeof(PacketHeader) + CHUNK_SIZE];

    memcpy(packet, &header, sizeof(PacketHeader));

    memcpy(packet + sizeof(PacketHeader), buffer, bytes_read);

    int packet_size = sizeof(PacketHeader) + bytes_read;

    // send data

    sendto(socket_fd, packet, packet_size, 0, (sockaddr *)&server,
           sizeof(server));

    cout << "Sent DATA #" << chunk_number << " | " << bytes_read << " bytes\n";

    char ack_packet[sizeof(PacketHeader)];

    sockaddr_in ack_server;
    socklen_t server_size = sizeof(ack_server);

    int ack_bytes = recvfrom(socket_fd, ack_packet, sizeof(ack_packet), 0,
                             (sockaddr *)&ack_server, &server_size);

    if (ack_bytes < 0) {
      cout << "Error receiving ACK\n";
      close(socket_fd);
      return 1;
    }

    // read acks 
    
    PacketHeader ack_header;

    memcpy(&ack_header, ack_packet, sizeof(PacketHeader));

    uint32_t ack_transaction_id = ntohl(ack_header.transaction_id);

    uint32_t ack_chunk_number = ntohl(ack_header.chunk_number);

    // check acks

    if (ack_header.packet_type == ACK && ack_transaction_id == transaction_id &&
        ack_chunk_number == chunk_number) {

      cout << "Received ACK #" << ack_chunk_number << "\n";

    } else {

      cout << "Invalid ACK\n";

      close(socket_fd);
      return 1;
    }

    chunk_number++;
  }

  file.close();
  close(socket_fd);

  cout << "File transfer completed\n";

  return 0;
}
