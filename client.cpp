#include <iostream>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "MessageProtocol.h"

#define PORT 8080
#define MAX_RETRIES 1
#define RETRY_DELAY 5  // Delay in seconds before retrying

bool loggedIn = false;

bool connect(int &clientSocket, struct sockaddr_in &serverAddr) {
    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        clientSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (clientSocket < 0) {
            std::cerr << "Socket creation failed (Attempt " << attempt << ")\n";
            sleep(RETRY_DELAY);
            continue;
        }

        if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            std::cerr << "Connection failed (Attempt " << attempt << "). Retrying...\n";
            close(clientSocket);
            sleep(RETRY_DELAY);
            continue;
        }

        std::cout << "Connected to the server.\n";
        return true;
    }
    return false;
}

int main() {
    int clientSocket;
    struct sockaddr_in serverAddr;

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) <= 0) {
        std::cerr << "Invalid address or address not supported." << std::endl;
        return -1;
    }

    // Initial connection attempt
    if (!connect(clientSocket, serverAddr)) {
        std::cerr << "Failed to connect to the server after multiple attempts.\n";
        return -1;
	}
	
    while (true) {
        std::string command, response;
        if (!loggedIn) {
            std::cout << "Enter command (REGISTER <username> <password> | LOGIN <username> <password> | EXIT): ";
        } else {
            std::cout << "Enter command (WHOAMI | LOGOUT | EXIT): ";
        }
        std::getline(std::cin, command);
		
        if (sendMessage(clientSocket, command) < 0) {
            std::cerr << "Failed to send message.\n";
            break;  // Exit loop if unable to send message, for now
        }

        if (receiveMessage(clientSocket, response) < 0) {
            std::cerr << "Failed to receive message from server.\n";
            break;  // Exit loop if unable to receive message, for now
        }

        std::cout << "Server response: " << response << std::endl;
        
        if (response.compare(0, 17, "Login successful.") == 0) {
        	loggedIn = true;
		} else if (response.compare(0, 24, "Logged out successfully.") == 0) {
			loggedIn = false;
		}

        if (command == "EXIT") {
            std::cout << "Disconnected from server." << std::endl;
            break;
        }
    }

    close(clientSocket);
    return 0;
}
