# computer-network-final

This project contains a simple client-server application that can be compiled and executed using `make`. The server and client programs communicate with each other over a network.

## Prerequisites

- Ensure that you have `make` installed on your machine.
- A C++ compiler (`g++`).

## File Structure

```
├── README.md 
├── code/            
    ├── Makefile
    ├── MessageProtocol.h
    ├── server.cpp
    └── client.cpp   
```

## Compilation

To compile the project, navigate to the project directory and run:

```
make
```

This will compile the server and client programs into executable files named `server` and `client`.

## Running the Program

### Starting the Server

To start the server, run:

```
./server
```

The server will start listening for incoming client connections on port 8080. Didn't deal with the situation that the port is in use currently.

### Starting the Client

In another terminal window or on a different machine, run the client program:

```
./client
```

The client will attempt to connect to the server and can send/receive data based on the implementation.

## Clean Up

To clean up the compiled files (e.g., `server`, `client`), run:

```
make clean
```

This will remove the compiled binaries and any other build artifacts.