#include <iostream>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <map>
#include <queue>
#include <deque>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "ChatProtocol.h"

#define SERVER_PORT 8080
#define MAX_RETRIES 3
#define RETRY_DELAY 1  // Delay in seconds before retrying

template <typename T>
void clear(std::queue<T>& q) {
	std::queue<T>().swap(q);
}

bool client_running = true;
struct ClientState {
    bool logged_in;
    std::string username;
    int listen_socket, server_socket, max_fd;
    int next_message_id;
    std::map<int, std::string> peers; // Key: peer socket, Value: peer username
    std::map<std::string, std::deque<Message>> historys; // Key: peer username, Value: chat history
    std::queue<Message> pendings; // messages not yet sent to server
    std::vector<Message> unresolved; // messages in progress, those waiting for response, in coming files, etc.
    std::map<int, std::ofstream> opened_files;
    
    void init(int server_socket, int listen_socket) {
        this->server_socket = server_socket;
        this->listen_socket = listen_socket;
        this->max_fd = std::max(server_socket, listen_socket);
        this->logged_in = false;
        this->next_message_id = 1;
        this->username.clear();
        this->peers.clear();
        this->historys.clear();
        this->unresolved.clear();
        this->opened_files.clear();
        clear(this->pendings);
    }
};

ClientState client_state;

bool connect(int &clientSocket, struct sockaddr_in &server_addr) {
    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        clientSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (clientSocket < 0) {
            std::cerr << "Socket creation failed (Attempt " << attempt << ")\n";
            sleep(RETRY_DELAY);
            continue;
        }

        if (connect(clientSocket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
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

void handle_server_message();
void handle_client_input();
void handle_pending_message();
void print_debug_prompt();
void print_login_prompt();
void print_chat_prompt();
std::vector<Message>::const_iterator find_message_index_by_id(const std::vector<Message>& messages, int messageID);

int main() {
    int listen_socket, server_socket;
    struct sockaddr_in listen_addr, server_addr, peer_addr;
    socklen_t addr_len = sizeof(listen_addr);

    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0) {
        std::cerr << "Failed to create listening socket.\n";
        pthread_exit(nullptr);
    }

    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces
    listen_addr.sin_port = 0;                  // Let the OS choose an available port

    if (bind(listen_socket, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
        std::cerr << "Failed to bind listening socket.\n";
        close(listen_socket);
        return -1;
    }
    
    struct sockaddr_in assigned_addr;
    socklen_t assigned_addr_len = sizeof(assigned_addr);
    if (getsockname(listen_socket, (struct sockaddr*)&assigned_addr, &assigned_addr_len) < 0) {
        std::cerr << "Failed to retrieve assigned port.\n";
        close(listen_socket);
        return -1;
    }
    
    const int assigned_port = ntohs(assigned_addr.sin_port);
    std::cout << "Listening on dynamically assigned port: " << assigned_port << "\n";

    if (listen(listen_socket, 5) < 0) {
        std::cerr << "Failed to listen on port.\n";
        close(listen_socket);
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address or address not supported." << std::endl;
        return -1;
    }

    // Initial connection attempt
    if (!connect(server_socket, server_addr)) {
        std::cerr << "Failed to connect to the server after multiple attempts.\n";
        return -1;
	}
	
	Message init_msg;
	init_msg.header.messageType = CLIENT_INIT;
	init_msg.header.messageID = 0;
	std::string port_str = std::to_string(assigned_port);
	init_msg.body.assign(port_str.begin(), port_str.end());
	init_msg.header.bodyLength = init_msg.body.size();
	if (!send_message(server_socket, init_msg)) {
		std::cerr << "Failed to send message to server.\n";
		return 0;
	}
	
	client_state.init(server_socket, listen_socket);
	print_debug_prompt();
	
    while (client_running) {
    	fd_set read_fds, write_fds;
	    FD_ZERO(&read_fds);
	    FD_ZERO(&write_fds);
	    FD_SET(client_state.listen_socket, &read_fds);
	    FD_SET(client_state.server_socket, &read_fds);
	    FD_SET(STDIN_FILENO, &read_fds);
	    if (!client_state.pendings.empty()) {
	    	FD_SET(client_state.server_socket, &write_fds);
		}
	    
	    struct timeval timeout = {10, 0}; // 1 second timeout
	
	    int activity = select(client_state.max_fd + 1, &read_fds, &write_fds, nullptr, &timeout);
	    
	    if (activity < 0 && errno != EINTR) {
	        std::cerr << "Error on select.\n";
	        break;
	    }
	    
	    if (activity > 0) {
	    	if (FD_ISSET(client_state.server_socket, &write_fds)) {
	    		handle_pending_message();
			}
	    	
	    	if (FD_ISSET(client_state.server_socket, &read_fds)) {
	    		handle_server_message();
			}
	    	
	    	if (FD_ISSET(STDIN_FILENO, &read_fds)) {
	    		handle_client_input();
			}
			
			if (FD_ISSET(client_state.listen_socket, &read_fds)) {
		        int new_socket = accept(client_state.listen_socket, (struct sockaddr*)&peer_addr, &addr_len);
		        if (new_socket < 0) {
		            std::cerr << "Failed to accept connection.\n";
		            continue;
		        }
		
		        std::cout << "Connection accepted from a peer.\n";
		
		        // Handle peer communication here
		        
		        close(new_socket);
			}
		}
    }

    close(server_socket);
    close(listen_socket);
    
    return 0;
}

void print_debug_prompt() {
	std::cout << "input: " << std::flush;
}

void print_login_prompt() {
	std::cout << "login or register" << std::endl;
}

void print_chat_prompt() {
	std::cout << "send message or logout" << std::endl;
}

void handle_client_input() {
	Message request;
	std::string input;
	std::cin >> input;
	
	if (input.compare(0, 8, "register") == 0) {
		std::string username, password;
		std::cin >> username >> password;
		request.header.messageType = REGISTER;
		request.header.messageID = client_state.next_message_id++;
		std::string body = username + " " + password;
		request.body.assign(body.begin(), body.end());
		request.header.bodyLength = request.body.size();
		client_state.pendings.push(request);
	} else if (input.compare(0, 5, "login") == 0) {
		std::string username, password;
		std::cin >> username >> password;
		request.header.messageType = LOGIN;
		request.header.messageID = client_state.next_message_id++;
		std::string body = username + " " + password;
		request.body.assign(body.begin(), body.end());
		request.header.bodyLength = request.body.size();
		client_state.pendings.push(request);
	} else if (input.compare(0, 4, "exit") == 0) {
		request.header.messageType = EXIT;
		request.header.messageID = client_state.next_message_id++;
		request.body.clear();
		request.header.bodyLength = request.body.size();
		send_message(client_state.server_socket, request);
		client_running = false;
		return;
	} else if (input.compare(0, 3, "msg") == 0) {
		std::string sender, receiver, message;
		std::cin >> sender >> receiver;
		std::getline(std::cin, message);
		request.header.messageType = TEXT_MESSAGE;
		request.header.messageID = client_state.next_message_id++;
		std::string body = sender + " " + receiver + " " + message;
		request.body.assign(body.begin(), body.end());
		request.header.bodyLength = request.body.size();
		client_state.pendings.push(request);
	} else if (input.compare(0, 6, "logout") == 0) {
		request.header.messageType = LOGOUT;
		request.header.messageID = client_state.next_message_id++;
		request.body.clear();
		request.header.bodyLength = request.body.size();
		client_state.pendings.push(request);
	} else if (input.compare(0, 4, "file") == 0) {
		std::string sender, receiver, path;
		std::cin >> sender >> receiver >> path;
		
	    std::ifstream file(path, std::ios::binary);
	    if (!file.is_open()) {
	        std::cerr << "Error opening file: " << path << std::endl;
	        return;
	    }
	    
		request.header.messageType = FILE_TRANSFER_INIT;
		request.header.messageID = client_state.next_message_id++;
		std::string body = sender + " " + receiver + " " + path.substr(path.find_last_of("/\\") + 1);
		request.body.assign(body.begin(), body.end());
		request.header.bodyLength = request.body.size();
		client_state.pendings.push(request);
	    
	    int file_transfer_message_id = request.header.messageID;
	    char buffer[MAX_BUFFER_SIZE];
	    
	    // Send file data in chunks
		while (file.read(buffer, MAX_BUFFER_SIZE) || file.gcount() > 0) {
			request.header.messageType = FILE_TRANSFER_DATA;
			request.header.messageID = file_transfer_message_id;
			
	        // Get the number of bytes read
	        std::streamsize bytes_read = file.gcount();
	        
	        // Prepare the body with the data read from the file
	        request.body.assign(buffer, buffer + bytes_read);
	        request.header.bodyLength = request.body.size();
	        client_state.pendings.push(request);
		}
		
		// Send final message to indicate end of transfer
		request.header.messageType = FILE_TRANSFER_DATA;
		request.header.messageID = file_transfer_message_id;
		request.body.clear(); // 0 byte indicate end of file transfer
		request.header.bodyLength = request.body.size();
		client_state.pendings.push(request);
		
		file.close();
	} else {
		std::cout << "Invalid input: " << input << std::endl;
		print_debug_prompt();
		
		return;
	}

	return;
}

void handle_server_message() {
    Message response;
    std::string message;
	try {
		response = receive_message(client_state.server_socket);
	} catch (const std::runtime_error& e) {
		std::cerr << "Error receiving message: " << e.what() << std::endl;
		client_running = false;
		return;
	}
	
	switch (response.header.messageType) {
		case RESPONSE_SUCCESS: {
			std::cout << "Success: " << response.bodyAsString() << std::endl;
			auto it = find_message_index_by_id(client_state.unresolved, response.header.messageID);
			if (it == client_state.unresolved.end()) {
				std::cout << "Original request not found." << std::endl;
			} else {
				std::cout << "Original request: " << it->bodyAsString() << std::endl;
				client_state.unresolved.erase(it);
			}
			break;
		}
		case RESPONSE_FAIL: {
			std::cout << "Fail: " << response.bodyAsString() << std::endl;
			auto it = find_message_index_by_id(client_state.unresolved, response.header.messageID);
			if (it == client_state.unresolved.end()) {
				std::cout << "Original request not found." << std::endl;
			} else {
				std::cout << "Original request: " << it->bodyAsString() << std::endl;
				client_state.unresolved.erase(it);
			}
			break;
		}
		case TEXT_MESSAGE: {
			std::cout << "Msg: " << response.bodyAsString() << std::endl;
			client_state.next_message_id = std::max(client_state.next_message_id, response.header.messageID + 1);
			break;
		}
		case FILE_TRANSFER_INIT: {
			std::cout << "File: " << response.bodyAsString() << std::endl;
			client_state.next_message_id = std::max(client_state.next_message_id, response.header.messageID + 1);
			client_state.unresolved.push_back(response);
			
            std::istringstream iss(response.bodyAsString());
            std::string sender, receiver, file_name;
            if (!(iss >> sender >> receiver >> file_name)) {
                std::cerr << "Invalid FILE_TRANSFER_INIT data!" << std::endl;
                break;
            }
			
			std::ofstream output_file;
            output_file.open(file_name, std::ios::binary);
            if (!output_file.is_open()) {
                std::cerr << "Failed to open file for writing: " << file_name << std::endl;
                break;
            }
            client_state.opened_files[response.header.messageID] = std::move(output_file);
			break;
		}
		case FILE_TRANSFER_DATA: {
			std::cout << "File data incoming."<< std::endl;
			if (response.header.bodyLength == 0) {
				auto it = find_message_index_by_id(client_state.unresolved, response.header.messageID);
				if (it != client_state.unresolved.end()) {
					if (client_state.opened_files[response.header.messageID].is_open()) {
						client_state.opened_files[response.header.messageID].close();
					}
					client_state.opened_files.erase(response.header.messageID);
					client_state.unresolved.erase(it);
				} else {
					std::cout << "Original request not found." << std::endl;
				}
				
				break;
			}
			
            client_state.opened_files[response.header.messageID].write(response.body.data(), response.body.size());
            if (!client_state.opened_files[response.header.messageID]) {
                std::cerr << "Error writing to file!" << std::endl;
                client_state.opened_files[response.header.messageID].close();
            }
			
			break;
		}
		default: {
			std::cout << "Unknown message type" << std::endl;
			break;
		}
	}
}

void handle_pending_message() {
	if (client_state.pendings.empty()) {
		return;
	}
	
	Message request = client_state.pendings.front();
	send_message(client_state.server_socket, request);
	if (request.header.messageType != FILE_TRANSFER_DATA) {
		client_state.unresolved.push_back(request);
	}
	client_state.pendings.pop();
	
	return;
}

std::vector<Message>::const_iterator find_message_index_by_id(const std::vector<Message>& messages, int messageID) {
    return std::find_if(messages.begin(), messages.end(), [messageID](const Message& msg) {
        return msg.header.messageID == messageID;
    });
}

void logMessage(const Message& msg) {
    // Log the message header details
    std::cerr << "Message ID: " << msg.header.messageID << std::endl;
    std::cerr << "Message Type: " << msg.header.messageType << std::endl;
    std::cerr << "Body Length: " << msg.header.bodyLength << std::endl;

    // If the message body is text, log the body as a string
    if (msg.header.messageType != FILE_TRANSFER_DATA) {
        std::cerr << "Message Body: " << msg.bodyAsString() << std::endl;
    } else {
        // Otherwise, print the body as raw bytes (for non-text messages)
        std::cerr << "Message Body (Raw Data): ";
        for (char byte : msg.body) {
            std::cerr << std::hex << (int)(unsigned char)byte << " ";
        }
        std::cerr << std::dec << std::endl; // Return to decimal output
    }
}
