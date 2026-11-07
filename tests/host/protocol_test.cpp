// Host-side unit tests for the protocol layer (ProtoUtil / SocketStream).
// Runs on a Linux host with g++ -- no HarmonyOS SDK needed:
//   g++ -std=c++17 -I tests/host/stubs -I entry/src/main/cpp/protocol
//       tests/host/protocol_test.cpp
//       entry/src/main/cpp/protocol/proto_util.cpp
//       entry/src/main/cpp/protocol/socket_stream.cpp
//       -o /tmp/protocol_test && /tmp/protocol_test
// Input state regressions use controllable stubs in client_input_test.cpp;
// real device injection still requires the HarmonyOS runtime.
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "proto_util.h"
#include "socket_stream.h"

using namespace dfpoc;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

// Build a framed payload the way SocketStream::writeFrame does, then
// feed it to a fresh stream via pushToBuffer and parse it with readf.
static SocketStream streamWithFrame(const std::vector<uint8_t>& payload)
{
    SocketStream s;
    s.pushToBuffer(payload.data(), payload.size());
    return s;
}

static void testWriteReadHello()
{
    // Hello: "%7s%2i%2i" -- fixed-width name, no length prefix.
    std::vector<uint8_t> out;
    std::string name = "Barrier";
    ProtoUtil::writef(out, "%7s%2i%2i", &name, 1, 8);
    CHECK(out.size() == 11);
    CHECK(std::memcmp(out.data(), "Barrier", 7) == 0);
    CHECK(out[7] == 0 && out[8] == 1);   // major 1, big-endian
    CHECK(out[9] == 0 && out[10] == 8);  // minor 8

    SocketStream s = streamWithFrame(out);
    std::string gotName;
    int16_t major = 0, minor = 0;
    CHECK(ProtoUtil::readf(s, "%7s%2i%2i", &gotName, &major, &minor));
    CHECK(gotName.size() == 7 && gotName == "Barrier");
    CHECK(major == 1 && minor == 8);
}

static void testWriteReadStringAndInts()
{
    std::vector<uint8_t> out;
    std::string text = "HarmonyPC";
    // %s: 4-byte NBO length + bytes, no terminator.
    ProtoUtil::writef(out, "%2i%4i%s", -1, 123456, &text);
    CHECK(out.size() == 2 + 4 + 4 + text.size());
    CHECK(out[0] == 0xFF && out[1] == 0xFF);  // -1 as int16

    SocketStream s = streamWithFrame(out);
    int16_t a = 0;
    int32_t b = 0;
    std::string got;
    CHECK(ProtoUtil::readf(s, "%2i%4i%s", &a, &b, &got));
    CHECK(a == -1);
    CHECK(b == 123456);
    CHECK(got == "HarmonyPC");
}

static void testWriteReadIntArray()
{
    std::vector<uint8_t> out;
    std::vector<uint32_t> opts = {10, 20, 30};
    ProtoUtil::writef(out, "%4I", &opts);
    CHECK(out.size() == 4 + 3 * 4);  // count + 3 x int32

    SocketStream s = streamWithFrame(out);
    std::vector<uint32_t> got;
    CHECK(ProtoUtil::readf(s, "%4I", &got));
    CHECK(got == opts);
}

static void testStringLengthLimit()
{
    // A %s length prefix above 1MB must fail, not allocate blindly.
    std::vector<uint8_t> out;
    // Include the full body: failure must be due to the limit, not EOF.
    out = {0x00, 0x20, 0x00, 0x00};
    out.resize(4 + 2 * 1024 * 1024, 'x');
    SocketStream s = streamWithFrame(out);
    std::string got;
    CHECK(!ProtoUtil::readf(s, "%s", &got));
}

static void testFrameBoundary()
{
    // Regression test for the frame-boundary bug: reading past the end
    // of a frame must fail WITHOUT consuming the next frame's bytes.
    std::vector<uint8_t> frame1 = {'C', 'I', 'N', 'N', 0, 1, 0, 2};  // key + 2 x int16
    std::vector<uint8_t> frame2 = {'D', 'M', 'M', 'V', 0, 5, 0, 6};

    SocketStream s;
    s.pushToBuffer(frame1.data(), frame1.size());
    uint8_t head[4] = {};
    CHECK(s.readExact(head, 4));
    CHECK(std::memcmp(head, "CINN", 4) == 0);
    // Asking for 6 bytes when only 4 remain in the frame must fail.
    int16_t x = 0, y = 0, z = 0;
    CHECK(!ProtoUtil::readf(s, "%2i%2i%2i", &x, &y, &z));
    // The next frame must still parse cleanly afterwards.
    s.pushToBuffer(frame2.data(), frame2.size());
    CHECK(s.readExact(head, 4));
    CHECK(std::memcmp(head, "DMMV", 4) == 0);
    CHECK(ProtoUtil::readf(s, "%2i%2i", &x, &y));
    CHECK(x == 5 && y == 6);
}

static void testBufferDoesNotGrowForever()
{
    // Push + fully consume many small frames: the internal buffer must
    // be compacted, not grow by one frame per message.
    SocketStream s;
    for (int i = 0; i < 1000; ++i) {
        std::vector<uint8_t> frame = {'C', 'N', 'O', 'P'};
        s.pushToBuffer(frame.data(), frame.size());
        uint8_t head[4] = {};
        CHECK(s.readExact(head, 4));
        CHECK(s.bufferRemaining() == 0);
    }
    CHECK(s.bufferRemaining() == 0);
}

static void testFrameTooLarge()
{
    // readFrame rejects a >4MB length prefix instead of allocating it.
    // (Exercised via pushToBuffer + readExactSocket is not possible
    // without a socket, so this checks the write side guard instead.)
    SocketStream s;
    std::vector<uint8_t> big(4 * 1024 * 1024 + 1, 0);
    CHECK(!s.writeFrame(big.data(), big.size()));
}

// Real TCP peers are needed here: an invalid descriptor would hide
// parser fall-through by failing the unintended recv() immediately.
static int connectLoopback(SocketStream& stream)
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool listening = ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        ::listen(listener, 1) == 0;
    CHECK(listening);
    if (!listening) { ::close(listener); return -1; }
    socklen_t size = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    const bool connected = stream.connect("127.0.0.1", ntohs(address.sin_port));
    CHECK(connected);
    if (!connected) { ::close(listener); return -1; }
    const int peer = ::accept(listener, nullptr, nullptr);
    ::close(listener);
    CHECK(peer >= 0);
    if (peer >= 0) {
        timeval timeout{2, 0};
        ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    return peer;
}

static void testEmptyAndConsumedFrameBoundaries()
{
    for (bool empty : {true, false}) {
        SocketStream stream;
        const int peer = connectLoopback(stream);
        if (peer < 0) return;
        const uint8_t incoming[] = {0, 0, 0, 4, 'N', 'E', 'X', 'T'};
        CHECK(::send(peer, incoming, sizeof(incoming), MSG_NOSIGNAL) == sizeof(incoming));
        const uint8_t current[] = {'C', 'N', 'O', 'P'};
        stream.pushToBuffer(empty ? nullptr : current, empty ? 0 : sizeof(current));
        uint8_t bytes[4]{};
        if (!empty) CHECK(stream.readExact(bytes, sizeof(bytes)));
        CHECK(!stream.readExact(bytes, 1));
        std::vector<uint8_t> next;
        CHECK(stream.readFrame(next));
        CHECK(next == std::vector<uint8_t>({'N', 'E', 'X', 'T'}));
        ::close(peer);
    }
}

static void testInterruptWakesReader()
{
    SocketStream stream;
    const int peer = connectLoopback(stream);
    if (peer < 0) return;
    bool readResult = true;
    std::thread reader([&] {
        std::vector<uint8_t> frame;
        readResult = stream.readFrame(frame);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto before = std::chrono::steady_clock::now();
    stream.interrupt();
    reader.join();
    CHECK(!readResult);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
    CHECK(!stream.isConnected());
    ::close(peer);

    const uint8_t buffered[] = {'C', 'N', 'O', 'P'};
    stream.pushToBuffer(buffered, sizeof(buffered));
    stream.interrupt();
    uint8_t bytes[4]{};
    CHECK(stream.readExact(bytes, sizeof(bytes)));
    CHECK(std::memcmp(bytes, buffered, sizeof(bytes)) == 0);
}

static bool recvPeerExact(int peer, uint8_t* bytes, size_t size)
{
    size_t received = 0;
    while (received < size) {
        const auto count = ::recv(peer, bytes + received, size - received, 0);
        if (count <= 0) return false;
        received += static_cast<size_t>(count);
    }
    return true;
}

static void testConcurrentWritersKeepFramesIntact()
{
    SocketStream stream;
    const int peer = connectLoopback(stream);
    if (peer < 0) return;
    std::atomic<bool> ok{true};
    auto writeFrames = [&](uint8_t value) {
        const uint8_t data[] = {value, value, value, value};
        for (int i = 0; i < 100; ++i) {
            if (!stream.writeFrame(data, sizeof(data))) ok = false;
        }
    };
    std::thread first(writeFrames, 'A');
    std::thread second(writeFrames, 'B');
    first.join();
    second.join();
    CHECK(ok.load());
    int a = 0, b = 0;
    for (int i = 0; i < 200; ++i) {
        uint8_t frame[8]{};
        const bool received = recvPeerExact(peer, frame, sizeof(frame));
        CHECK(received);
        if (!received) break;
        CHECK(frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 4);
        CHECK(frame[4] == frame[5] && frame[5] == frame[6] && frame[6] == frame[7]);
        if (frame[4] == 'A') ++a;
        if (frame[4] == 'B') ++b;
    }
    CHECK(a == 100 && b == 100);
    ::close(peer);
}

int main()
{
    testWriteReadHello();
    testWriteReadStringAndInts();
    testWriteReadIntArray();
    testStringLengthLimit();
    testFrameBoundary();
    testBufferDoesNotGrowForever();
    testFrameTooLarge();
    testEmptyAndConsumedFrameBoundaries();
    testInterruptWakesReader();
    testConcurrentWritersKeepFramesIntact();
    std::printf("protocol_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
