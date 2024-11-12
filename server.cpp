#include <iostream>
#include <string>
#include <map>
#include <unistd.h>
#include <arpa/inet.h>
#include "MessageProtocol.h"

#define PORT 8080

std::map<std::string, std::string> users;

void handleClient(int clientSocket) {
	bool loggedIn = false;
    std::string command;
    std::string username, password;

    while (true) {
    	ssize_t bytesReceived = receiveMessage(clientSocket, command);
    	
        if (bytesReceived == 0) {
            std::cout << "Client disconnected.\n";
            break;
        } else if (bytesReceived == -1) {
            std::cerr << "Error: Failed to read message length from client.\n";
            break;
        } else if (bytesReceived == -2) {
            std::cerr << "Error: Failed to read message content from client.\n";
            break;
        }
    	
        std::string response;

        if (command.find("REGISTER") == 0) {
            // Register a new user
            username = command.substr(9, command.find(' ', 9) - 9);
            password = command.substr(command.find(' ', 9) + 1);

            if (users.find(username) == users.end()) {
                users[username] = password;
                response = "User registered successfully.";
            } else {
                response = "User already exists.";
            }
        } else if (command.find("LOGIN") == 0) {
            // Log in an existing user
            username = command.substr(6, command.find(' ', 6) - 6);
            password = command.substr(command.find(' ', 6) + 1);
            
            if (loggedIn) {
                response = "Login failed. Already logged in.";
            } else if (users.find(username) != users.end() && users[username] == password) {
                loggedIn = true;
                response = "Login successful.";
            } else {
                response = "Login failed. Invalid credentials.";
            }
        } else if (command == "LOGOUT") {
            if (loggedIn) {
                loggedIn = false;
                response = "Logged out successfully.";
            } else {
                response = "No user is logged in.";
            }
        } else if (command == "EXIT") {
            response = "Disconnecting from server.";
            sendMessage(clientSocket, response);
            std::cout << "Client disconnected." << std::endl;
            break;
        } else if (command == "WHOAMI") {
        	if (loggedIn) {
        		response = username;
			} else {
				response = "No user is logged in.";
			}
		} else {
            response = "Invalid command.";
        }
        
        ssize_t bytesSent = sendMessage(clientSocket, response);
        if (bytesSent < 0) {
            std::cerr << "Error sending message to client.\n";
            break;
        }
    }
}

int main() {
    int serverSocket, clientSocket;
    struct sockaddr_in serverAddr, clientAddr;
    int opt = 1;
    socklen_t addrLen = sizeof(clientAddr);

	// Create and configure the server socket
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }
    
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Bind failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    if (listen(serverSocket, 10) < 0) {
        perror("Listen failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    std::cout << "Server is listening on port " << PORT << std::endl;

    // Main loop to accept and handle clients one at a time
    while (true) {
        clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientSocket < 0) {
            perror("Accept failed");
            continue;  // Continue to the next iteration to accept new clients
        }

        std::cout << "Client connected from " << inet_ntoa(clientAddr.sin_addr) << std::endl;
        handleClient(clientSocket);  // Handle the connected client
        close(clientSocket);  // Close the socket after handling the client
        std::cout << "Ready to accept new client." << std::endl;
    }

    close(serverSocket);
    return 0;
}
