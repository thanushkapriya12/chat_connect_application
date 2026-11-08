#pragma once
#include <iostream>
#include <string>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using Handle = SOCKET;
constexpr Handle invalidHandle = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
using Handle = int;
constexpr Handle invalidHandle = -1;
#endif

class NetworkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// RAII: startup/cleanup of the operating-system network library.
class NetworkRuntime {
public:
    NetworkRuntime() {
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2,2), &data)) throw NetworkError("Network startup failed.");
#endif
    }
    ~NetworkRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};

class Socket {
    Handle handle = invalidHandle;
public:
    Socket() : handle(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) {
        if (handle == invalidHandle) throw NetworkError("Could not create socket.");
    }
    explicit Socket(Handle value) : handle(value) {
        if (handle == invalidHandle) throw NetworkError("Could not accept connection.");
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : handle(std::exchange(other.handle, invalidHandle)) {}
    ~Socket() {
        if (handle == invalidHandle) return;
#ifdef _WIN32
        closesocket(handle);
#else
        ::close(handle);
#endif
    }
    void stop() {
#ifdef _WIN32
        ::shutdown(handle, SD_BOTH);
#else
        ::shutdown(handle, SHUT_RDWR);
#endif
    }
    void listenAt(unsigned short port) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            throw NetworkError("Port unavailable. Close the old server or use another port.");
        if (::listen(handle, 16) != 0) throw NetworkError("Listen failed.");
    }
    Socket acceptOne() { return Socket(::accept(handle, nullptr, nullptr)); }
    // Real TCP peer address; never trust a caller-supplied forwarding header.
    std::string peerIp() const {
        sockaddr_in addr{};
#ifdef _WIN32
        int length=sizeof(addr);
#else
        socklen_t length=sizeof(addr);
#endif
        if(::getpeername(handle,reinterpret_cast<sockaddr*>(&addr),&length)!=0)
            return "unknown";
        char ip[INET_ADDRSTRLEN]{};
        if(!::inet_ntop(AF_INET,&addr.sin_addr,ip,sizeof(ip)))return "unknown";
        return ip;
    }
    int read(char* buffer, int size) { return static_cast<int>(::recv(handle, buffer, size, 0)); }
    void timeout() {
#ifdef _WIN32
        DWORD value = 30000;
#else
        timeval value{30,0};
#endif
        setsockopt(handle,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&value),sizeof(value));
        setsockopt(handle,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&value),sizeof(value));
    }
    bool write(const std::string& frame) {
        std::size_t sent = 0;
        while (sent < frame.size()) {
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            auto count = ::send(handle, frame.data() + sent, static_cast<int>(frame.size() - sent), flags);
            if (count <= 0) return false;
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }
};
