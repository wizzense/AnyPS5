#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_AITHERBRIDGE_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_AITHERBRIDGE_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace AitherBridge {

struct PadOverride {
    bool active = false;
    std::uint32_t buttons = 0;
    std::array<std::uint8_t, 4> sticks{128, 128, 128, 128};
    std::uint8_t l2 = 0;
    std::uint8_t r2 = 0;
};

bool ParseCommand(const std::string& line, PadOverride& state);
std::string FrameEvent(std::uint64_t count, std::uint32_t width, std::uint32_t height, std::int32_t buffer, std::uint64_t timeUs);

class Server {
public:
    explicit Server(std::uint16_t port);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t Port() const { return boundPort; }
    bool Connected() const { return client.load() >= 0; }
    void OnFrame(std::uint64_t count, std::uint32_t width, std::uint32_t height, std::int32_t buffer);
    void ApplyPad(std::uint32_t& buttons, std::array<std::uint8_t, 4>& sticks, std::uint8_t& l2, std::uint8_t& r2);

private:
    void acceptLoop(std::stop_token token);
    void readLoop(std::stop_token token, long long socket);
    bool sendLine(const std::string& line);
    void dropClient();

    long long listener = -1;
    std::uint16_t boundPort = 0;
    std::atomic<long long> client{-1};
    std::mutex sendMutex;
    std::mutex padMutex;
    PadOverride pad{};
    std::jthread acceptThread;
};

Server* Instance();

}

#endif
