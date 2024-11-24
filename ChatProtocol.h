#ifndef CHAT_PROTOCOL_H
#define CHAT_PROTOCOL_H

#include <string>
#include <vector>
#include <cstring>

// Maximum buffer size for messages
const int MAX_BUFFER_SIZE = 2048;

// Enum for message types
enum MessageType {
	CLIENT_INIT = 0,
    REGISTER = 1,
    LOGIN = 2,
    LOGOUT = 3,
    TEXT_MESSAGE = 4,
    FILE_TRANSFER_INIT = 5,
    FILE_TRANSFER_DATA = 6,
    EXIT = 7,
    RESPONSE_SUCCESS = 8,
    RESPONSE_FAIL = 9,
    RESPONSE_AGAIN = 10
};

// Structure for the message header
struct MessageHeader {
	int messageID;
    int messageType; // Type of the message
    int bodyLength;  // Length of the message body

    // Constructor
    MessageHeader() : messageID(0), messageType(0), bodyLength(0) {}
};

// Structure for the complete message
struct Message {
    MessageHeader header;     // Header for message metadata
    std::vector<char> body;   // Body can now handle both text and binary data

    // Serialize the message to a byte stream
    std::vector<char> serialize() const {
        std::vector<char> buffer(sizeof(header) + body.size());
        memcpy(buffer.data(), &header, sizeof(header));
        memcpy(buffer.data() + sizeof(header), body.data(), body.size());
        return buffer;
    }

    // Deserialize a byte stream into a Message object
    static Message deserialize(const std::vector<char>& buffer) {
        Message msg;

        // Copy header
        memcpy(&msg.header, buffer.data(), sizeof(msg.header));

        // Copy body
        msg.body.assign(buffer.begin() + sizeof(msg.header), buffer.end());
        return msg;
    }

    // Convert body to string (for text messages)
    std::string bodyAsString() const {
        return std::string(body.begin(), body.end());
    }
};

// Utility functions
void logMessage(const Message& msg); // For debugging/logging

bool send_message(int socket, const Message& message) {
	logMessage(message);
	
    // Serialize the message
    std::vector<char> buffer = message.serialize();

    // Send the serialized data
    size_t totalSent = 0;
    while (totalSent < buffer.size()) {
        ssize_t bytesSent = send(socket, buffer.data() + totalSent, buffer.size() - totalSent, 0);
        if (bytesSent <= 0) {
            perror("Send failed");
            return false;
        }
        totalSent += bytesSent;
    }
    return true;
}


Message receive_message(int socket) {
    Message msg;

    // Read the fixed-size header
    char headerBuffer[sizeof(MessageHeader)];
    size_t totalReceived = 0;
    while (totalReceived < sizeof(MessageHeader)) {
        ssize_t bytesReceived = recv(socket, headerBuffer + totalReceived, sizeof(MessageHeader) - totalReceived, 0);
        if (bytesReceived <= 0) {
            perror("Receive failed");
            throw std::runtime_error("Connection closed or error occurred");
        }
        totalReceived += bytesReceived;
    }

    // Copy the header data into the MessageHeader structure
    memcpy(&msg.header, headerBuffer, sizeof(msg.header));

    // Read the body based on the length in the header
    if (msg.header.bodyLength > 0) {
        msg.body.resize(msg.header.bodyLength);
        ssize_t totalReceived = 0;
        while (totalReceived < msg.header.bodyLength) {
            ssize_t bytesReceived = recv(socket, msg.body.data() + totalReceived, msg.header.bodyLength - totalReceived, 0);
            if (bytesReceived <= 0) {
                perror("Receive failed");
                throw std::runtime_error("Connection closed or error occurred");
            }
            totalReceived += bytesReceived;
        }
    }
	
	logMessage(msg);
    return msg;
}

#endif // CHAT_PROTOCOL_H