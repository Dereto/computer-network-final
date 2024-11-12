#ifndef MESSAGE_PROTOCOL_H
#define MESSAGE_PROTOCOL_H

#include <iostream>
#include <string>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

ssize_t sendMessage(int sockfd, const std::string &message) {
    uint32_t messageLength = message.size();
    uint32_t networkOrderLength = htonl(messageLength);

    // Send the length header
    ssize_t bytesSent = send(sockfd, &networkOrderLength, sizeof(networkOrderLength), 0);
    if (bytesSent == -1) {
        return -1;  // Error sending length
    }

    const char *messageData = message.c_str();
    ssize_t totalBytesSent = bytesSent;
    while (messageLength > 0) {
        bytesSent = send(sockfd, messageData, messageLength, 0);
        if (bytesSent == -1) {
            return -2;  // Error sending message content
        }
        messageLength -= bytesSent;
        messageData += bytesSent;
        totalBytesSent += bytesSent;
    }

    return totalBytesSent;  // Total bytes sent, including header
}

ssize_t receiveMessage(int sockfd, std::string &message) {
    uint32_t networkOrderLength;
    ssize_t bytesRead = recv(sockfd, &networkOrderLength, sizeof(networkOrderLength), 0);
    if (bytesRead == 0) {
        return 0;  // Connection closed
    } else if (bytesRead < 0) {
        return -1;  // Error reading message length
    }

    uint32_t messageLength = ntohl(networkOrderLength);
    if (messageLength == 0) {
        message.clear();  // Handle zero-length messages
        return sizeof(networkOrderLength);  // Indicate only the length field was read
    }

    std::vector<char> buffer(messageLength);
    bytesRead = recv(sockfd, buffer.data(), messageLength, MSG_WAITALL);
    if (bytesRead == 0) {
        return 0;  // Connection closed
    } else if (bytesRead < 0) {
        return -2;  // Error reading message content
    }

    message.assign(buffer.begin(), buffer.end());
    return sizeof(networkOrderLength) + bytesRead;  // Return total bytes read
}


#endif // MESSAGE_PROTOCOL_H