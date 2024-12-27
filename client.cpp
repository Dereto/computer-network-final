#define MINIAUDIO_IMPLEMENTATION
#include "./miniaudio.h"
#include <pthread.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <map>
#include <queue>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include "ChatProtocol.h"


// Shared variable
std::atomic<bool> client_running(true);
std::atomic<int> next_message_id(1);
std::atomic<bool> incoming_call(false);
std::atomic<bool> outgoing_call(false);
std::atomic<bool> file_playback(false);
std::atomic<bool> file_playback_not_initable(false);
std::string peer_ip;
unsigned short peer_port;

// Shared resource
std::queue<Message> transmit_queue;
std::vector<Message> pending_list;
std::map<int, std::ifstream> outgoing_files;
ma_decoder file_decoder;

// Mutex
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for transmit_queue
pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for pending_list
pthread_mutex_t map_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for outgoing_files
pthread_mutex_t bool_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for incoming_call and outgoing_call

// Condition variable
pthread_cond_t queue_non_empty = PTHREAD_COND_INITIALIZER; // transmit_queue not empty
pthread_cond_t call_pending = PTHREAD_COND_INITIALIZER; 
pthread_cond_t call_hang = PTHREAD_COND_INITIALIZER; // transmit_queue not empty

// Client state management function
void client_init() {
	std::queue<Message>().swap(transmit_queue);
	pending_list.clear();
}

void client_reset() {
	// todo: should clear everything
}

// Thread function
void* user_handler(void* arg);
void* server_message_sender(void* arg);
void* server_message_receiver(void* arg);
void* call_handler(void* arg);

// Utility function
std::vector<Message>::const_iterator find_message_index_by_id(const std::vector<Message>& messages, int messageID);

// Stream audio funtion
void noise_gate(float* buffer, size_t frameCount, float threshold) {
    for (size_t i = 0; i < frameCount; ++i) {
        if (std::fabs(buffer[i]) < threshold) {
            buffer[i] = 0.0f; // Suppress low amplitude noise
        }
    }
}

void mix_audio(float* destination, const float* source, ma_uint32 frameCount) {
    for (size_t i = 0; i < frameCount; ++i) {
        destination[i] = destination[i] + source[i];
    }
}

void odata_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
	// stream mic audio
    int sockfd = *((int*)pDevice->pUserData);
    
	// Copy the input to a local buffer for processing
	std::vector<float> audioBuffer(frameCount);
	memcpy(audioBuffer.data(), pInput, frameCount * sizeof(float));
	
    // Apply audio processing (e.g., noise gate or low-pass filter)
    noise_gate(audioBuffer.data(), frameCount, 0.01f);
    
    if (file_playback) {
    	ma_uint64 framesRead = 0;
    	std::vector<float> fileBuffer(frameCount);
    	ma_decoder_read_pcm_frames(&file_decoder, (void*)fileBuffer.data(), frameCount, &framesRead);
    	if (framesRead < frameCount) {
    		file_playback = false;
    		file_playback_not_initable = true;
    		pthread_cond_signal(&call_hang);
    		std::cerr << "audio file reached the end" << std::endl;
		} else {
			mix_audio(audioBuffer.data(), fileBuffer.data(), frameCount);
		}
	}
    
    // Send captured audio data (input buffer) via socket
    ssize_t bytes_sent = send(sockfd, audioBuffer.data(), frameCount * sizeof(float), 0);
    if (bytes_sent == -1) {
    	outgoing_call = false;
    	pthread_cond_signal(&call_hang);
        std::cerr << "Error sending data over socket" << std::endl;
	}
}

void idata_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    int sockfd = *((int*)pDevice->pUserData);

    // Receive audio data from the socket
    ssize_t bytes_received = recv(sockfd, pOutput, frameCount * sizeof(float), 0);
    if (bytes_received == -1) {
    	incoming_call = false;
    	pthread_cond_signal(&call_hang);
		std::cerr << "Error receiving data over socket" << std::endl;
    } else if (bytes_received == 0) {
    	incoming_call = false;
    	pthread_cond_signal(&call_hang);
        std::cerr << "No data received." << std::endl;
    }
}

void logMessage(const Message& msg) {
	return;
//    // Log the message header details
//    std::cerr << "Message ID: " << msg.header.messageID << std::endl;
//    std::cerr << "Message Type: " << msg.header.messageType << std::endl;
//    std::cerr << "Body Length: " << msg.header.bodyLength << std::endl;
//
//    // If the message body is text, log the body as a string
//    if (msg.header.messageType != FILE_TRANSFER_DATA) {
//        std::cerr << "Message Body: " << msg.bodyAsString() << std::endl;
//    } else {
//        // Otherwise, print the body as raw bytes (for non-text messages)
//        std::cerr << "Message Body (Raw Data): ";
//        for (char byte : msg.body) {
//            std::cerr << std::hex << (int)(unsigned char)byte << " ";
//        }
//        std::cerr << std::dec << std::endl; // Return to decimal output
//    }
}

Message create_register_message(int message_id, const std::string& username, const std::string& password) {
	Message request;
	
	request.header.messageType = REGISTER;
	request.header.messageID = message_id;
	std::string body = username + " " + password;
	request.body.assign(body.begin(), body.end());
	request.header.bodyLength = request.body.size();
	
	return request;
}

Message create_login_message(int message_id, const std::string& username, const std::string& password) {
	Message request;
	
	request.header.messageType = LOGIN;
	request.header.messageID = message_id;
	std::string body = username + " " + password;
	request.body.assign(body.begin(), body.end());
	request.header.bodyLength = request.body.size();
	
	return request;
}

Message create_exit_message(int message_id) {
	Message request;
	
	request.header.messageType = EXIT;
	request.header.messageID = message_id;
	request.body.clear();
	request.header.bodyLength = request.body.size();
	
	return request;
}

Message create_msg_message(int message_id, const std::string& sender, const std::string& receiver, const std::string& message) {
	Message request;
	
	request.header.messageType = TEXT_MESSAGE;
	request.header.messageID = message_id;
	std::string body = sender + " " + receiver + " " + message;
	request.body.assign(body.begin(), body.end());
	request.header.bodyLength = request.body.size();
	
	return request;
}

Message create_logout_message(int message_id) {
	Message request;

	request.header.messageType = LOGOUT;
	request.header.messageID = message_id;
	request.body.clear();
	request.header.bodyLength = request.body.size();
	
	return request;
}

Message create_file_transfer_init_message(int message_id, const std::string& sender, const std::string& receiver, const std::string& path) {
	Message request;
	
	request.header.messageType = FILE_TRANSFER_INIT;
	request.header.messageID = message_id;
	std::string body = sender + " " + receiver + " " + path.substr(path.find_last_of("/\\") + 1);
	request.body.assign(body.begin(), body.end());
	request.header.bodyLength = request.body.size();
	
	return request;
}

Message create_stream_req_message(int message_id, const std::string& sender, const std::string& receiver) {
	Message request;
	
	request.header.messageType = STREAM_REQ;
	request.header.messageID = message_id;
	std::string body = sender + " " + receiver;
	request.body.assign(body.begin(), body.end());
	request.header.bodyLength = request.body.size();
	
	return request;
}

// Atomically compare and swap
template<typename T>
void update_maximum(std::atomic<T>& maximum_value, const T& value) noexcept 
{
    T current = maximum_value.load(std::memory_order_relaxed);
    while (current < value &&
           !maximum_value.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed))
    {
        // If CAS fails, current will be updated with the latest value of maximum_value
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <server_address> <port>" << std::endl;
        return -1;
    }
    
    int listen_socket, server_socket;
    struct sockaddr_in listen_addr, server_addr;
    socklen_t addr_len = sizeof(listen_addr);
	
	// socket for streaming audio
    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0) {
        std::cerr << "Failed to create listening socket.\n";
        return -1;
    }

    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces
    listen_addr.sin_port = 0;                  // Let the OS choose an available port

	// bind socket for streaming audio
    if (bind(listen_socket, (struct sockaddr*)&listen_addr, addr_len) < 0) {
        std::cerr << "Failed to bind listening socket.\n";
        close(listen_socket);
        return -1;
    }
    
    // retrieve listening port information
    struct sockaddr_in assigned_addr;
    socklen_t assigned_addr_len = sizeof(assigned_addr);
    if (getsockname(listen_socket, (struct sockaddr*)&assigned_addr, &assigned_addr_len) < 0) {
        std::cerr << "Failed to retrieve assigned port.\n";
        close(listen_socket);
        return -1;
    }
    
    const int assigned_port = ntohs(assigned_addr.sin_port);
    std::cout << "Listening on dynamically assigned port: " << assigned_port << "\n";
	
	// listen for peers
    if (listen(listen_socket, 5) < 0) {
        std::cerr << "Failed to listen on port.\n";
        close(listen_socket);
        return -1;
    }
	
	// server address
	const char* server_ip = argv[1];
	int server_port = std::atoi(argv[2]);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address or address not supported." << std::endl;
        return -1;
    }

	// server socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        std::cerr << "Socket creation failed\n";
        close(listen_socket);
        return -1;
    }
	
	// connect to server
    if (connect(server_socket, (struct sockaddr*)&server_addr, addr_len) < 0) {
        std::cerr << "Connection failed\n";
        close(listen_socket);
        close(server_socket);
        return -1;
    }

    std::cout << "Connected to the server.\n";
	
	// initialize client and send first message
    client_init();

	Message init_msg;
	init_msg.header.messageType = CLIENT_INIT;
	init_msg.header.messageID = 0;
	std::string port_str = std::to_string(assigned_port);
	init_msg.body.assign(port_str.begin(), port_str.end());
	init_msg.header.bodyLength = init_msg.body.size();
	if (!send_message(server_socket, init_msg)) {
		std::cerr << "Failed to send message to server.\n";
        close(listen_socket);
        close(server_socket);
		return 0;
	}
	
	// todo: signal handling
	pthread_t server_receiver_thread, server_sender_thread, user_thread, auido_thread;
	
    // Create the thread for sending server messages
    if (pthread_create(&server_sender_thread, nullptr, server_message_sender, (void*)&server_socket) != 0) {
        std::cerr << "Failed to create server_sender_thread thread.\n";
        close(server_socket);
        close(listen_socket);
        return -1;
    }
    
    // Create the thread for sending server messages
    if (pthread_create(&auido_thread, nullptr, call_handler, (void*)&listen_socket) != 0) {
        std::cerr << "Failed to create auido_thread thread.\n";
        close(server_socket);
        close(listen_socket);
        return -1;
    }

    // Create the thread for handling server messages
    if (pthread_create(&server_receiver_thread, nullptr, server_message_receiver, (void*)&server_socket) != 0) {
        std::cerr << "Failed to create server_receiver_thread thread.\n";
        close(server_socket);
        close(listen_socket);
        return -1;
    }

    // Create the thread for handling user inputs
    if (pthread_create(&user_thread, nullptr, user_handler, nullptr) != 0) {
        std::cerr << "Failed to create user_handler thread.\n";
        close(server_socket);
        close(listen_socket);
        return -1;
    }

    pthread_join(user_thread, nullptr);
    pthread_join(server_receiver_thread, nullptr);
    
    // client_running must be false for above two to join, wake server_sender_thread up
    pthread_cond_signal(&queue_non_empty);
    
    pthread_join(server_sender_thread, nullptr);
    
    close(server_socket);
    close(listen_socket);

    return 0;
}

void* user_handler(void* arg) {
	while(client_running) {
		std::string input;
		std::cin >> input;
		
		if (input.compare(0, 8, "register") == 0) {
			std::string username, password;
			std::cin >> username >> password;
			
			pthread_mutex_lock(&queue_mutex);
			transmit_queue.push(create_register_message(next_message_id++, username, password));
			pthread_cond_signal(&queue_non_empty);
			pthread_mutex_unlock(&queue_mutex);
		} else if (input.compare(0, 5, "login") == 0) {
			std::string username, password;
			std::cin >> username >> password;
			
			pthread_mutex_lock(&queue_mutex);
			transmit_queue.push(create_login_message(next_message_id++, username, password));
			pthread_cond_signal(&queue_non_empty);
			pthread_mutex_unlock(&queue_mutex);
		} else if (input.compare(0, 4, "exit") == 0) {
			pthread_mutex_lock(&queue_mutex);
			transmit_queue.push(create_exit_message(next_message_id++));
			pthread_cond_signal(&queue_non_empty);
			pthread_mutex_unlock(&queue_mutex);
			
			client_running = false;
			return nullptr;
		} else if (input.compare(0, 3, "msg") == 0) {
			std::string sender, receiver, message;
			std::cin >> sender >> receiver;
			std::getchar();
			std::getline(std::cin, message);
			
			pthread_mutex_lock(&queue_mutex);
			transmit_queue.push(create_msg_message(next_message_id++, sender, receiver, message));
			pthread_cond_signal(&queue_non_empty);
			pthread_mutex_unlock(&queue_mutex);
		} else if (input.compare(0, 6, "logout") == 0) {
			pthread_mutex_lock(&queue_mutex);
			transmit_queue.push(create_logout_message(next_message_id++));
			pthread_cond_signal(&queue_non_empty);
			pthread_mutex_unlock(&queue_mutex);
		} else if (input.compare(0, 4, "file") == 0) {
			std::string sender, receiver, path;
			std::cin >> sender >> receiver >> path;
			
			// Test and open target file
		    std::ifstream input_file(path, std::ios::binary);
		    if (!input_file.is_open()) {
		        std::cerr << "Error opening file: " << path << std::endl;
		        continue;
		    }
		    
		    int file_message_id = next_message_id++;
		    
		    pthread_mutex_lock(&map_mutex);
		    outgoing_files[file_message_id] = std::move(input_file);
		    pthread_mutex_unlock(&map_mutex);
			
			pthread_mutex_lock(&queue_mutex);
			transmit_queue.push(create_file_transfer_init_message(file_message_id, sender, receiver, path));
			pthread_cond_signal(&queue_non_empty);
			pthread_mutex_unlock(&queue_mutex);
		} else if (input.compare(0, 4, "call") == 0) {
			std::string sender, receiver;
			std::cin >> sender >> receiver;
			
			pthread_mutex_lock(&queue_mutex);
			transmit_queue.push(create_stream_req_message(next_message_id++, sender, receiver));
			pthread_cond_signal(&queue_non_empty);
			pthread_mutex_unlock(&queue_mutex);
		} else if (input.compare(0, 4, "play") == 0) {
			std::string path;
			std::cin >> path;
			
			if (!outgoing_call) {
				std::cerr << "No ongoing call" << std::endl;
				continue;
			} else if (file_playback) {
				std::cerr << "Already playing audio file" << std::endl;
				continue;
			} else if (file_playback_not_initable) {
				std::cerr << "please retry later" << std::endl;
				continue;
			}
			
			ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, 48000);
			ma_result result = ma_decoder_init_file(path.c_str(), &config, &file_decoder);
		    if (result != MA_SUCCESS) {
		        std::cerr << "Could not load file: " << path << std::endl;
		        continue;
		    }
		    
		    file_playback = true;
		} else if (input.compare(0, 4, "hang") == 0) {
			incoming_call = false;
			outgoing_call = false;
			pthread_cond_signal(&call_hang);
		} else {
			std::cout << "Invalid input: " << input << std::endl;
		}
	}

	return nullptr;
}

void* server_message_sender(void* arg) {
	int server_socket = *(int*)arg;
	
	while (true) {
		pthread_mutex_lock(&queue_mutex);
		while (transmit_queue.empty() && client_running) {
			// todo: main thread had to wake it up
			pthread_cond_wait(&queue_non_empty, &queue_mutex);
		}
		
		if (!client_running && transmit_queue.empty()) {
			break;
		}
		
		// retrieve and pop next request from queue
		Message request = std::move(transmit_queue.front());
		transmit_queue.pop();
		
		pthread_mutex_unlock(&queue_mutex);
		
		// todo: select()
		
		if (request.header.messageType == FILE_TRANSFER_INIT) {
			// push sent message to pending list
			pthread_mutex_lock(&list_mutex);
			
			send_message(server_socket, request); // todo: error handling
			pending_list.push_back(request);
			
			pthread_mutex_unlock(&list_mutex);
			
			int file_transfer_message_id = request.header.messageID;
			
			pthread_mutex_lock(&map_mutex);
			std::ifstream &file = outgoing_files[file_transfer_message_id];
			pthread_mutex_unlock(&map_mutex);
			
			char buffer[MAX_BUFFER_SIZE];
			
			while (file.read(buffer, MAX_BUFFER_SIZE) || file.gcount() > 0) {
				Message data_block;
				data_block.header.messageType = FILE_TRANSFER_DATA;
				data_block.header.messageID = file_transfer_message_id;
				
		        // Get the number of bytes read
		        std::streamsize bytes_read = file.gcount();
		        
		        // Prepare the body with the data read from the file
		        data_block.body.assign(buffer, buffer + bytes_read);
		        data_block.header.bodyLength = data_block.body.size();
		        send_message(server_socket, data_block);
			}
			
			// Send final message to indicate end of transfer
			Message data_end;
			data_end.header.messageType = FILE_TRANSFER_DATA;
			data_end.header.messageID = file_transfer_message_id;
			data_end.body.clear(); // 0 byte indicate end of file transfer
			data_end.header.bodyLength = data_end.body.size();
			send_message(server_socket, data_end);
			
			file.close();
			
			pthread_mutex_lock(&map_mutex);
			outgoing_files.erase(file_transfer_message_id);
			pthread_mutex_unlock(&map_mutex);
		} else if (request.header.messageType == RESPONSE_SUCCESS || request.header.messageType == RESPONSE_FAIL) {
			send_message(server_socket, request);
		} else {
			// push sent message to pending list
			pthread_mutex_lock(&list_mutex);
			
			send_message(server_socket, request); // todo: error handling
			pending_list.push_back(std::move(request));
			
			pthread_mutex_unlock(&list_mutex);
		}
	}
	
	return nullptr;
}

void* server_message_receiver(void* arg) {
	int server_socket = *(int*)arg;
	std::map<int, std::ofstream> incoming_files;
	
	while (client_running) {
	    Message response;
	    std::string message;
		try {
			response = receive_message(server_socket);
		} catch (const std::runtime_error& e) {
			if (client_running) {
				std::cerr << "Error receiving message: " << e.what() << std::endl;
				client_running = false;
			}
			return nullptr;
		}
		
		switch (response.header.messageType) {
			case RESPONSE_SUCCESS: {
				std::cout << "Success: " << response.bodyAsString() << std::endl;
				
				pthread_mutex_lock(&list_mutex);
				auto it = find_message_index_by_id(pending_list, response.header.messageID);
				if (it == pending_list.end()) {
					std::cout << "Original request not found." << std::endl;
				} else {
					switch ((it->header).messageType) {
						case STREAM_REQ: {
							if (incoming_call || outgoing_call) {
								// todo?
							} else {
								std::istringstream iss(response.bodyAsString());
								
					            if (!(iss >> peer_ip >> peer_port)) {
					                std::cerr << "Invalid peer ip or port!" << std::endl;
					            } else {
									outgoing_call = true;
									pthread_cond_signal(&call_pending);
								}
							}
							break;
						}
					}
					
					pending_list.erase(it);
				}
				pthread_mutex_unlock(&list_mutex);
				
				break;
			}
			case RESPONSE_FAIL: {
				std::cout << "Fail: " << response.bodyAsString() << std::endl;
				
				pthread_mutex_lock(&list_mutex);
				auto it = find_message_index_by_id(pending_list, response.header.messageID);
				if (it == pending_list.end()) {
					std::cout << "Original request not found." << std::endl;
				} else {
					std::cout << "Original request: " << it->bodyAsString() << std::endl;
					pending_list.erase(it);
				}
				pthread_mutex_unlock(&list_mutex);
				
				break;
			}
			case TEXT_MESSAGE: {
				std::istringstream iss(response.bodyAsString());
				std::string sender, receiver, msg;
	            if (!(iss >> sender >> receiver >> msg)) {
	                std::cerr << "Invalid TEXT_MESSAGE data!" << std::endl;
	                break;
	            }
				std::cout << sender << ": " << msg << std::endl;
				update_maximum(next_message_id, response.header.messageID + 1);
				break;
			}
			case FILE_TRANSFER_INIT: {
				std::cout << "File: " << response.bodyAsString() << std::endl;
				update_maximum(next_message_id, response.header.messageID + 1);
				
				pthread_mutex_lock(&list_mutex);
				pending_list.push_back(response);
				pthread_mutex_unlock(&list_mutex);
				
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

	            incoming_files[response.header.messageID] = std::move(output_file);
	            
				break;
			}
			case FILE_TRANSFER_DATA: {
				if (response.header.bodyLength == 0) {
					pthread_mutex_lock(&list_mutex);
					auto it = find_message_index_by_id(pending_list, response.header.messageID);
					if (it != pending_list.end()) {
						if (incoming_files[response.header.messageID].is_open()) {
							incoming_files[response.header.messageID].close();
						}
						incoming_files.erase(response.header.messageID);
						pending_list.erase(it);
					} else {
						std::cout << "Original request not found." << std::endl;
					}
					pthread_mutex_unlock(&list_mutex);
					
					break;
				}
				
				// todo: error handling
				incoming_files[response.header.messageID].write(response.body.data(), response.body.size());
	            if (!incoming_files[response.header.messageID]) {
	                std::cerr << "Error writing to file!" << std::endl;
	                incoming_files[response.header.messageID].close();
	            }
				
				break;
			}
			case STREAM_REQ: {
				std::cout << "Call: " << response.bodyAsString() << std::endl;
				update_maximum(next_message_id, response.header.messageID + 1);

				pthread_mutex_lock(&bool_mutex);
				if (incoming_call || outgoing_call) {
					response.header.messageType = RESPONSE_FAIL;
				} else {
					std::string sender, receiver;
		            std::istringstream iss(response.bodyAsString());
		            
		            if (!(iss >> sender >> receiver >> peer_ip >> peer_port)) {
		                std::cerr << "Invalid peer ip or port!" << std::endl;
		                response.header.messageType = RESPONSE_FAIL;
		            } else {
		            	incoming_call = true;
						pthread_cond_signal(&call_pending);
						response.header.messageType = RESPONSE_SUCCESS;
						
					}
				}
				pthread_mutex_unlock(&bool_mutex);
				
				pthread_mutex_lock(&queue_mutex);
				transmit_queue.push(std::move(response));
				pthread_cond_signal(&queue_non_empty);
				pthread_mutex_unlock(&queue_mutex);
				break;
			}
			default: {
				std::cout << "Unknown message type" << std::endl;
				break;
			}
		}
	}
	
	return nullptr;
}

void* call_handler(void* arg) {
	int listen_socket = *(int*) arg;
	while (client_running) {
		pthread_mutex_lock(&bool_mutex);
		while (!incoming_call && !outgoing_call && client_running) {
			pthread_cond_wait(&call_pending, &bool_mutex);
		}
		pthread_mutex_unlock(&bool_mutex);
		
		std::cerr << "aduio thread woke up" << std::endl;
		
		if (!client_running) {
			break;
		}
		
		if (incoming_call) {
			std::cerr << "called by " << peer_ip << " " << peer_port << std::endl;
			
			int peer_socket;
			struct sockaddr_in peer_addr;
			socklen_t addr_len = sizeof(peer_addr);
			
	        peer_socket = accept(listen_socket, (struct sockaddr*)&peer_addr, &addr_len);
	        if (peer_socket == -1) {
	            std::cerr << "Error accepting peer.\n";
				incoming_call = false;
	            
	            break;
	        }
	        
	        std::cout << "Connected to the peer.\n";
	        
			// config setup
		    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
		    deviceConfig.capture.format   = ma_format_f32;
		    deviceConfig.capture.channels = 1; // mono audio
		    deviceConfig.sampleRate       = 48000; // Sample rate
		    deviceConfig.dataCallback     = idata_callback;
		    deviceConfig.pUserData        = &peer_socket;  // Pass the socket to the callback
		    
		    // device setup
		    ma_device device;
		    ma_result result = ma_device_init(NULL, &deviceConfig, &device);
		    if (result != MA_SUCCESS) {
		        std::cerr << "Failed to initialize playback device." << std::endl;
		        close(peer_socket);
		        
				incoming_call = false;
				
		        continue;
		    }
		
		    // Start capturing audio
		    result = ma_device_start(&device);
		    if (result != MA_SUCCESS) {
		        std::cerr << "Failed to start playback device." << std::endl;
		        ma_device_uninit(&device);
		        close(peer_socket);
		        
				pthread_mutex_lock(&bool_mutex);
				incoming_call = false;
				pthread_mutex_unlock(&bool_mutex);
		        
		        continue;
		    }
			
			pthread_mutex_lock(&bool_mutex);
			while (incoming_call) {
				pthread_cond_wait(&call_hang, &bool_mutex);
			}
			pthread_mutex_unlock(&bool_mutex);
			
			
			// clean up
		    ma_device_stop(&device);
		    ma_device_uninit(&device);
			close(peer_socket);
			
			incoming_call = false;
		} else if (outgoing_call) {
			std::cerr << "calling " << peer_ip << " " << peer_port << std::endl;
			
			int peer_socket;
			struct sockaddr_in peer_addr;
			socklen_t addr_len = sizeof(peer_addr);
			
			// peer address
		    peer_addr.sin_family = AF_INET;
		    peer_addr.sin_port = htons(peer_port);
		
		    if (inet_pton(AF_INET, peer_ip.c_str(), &peer_addr.sin_addr) <= 0) {
		        std::cerr << "Invalid address or address not supported." << std::endl;
		        
				outgoing_call = false;
				
				continue;
		    }
		
			// peer socket
		    peer_socket = socket(AF_INET, SOCK_STREAM, 0);
		    if (peer_socket < 0) {
		        std::cerr << "Socket creation failed\n";
		        
				outgoing_call = false;
				
				continue;
		    }
			
			// connect to server
		    if (connect(peer_socket, (struct sockaddr*)&peer_addr, addr_len) < 0) {
		        std::cerr << "Connection failed\n";
		        close(peer_socket);
		        
				outgoing_call = false;
		        
		        continue;
		    }
		
		    std::cout << "Connected to the peer.\n";
		    
			// config setup
			ma_device_config deviceConfig = ma_device_config_init(ma_device_type_capture);
			deviceConfig.capture.format   = ma_format_f32;
		    deviceConfig.capture.channels = 1; // mono audio
		    deviceConfig.sampleRate       = 48000; // Sample rate
		    deviceConfig.dataCallback     = odata_callback;
		    deviceConfig.pUserData        = &peer_socket;  // Pass the socket to the callback
		    
		    // device setup
		    ma_device device;
		    ma_result result = ma_device_init(NULL, &deviceConfig, &device);
		    if (result != MA_SUCCESS) {
		        std::cerr << "Failed to initialize capture device." << std::endl;
		        close(peer_socket);
		        
				outgoing_call = false;
				
		        continue;
		    }
		
		    // Start capturing audio
		    result = ma_device_start(&device);
		    if (result != MA_SUCCESS) {
		        std::cerr << "Failed to start capture device." << std::endl;
		        ma_device_uninit(&device);
		        close(peer_socket);
		        
				outgoing_call = false;
		        
		        continue;
		    }
		    
			pthread_mutex_lock(&bool_mutex);
			while (outgoing_call) {
				if (file_playback_not_initable) {
					ma_decoder_uninit(&file_decoder);
					file_playback_not_initable = false;
				}
				pthread_cond_wait(&call_hang, &bool_mutex);
			}
			pthread_mutex_unlock(&bool_mutex);
			
			// clean up
			ma_device_stop(&device);
		    ma_device_uninit(&device);
		    if (file_playback || file_playback_not_initable) {
		    	ma_decoder_uninit(&file_decoder);
			}
			close(peer_socket);
			
			pthread_mutex_lock(&bool_mutex);
			outgoing_call = false;
			file_playback = false;
			file_playback_not_initable = false;
			pthread_mutex_unlock(&bool_mutex);
		}
	}
	
	return nullptr;
}


std::vector<Message>::const_iterator find_message_index_by_id(const std::vector<Message>& messages, int messageID) {
    return std::find_if(messages.begin(), messages.end(), [messageID](const Message& msg) {
        return msg.header.messageID == messageID;
    });
}