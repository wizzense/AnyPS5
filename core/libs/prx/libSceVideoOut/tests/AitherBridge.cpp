#include "prx/libSceVideoOut/include/AitherBridge.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSocket = SOCKET;
void closeTest(TestSocket socket) { closesocket(socket); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
void closeTest(TestSocket socket) { close(socket); }
#endif

namespace {

void check(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

TestSocket connectTo(std::uint16_t port) {
    const TestSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect failed");
    return socket;
}

std::string readLine(TestSocket socket) {
    std::string line;
    char c = 0;
    while (recv(socket, &c, 1, 0) == 1) {
        if (c == '\n') return line;
        line.push_back(c);
    }
    throw std::runtime_error("connection closed before a full line");
}

void sendText(TestSocket socket, const std::string& text) {
    check(send(socket, text.data(), static_cast<int>(text.size()), 0) == static_cast<int>(text.size()), "send failed");
}

template <typename TPredicate>
bool waitFor(TPredicate predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

struct Pad {
    std::uint32_t buttons = 0;
    std::array<std::uint8_t, 4> sticks{128, 128, 128, 128};
    std::uint8_t l2 = 0;
    std::uint8_t r2 = 0;
};

Pad applied(AitherBridge::Server& server) {
    Pad pad;
    server.ApplyPad(pad.buttons, pad.sticks, pad.l2, pad.r2);
    return pad;
}

void testParse() {
    AitherBridge::PadOverride state;
    check(AitherBridge::ParseCommand("pad 16384 0 255 128 128 0 200", state), "valid pad rejected");
    check(state.active && state.buttons == 0x4000 && state.sticks[0] == 0 && state.sticks[1] == 255 && state.r2 == 200, "pad fields wrong");
    check(!AitherBridge::ParseCommand("pad 1 2 3", state), "short pad accepted");
    check(!AitherBridge::ParseCommand("pad 1 0 0 0 0 0 256", state), "out-of-range byte accepted");
    check(!AitherBridge::ParseCommand("pad 1 0 0 0 0 0 0 9", state), "trailing token accepted");
    check(!AitherBridge::ParseCommand("jump", state), "unknown verb accepted");
    check(state.active, "a rejected command changed the state");
    check(AitherBridge::ParseCommand("release", state) && !state.active, "release did not clear");
    check(AitherBridge::FrameEvent(7, 1920, 1080, 2, 99) == "{\"t\":\"frame\",\"n\":7,\"w\":1920,\"h\":1080,\"buf\":2,\"ts_us\":99}\n", "frame event shape");
}

void testRoundTrip() {
    AitherBridge::Server server(0);
    check(server.Port() != 0, "no port bound");
    const TestSocket socket = connectTo(server.Port());
    check(readLine(socket) == "{\"t\":\"hello\",\"proto\":1}", "hello missing");
    check(waitFor([&] { return server.Connected(); }), "client not registered");

    server.OnFrame(1, 1280, 720, 0);
    const auto frame = readLine(socket);
    check(frame.rfind("{\"t\":\"frame\",\"n\":1,\"w\":1280,\"h\":720,\"buf\":0,", 0) == 0, "frame line wrong");

    sendText(socket, "pad 16384 0 128 128 128 0 0\n");
    check(waitFor([&] { return applied(server).buttons == 0x4000; }), "pad not applied");
    check(applied(server).sticks[0] == 0, "stick not applied");

    sendText(socket, "nonsense\n");
    check(readLine(socket) == "{\"t\":\"error\",\"msg\":\"bad command\"}", "bad command not reported");
    check(applied(server).buttons == 0x4000, "bad command cleared the pad");

    closeTest(socket);
    check(waitFor([&] { return !server.Connected(); }), "disconnect not seen");
    check(applied(server).buttons == 0, "pad stuck after disconnect");

    const TestSocket second = connectTo(server.Port());
    check(readLine(second) == "{\"t\":\"hello\",\"proto\":1}", "second client not accepted");
    closeTest(second);
}

}

int main() {
#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    try {
        testParse();
        testRoundTrip();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "aither_bridge: %s\n", error.what());
        return 1;
    }
    std::puts("aither_bridge: ok");
    return 0;
}
