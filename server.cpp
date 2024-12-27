#include <iostream>
#include <string>
#include <queue>
#include <deque>
#include <algorithm>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include "ChatProtocol.h"
#include "User.h"

#define SERVER_PORT 7548
#define MAX_THREADS 10

std::atomic<bool> server_running(true);
std::deque<t_user> users; // Shared resource
std::queue<int> client_queue; // Job queue for client sockets
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for queue
pthread_cond_t condition_var = PTHREAD_COND_INITIALIZER; // Condition variable

void signal_handler(int signum) {
    std::cout << "\nSignal " << signum << " received. Shutting down server...\n";
    server_running = false;

    pthread_mutex_lock(&queue_mutex);
    pthread_cond_broadcast(&condition_var);
    pthread_mutex_unlock(&queue_mutex);
}

void* worker_thread(void* arg);
void handle_client(int client_socket);
t_user* find_user(const std::string& username);
bool username_exists(const std::string& username);

int main() {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        std::cerr << "Error creating socket.\n";
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "Error binding socket.\n";
        return -1;
    }

    if (listen(server_socket, 10) == -1) {
        std::cerr << "Error listening on socket.\n";
        return -1;
    }
    
    // Handle signal
    // todo: a total failure here
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create worker threads
    pthread_t threads[MAX_THREADS];
    for (int i = 0; i < MAX_THREADS; ++i) {
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);
    }

    // Accept and enqueue clients
	while (server_running) {
	    fd_set read_fds;
	    FD_ZERO(&read_fds);
	    FD_SET(server_socket, &read_fds);
	
	    struct timeval timeout = {10, 0}; // 1 second timeout
	
	    int activity = select(server_socket + 1, &read_fds, nullptr, nullptr, &timeout);
	
	    if (activity < 0 && errno != EINTR) {
	        std::cerr << "Error on select.\n";
	        break;
	    }
	
	    if (activity > 0 && FD_ISSET(server_socket, &read_fds)) {
	        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);
	        if (client_socket == -1) {
	            if (!server_running) break; // Exit if server is shutting down
	            std::cerr << "Error accepting client.\n";
	            continue;
	        }
	
	        // Enqueue the client socket
	        pthread_mutex_lock(&queue_mutex);
	        client_queue.push(client_socket);
	        pthread_cond_signal(&condition_var);
	        pthread_mutex_unlock(&queue_mutex);
	    }
	}
    
    // Close the server socket
    close(server_socket);

    // Notify threads to exit and join them
    pthread_mutex_lock(&queue_mutex);
    pthread_cond_broadcast(&condition_var);
    pthread_mutex_unlock(&queue_mutex);

    for (int i = 0; i < MAX_THREADS; ++i) {
        pthread_join(threads[i], nullptr);
    }

    std::cout << "Server shutdown complete.\n";
    return 0;
}

void* worker_thread(void* arg) {
    while (server_running) {
        int client_socket;

        // Lock queue and wait for a job or termination signal
        pthread_mutex_lock(&queue_mutex);
        while (server_running && client_queue.empty()) {
            pthread_cond_wait(&condition_var, &queue_mutex);
        }
        if (!server_running) {
            pthread_mutex_unlock(&queue_mutex);
            break; // Exit thread loop if server is shutting down
        }

        client_socket = client_queue.front();
        client_queue.pop();
        pthread_mutex_unlock(&queue_mutex);

        // Process the client
        handle_client(client_socket);

        // Close the client socket after handling
        close(client_socket);
        
        std::cout << "User exited" << std::endl;
    }
    return nullptr;
}

void handle_client(int client_socket) {
	int client_port;
	try {
	    Message msg = receive_message(client_socket);
	    if (msg.header.messageType == CLIENT_INIT) {
	        std::string port_string = msg.bodyAsString();
	        client_port = std::stoi(port_string);
	    } else {
	    	// Fail to init client
	    	return;
		}
	} catch (const std::runtime_error& e) {
	    std::cerr << "Error receiving message: " << e.what() << std::endl;
	    return;
	}
	
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    if (getpeername(client_socket, (struct sockaddr*)&client_addr, &client_addr_len) < 0) {
        std::cerr << "Failed to retrieve client addr." << std::endl;
        return;
    }
	
    const char* client_ip = inet_ntoa(client_addr.sin_addr);
    if (client_ip == nullptr) {
        std::cerr << "Failed to convert client address to string." << std::endl;
        return;
    }

    std::cout << "Client IPv4 address: " << client_ip << ", listening port:" << client_port << std::endl;
	
	t_user* current_user = nullptr;
    while (true) {
    	Message request;
    	std::string message;
		try {
		    request = receive_message(client_socket);
		    message = request.bodyAsString();
		} catch (const std::runtime_error& e) {
		    std::cerr << "Error receiving message: " << e.what() << std::endl;
		    if (current_user != nullptr) {
		    	current_user->logout();
			}
		    return;
		}

        Message response;
        std::string body;

        pthread_mutex_lock(&queue_mutex); // Lock for thread-safe access to `users`
        
        if (current_user != nullptr && (request.header.messageType != RESPONSE_SUCCESS && request.header.messageType != RESPONSE_FAIL)) {
        	current_user->next_message_id();
		}
        
        switch (request.header.messageType) {
        	case REGISTER: {
	            std::string username = message.substr(0, message.find(' ', 0));
	            std::string password = message.substr(message.find(' ', 0) + 1);
	
	            if (!username_exists(username)) {
	                users.push_back(t_user(username, password));
	                response.header.messageType = RESPONSE_SUCCESS;
	                body = "User registered successfully.";
	            } else {
	            	response.header.messageType = RESPONSE_FAIL;
	                body = "User already exists.";
	            }
	            break;
			}
	        case LOGIN: {
	            std::string username = message.substr(0, message.find(' ', 0));
	            std::string password = message.substr(message.find(' ', 0) + 1);
	            
	            t_user *user = find_user(username);
	            
		        if (user == nullptr) {
		        	response.header.messageType = RESPONSE_FAIL;
		            body = "Login failed. Invalid credentials.";
				} else if (user->login_status()) {
					response.header.messageType = RESPONSE_FAIL;
					body = "Login failed. Already logged in.";
				} else if (user->login(password)) {
					current_user = user;
					current_user->set_socket_status(client_socket, client_port, client_ip);
					current_user->set_message_id(request.header.messageID);
					response.header.messageType = RESPONSE_SUCCESS;
					body = "Login successful.";
				} else {
					response.header.messageType = RESPONSE_FAIL;
					body = "Login failed. Invalid credentials.";
				}
				break;
			}
			case LOGOUT: {
	        	if (current_user == nullptr) {
	        		response.header.messageType = RESPONSE_FAIL;
	        		body = "Logout failed. Invalid credentials.";
				} else if (current_user->logout()){
					current_user = nullptr;
					response.header.messageType = RESPONSE_SUCCESS;
					body = "Logout successful.";
				} else {
					current_user = nullptr;
					response.header.messageType = RESPONSE_FAIL;
					body = "Logout failed. Already logged out.";
				}
				break;
			}
			case EXIT: {
	        	if (current_user != nullptr) {
	        		current_user->logout();
	        	}
	        	
	        	pthread_mutex_unlock(&queue_mutex);
	        	return;
			}
	        case TEXT_MESSAGE: {
	        	size_t pos = message.find(' ', 0);
	            std::string sender = message.substr(0, pos);
	            std::string receiver = message.substr(pos + 1, message.find(' ', pos));
	            
	            if ((current_user == nullptr) || (sender != current_user->username())) {
					response.header.messageType = RESPONSE_FAIL;
					body = "Msg failed. Invalid credentials.";
				} else {
					t_user* other_user = find_user(receiver);
					if (other_user == nullptr) {
						response.header.messageType = RESPONSE_FAIL;
						body = "Msg failed. User does not exist.";
					} else if (!other_user->login_status()) {
						response.header.messageType = RESPONSE_FAIL;
						body = "Msg failed. User is not online.";
					} else {
						Message forward = request;
						forward.header.messageID = other_user->next_message_id();
						if (send_message(other_user->socket(), forward)) {
							response.header.messageType = RESPONSE_SUCCESS;
							body = "Msg success.";
						} else {
							response.header.messageType = RESPONSE_FAIL;
							body = "Msg failed. can't send.";
						}
					}
				}
				
				break;
			}
			case FILE_TRANSFER_INIT: {
	        	size_t pos = message.find(' ', 0);
	            std::string sender = message.substr(0, pos);
	            std::string receiver = message.substr(pos + 1, message.find(' ', pos));
	            
	            int forward_message_id;
	            if ((current_user == nullptr) || (sender != current_user->username())) {
					response.header.messageType = RESPONSE_FAIL;
					body = "File failed. Invalid credentials.";
				} else {
					t_user* other_user = find_user(receiver);
					if (other_user == nullptr) {
						response.header.messageType = RESPONSE_FAIL;
						body = "File failed. User does not exist.";
					} else if (!other_user->login_status()) {
						response.header.messageType = RESPONSE_FAIL;
						body = "File failed. User is not online.";
					} else {
						Message forward = request;
						forward.header.messageID = other_user->next_message_id();
						forward_message_id = forward.header.messageID;
						if (send_message(other_user->socket(), forward)) {
							response.header.messageType = RESPONSE_SUCCESS;
							body = "File success.";
						} else {
							response.header.messageType = RESPONSE_FAIL;
							body = "File failed. can't send.";
						}
					}
				}
				
				t_user* other_user = find_user(receiver);
				
				pthread_mutex_unlock(&queue_mutex); // Unlock after accessing `users`
				
				while(true) {
					Message forward_data;
					try {
					    forward_data = receive_message(client_socket);
					} catch (const std::runtime_error& e) {
					    std::cerr << "Error receiving message: " << e.what() << std::endl;
					    return;
					}
					
					pthread_mutex_lock(&queue_mutex);
					
					forward_data.header.messageID = forward_message_id;
					if (!send_message(other_user->socket(), forward_data)) {
						response.header.messageType = RESPONSE_FAIL;
						body = "File failed. can't send.";
						break;
					}
					
					if (forward_data.header.bodyLength == 0) {
						break;
					}
					
					pthread_mutex_unlock(&queue_mutex);
				}
				break;
			}
			case STREAM_REQ: {
	        	size_t pos = message.find(' ', 0);
	            std::string sender = message.substr(0, pos);
	            std::string receiver = message.substr(pos + 1);
	            
	            if ((current_user == nullptr) || (sender != current_user->username())) {
					response.header.messageType = RESPONSE_FAIL;
					body = "Call failed. Invalid credentials.";
				} else {
					t_user* other_user = find_user(receiver);
					if (other_user == nullptr) {
						response.header.messageType = RESPONSE_FAIL;
						body = "Call failed. User does not exist.";
					} else if (!other_user->login_status()) {
						response.header.messageType = RESPONSE_FAIL;
						body = "Call failed. User is not online.";
					} else {
						Message forward = request;
						forward.header.messageID = other_user->next_message_id();
						std::string body = forward.bodyAsString() + " " + current_user->address();
						forward.body.assign(body.begin(), body.end());
						forward.header.bodyLength = forward.body.size();
						current_user->set_pending_call_id(request.header.messageID);
						if (send_message(other_user->socket(), forward)) {
							// wait for receiver response
							pthread_mutex_unlock(&queue_mutex);
							continue;
						} else {
							response.header.messageType = RESPONSE_FAIL;
							body = "Call failed. can't send.";
						}
					}
				}
				break;
			}
			case RESPONSE_SUCCESS: {
	        	size_t pos = message.find(' ', 0);
	            std::string sender = message.substr(0, pos);
	            std::string receiver = message.substr(pos + 1, message.find(' ', pos));
	            
	            if ((current_user == nullptr) || (receiver != current_user->username())) {
					// Who are you?
				} else {
					t_user* other_user = find_user(sender);
					if (other_user == nullptr || !other_user->login_status()) {
						// no need for response
					} else {
						Message forward = request;
						forward.header.messageID = other_user->pending_call_id();
						std::string receiver_address = current_user->address();
						forward.body.assign(receiver_address.begin(), receiver_address.end());
						forward.header.bodyLength = forward.body.size();
						send_message(other_user->socket(), forward);
					}
				}
				
				pthread_mutex_unlock(&queue_mutex);
				continue;
			}
			case RESPONSE_FAIL: {
	        	size_t pos = message.find(' ', 0);
	            std::string sender = message.substr(0, pos);
	            std::string receiver = message.substr(pos + 1, message.find(' ', pos));
	            
	            if ((current_user == nullptr) || (receiver != current_user->username())) {
					// Who are you?
				} else {
					t_user* other_user = find_user(sender);
					if (other_user == nullptr || !other_user->login_status()) {
						// no need for response
					} else {
						Message forward = request;
						forward.header.messageID = other_user->pending_call_id();
						send_message(other_user->socket(), forward);
					}
				}
				
				pthread_mutex_unlock(&queue_mutex);
				continue;
			}
	        default: {
	        	response.header.messageType = RESPONSE_FAIL;
	        	body = "Invalid command";
	        	break;
			}
		}
        pthread_mutex_unlock(&queue_mutex); // Unlock after accessing `users`
        
        response.body.assign(body.begin(), body.end());
        response.header.bodyLength = response.body.size();
        response.header.messageID = request.header.messageID;

        send_message(client_socket, response);
    }
}

t_user* find_user(const std::string& username) {
    auto it = std::find_if(users.begin(), users.end(), [&username](const t_user& u) {
        return u.username() == username;
    });

    if (it != users.end()) {
        return &(*it);
    }

    return nullptr;
}

bool username_exists(const std::string& username) {
    auto it = std::find_if(users.begin(), users.end(), [&username](const t_user& u) {
        return u.username() == username;
    });

    return it != users.end();
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
