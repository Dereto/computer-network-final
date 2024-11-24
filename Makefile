# Makefile for server and client

# Compiler and flags
CXX = g++
CXXFLAGS = -Wall -std=c++17

# Targets
TARGETS = server client

# Build targets
all: $(TARGETS)

server: server.cpp
	$(CXX) $(CXXFLAGS) -o server server.cpp -lpthread -lssl -lcrypto

client: client.cpp
	$(CXX) $(CXXFLAGS) -o client client.cpp

# Clean up build files
clean:
	rm -f $(TARGETS)

# Phony targets
.PHONY: all clean

