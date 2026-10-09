#include "prx/libSceVideoOut/include/AitherBridge.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace AitherBridge {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr int SendFlags = 0;
void closeSocket(long long socket) { closesocket(static_cast<NativeSocket>(socket)); }
void shutdownSocket(long long socket) { shutdown(static_cast<NativeSocket>(socket), SD_BOTH); }
bool invalid(NativeSocket socket) { return socket == INVALID_SOCKET; }
#else
using NativeSocket = int;
constexpr int SendFlags = MSG_NOSIGNAL;
void closeSocket(long long socket) { close(static_cast<NativeSocket>(socket)); }
void shutdownSocket(long long socket) { shutdown(static_cast<NativeSocket>(socket), SHUT_RDWR); }
bool invalid(NativeSocket socket) { return socket < 0; }
#endif

void ensureSockets() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("aither bridge: WSAStartup failed");
        return true;
    }();
    (void)started;
#endif
}

void setSendTimeout(NativeSocket socket, int milliseconds) {
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(milliseconds);
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    timeval value{milliseconds / 1000, (milliseconds % 1000) * 1000};
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
    int one = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

bool parseByte(std::istringstream& in, std::uint8_t& out) {
    long value = 0;
    if (!(in >> value) || value < 0 || value > 255) return false;
    out = static_cast<std::uint8_t>(value);
    return true;
}

}

bool ParseCommand(const std::string& line, PadOverride& state) {
    std::istringstream in(line);
    std::string verb;
    if (!(in >> verb)) return false;
    if (verb == "release") {
        std::string extra;
        if (in >> extra) return false;
        state = PadOverride{};
        return true;
    }
    if (verb != "pad") return false;
    PadOverride next;
    unsigned long long buttons = 0;
    if (!(in >> buttons) || buttons > 0xFFFFFFFFull) return false;
    next.buttons = static_cast<std::uint32_t>(buttons);
    for (auto& stick : next.sticks) {
        if (!parseByte(in, stick)) return false;
    }
    if (!parseByte(in, next.l2) || !parseByte(in, next.r2)) return false;
    std::string extra;
    if (in >> extra) return false;
    next.active = true;
    state = next;
    return true;
}

std::string FrameEvent(std::uint64_t count, std::uint32_t width, std::uint32_t height, std::int32_t buffer, std::uint64_t timeUs) {
    char text[160];
    std::snprintf(text, sizeof(text), "{\"t\":\"frame\",\"n\":%llu,\"w\":%u,\"h\":%u,\"buf\":%d,\"ts_us\":%llu}\n",
                  static_cast<unsigned long long>(count), width, height, buffer, static_cast<unsigned long long>(timeUs));
    return text;
}

Server::Server(std::uint16_t port) {
    ensureSockets();
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (invalid(socket)) throw std::runtime_error("aither bridge: socket() failed");
    int one = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(socket, 1) != 0) {
        closeSocket(static_cast<long long>(socket));
        throw std::runtime_error("aither bridge: cannot listen on 127.0.0.1:" + std::to_string(port));
    }
    socklen_t length = sizeof(address);
    getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length);
    boundPort = ntohs(address.sin_port);
    listener = static_cast<long long>(socket);
    acceptThread = std::jthread([this](std::stop_token token) { acceptLoop(token); });
}

Server::~Server() {
    acceptThread.request_stop();
    shutdownSocket(listener);
    closeSocket(listener);
    dropClient();
    if (acceptThread.joinable()) acceptThread.join();
}

void Server::acceptLoop(std::stop_token token) {
    while (!token.stop_requested()) {
        const NativeSocket accepted = accept(static_cast<NativeSocket>(listener), nullptr, nullptr);
        if (token.stop_requested()) {
            if (!invalid(accepted)) closeSocket(static_cast<long long>(accepted));
            return;
        }
        if (invalid(accepted)) continue;
        setSendTimeout(accepted, 50);
        client.store(static_cast<long long>(accepted));
        sendLine("{\"t\":\"hello\",\"proto\":1}\n");
        readLoop(token, static_cast<long long>(accepted));
        dropClient();
    }
}

void Server::readLoop(std::stop_token token, long long socket) {
    std::string pending;
    char chunk[512];
    while (!token.stop_requested()) {
        const auto received = recv(static_cast<NativeSocket>(socket), chunk, sizeof(chunk), 0);
        if (received <= 0) return;
        pending.append(chunk, static_cast<std::size_t>(received));
        std::size_t end = 0;
        while ((end = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, end);
            pending.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            bool accepted = false;
            {
                std::lock_guard lock(padMutex);
                PadOverride next = pad;
                accepted = ParseCommand(line, next);
                if (accepted) pad = next;
            }
            if (!accepted) sendLine("{\"t\":\"error\",\"msg\":\"bad command\"}\n");
        }
        if (pending.size() > 4096) return;
    }
}

bool Server::sendLine(const std::string& line) {
    std::lock_guard lock(sendMutex);
    const long long socket = client.load();
    if (socket < 0) return false;
    std::size_t sent = 0;
    while (sent < line.size()) {
        const auto result = send(static_cast<NativeSocket>(socket), line.data() + sent, static_cast<int>(line.size() - sent), SendFlags);
        if (result <= 0) {
            shutdownSocket(socket);
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

void Server::dropClient() {
    // Release the pad before the client is marked gone: once Connected() is false, no held
    // button may still be applied.
    {
        std::lock_guard lock(padMutex);
        pad = PadOverride{};
    }
    std::lock_guard lock(sendMutex);
    const long long socket = client.exchange(-1);
    if (socket >= 0) {
        shutdownSocket(socket);
        closeSocket(socket);
    }
}

void Server::OnFrame(std::uint64_t count, std::uint32_t width, std::uint32_t height, std::int32_t buffer) {
    if (client.load() < 0) return;
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    sendLine(FrameEvent(count, width, height, buffer, static_cast<std::uint64_t>(now)));
}

void Server::ApplyPad(std::uint32_t& buttons, std::array<std::uint8_t, 4>& sticks, std::uint8_t& l2, std::uint8_t& r2) {
    std::lock_guard lock(padMutex);
    if (!pad.active) return;
    buttons |= pad.buttons;
    sticks = pad.sticks;
    if (pad.l2 > l2) l2 = pad.l2;
    if (pad.r2 > r2) r2 = pad.r2;
}

Server* Instance() {
    static const std::unique_ptr<Server> server = []() -> std::unique_ptr<Server> {
        const char* value = std::getenv("APS5_AGENT_BRIDGE");
        if (value == nullptr || *value == '\0') return nullptr;
        char* end = nullptr;
        const long port = std::strtol(value, &end, 10);
        if (end == value || *end != '\0' || port < 0 || port > 65535) throw std::runtime_error("APS5_AGENT_BRIDGE must be a TCP port number");
        return std::make_unique<Server>(static_cast<std::uint16_t>(port));
    }();
    return server.get();
}

}
