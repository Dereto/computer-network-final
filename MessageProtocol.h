#ifndef MESSAGE_PROTOCOL_H
#define MESSAGE_PROTOCOL_H

#include <iostream>
#include <string>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <map>

enum Operation {
	LOGIN,
	LOGOUT,
	REGISTER,
	MSG,
	CONNECT_TO,
	SYNC_USER,
	CLIENT_INIT,	// client: CLIENT_INIT <listen_port>
	SUCCESS,
	FAIL,
	EXIT
};

ssize_t send_message(int sockfd, enum Operation op_code, const std::string &message) {
    uint32_t message_length = message.size();
    uint32_t network_order_length = htonl(message_length + sizeof(op_code));

    // Send the length header
    ssize_t bytes_sent = send(sockfd, &network_order_length, sizeof(network_order_length), 0);
    if (bytes_sent == -1) {
        return -1;  // Error sending length
    }
    
    ssize_t totalbytes_sent = bytes_sent;
	bytes_sent = send(sockfd, &op_code, sizeof(op_code), 0);
    if (bytes_sent == -1) {
        return -2;  // Error sending message content
    }
    totalbytes_sent += bytes_sent;

    const char *message_data = message.c_str();
    while (message_length > 0) {
        bytes_sent = send(sockfd, message_data, message_length, 0);
        if (bytes_sent == -1) {
            return -2;  // Error sending message content
        }
        message_length -= bytes_sent;
        message_data += bytes_sent;
        totalbytes_sent += bytes_sent;
    }

    return totalbytes_sent;  // Total bytes sent, including header
}

ssize_t receive_message(int sockfd, enum Operation &op_code, std::string &message) {
    uint32_t network_order_length;
    ssize_t bytes_read = recv(sockfd, &network_order_length, sizeof(network_order_length), 0);
    if (bytes_read == 0) {
        return 0;  // Connection closed
    } else if (bytes_read < 0) {
        return -1;  // Error reading message length
    }

    uint32_t content_length = ntohl(network_order_length);
    if (content_length == 0) {
        message.clear();  // Handle zero-length content
        return sizeof(network_order_length);  // Indicate only the length field was read
    }
    
    bytes_read = recv(sockfd, &op_code, sizeof(op_code), MSG_WAITALL);
    if (bytes_read == 0) {
        return 0;  // Connection closed
    } else if (bytes_read < 0) {
        return -1;  // Error reading message length
    }
    
    content_length -= bytes_read;
    if (content_length == 0) {
        message.clear();  // Handle zero-length messages
        return sizeof(network_order_length) + sizeof(op_code);  // Indicate only the length and op_code field was read
    }

    std::vector<char> buffer(content_length);
    bytes_read = recv(sockfd, buffer.data(), content_length, MSG_WAITALL);
    if (bytes_read == 0) {
        return 0;  // Connection closed
    } else if (bytes_read < 0) {
        return -2;  // Error reading message content
    }

    message.assign(buffer.begin(), buffer.end());
    return sizeof(network_order_length) + sizeof(op_code) + bytes_read;  // Return total bytes read
}


#endif // MESSAGE_PROTOCOL_H