#ifndef USER_H
#define USER_H

#include <iostream>
#include <string>
#include <functional>
#include <unordered_map>
#include <openssl/sha.h> // OpenSSL for hashing

class t_user {
public:
    t_user(const std::string& uname, const std::string& pwd) 
        : username_(uname), is_logged_in_(false), message_id_(0), pending_call_id_(0) {
        password_hash_ = hash_password(pwd);
    }

    ~t_user() {
        logout();
    }

    std::string username() const {
        return username_;
    }

    bool check_password(const std::string& pwd) const {
        return hash_password(pwd) == password_hash_;
    }

    bool change_password(const std::string& new_password) {
        if (!is_logged_in_) {
            return false;
        }
        password_hash_ = hash_password(new_password);
        return true;
    }

    bool login(const std::string& pwd) {
        if (is_logged_in_) {
            return false;
        }
        if (check_password(pwd)) {
            is_logged_in_ = true;
            pending_call_id_ = 0;
            return true;
        }
        return false;
    }

    bool logout() {
        if (!is_logged_in_) {
            return false;
        }
        is_logged_in_ = false;
        return true;
    }
    
    bool login_status() const {
    	return is_logged_in_;
	}
	
	void set_socket_status(int client_socket, int client_port, const char* client_ip) {
		socket_ = client_socket;
		port_ = client_port;
		ip_ = client_ip;
	}
	
	int socket() const {
		return socket_;
	}
	
	// Should only be call upon login
	void set_message_id(int current_message_id) {
		message_id_ = current_message_id;
	}
	
	// Should be call when recevive any message while login, and forwarding message
	int next_message_id() {
		message_id_++;
		return message_id_;
	}
	
	void set_pending_call_id(int message_id) {
		pending_call_id_ = message_id;
	}
	
	int pending_call_id() const {
		return pending_call_id_;
	}
	
	std::string address() const {
		return ip_ + " " + std::to_string(port_);
	}
private:
    std::string username_;
    std::string password_hash_;
    bool is_logged_in_;
    std::string ip_;
    int socket_, port_;
    int message_id_, pending_call_id_;

    // Hash password using SHA-256
    std::string hash_password(const std::string& password) const {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(password.c_str()), password.size(), hash);
        std::string hash_string;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            hash_string += sprintf_hex(hash[i]);
        }
        return hash_string;
    }

    // Helper function to convert byte to hex string
    std::string sprintf_hex(unsigned char c) const {
        char buffer[3]; // 2 hex chars + null terminator
        sprintf(buffer, "%02x", c);
        return std::string(buffer);
    }
};

#endif // USER_H