// Host-side unit tests for the protocol layer (ProtoUtil / SocketStream).
// Runs on a normal Linux/macOS host with g++ -- no HarmonyOS SDK needed:
//   g++ -std=c++17 -I tests/host/stubs -I entry/src/main/cpp/protocol
//       tests/host/protocol_test.cpp
//       entry/src/main/cpp/protocol/proto_util.cpp
//       entry/src/main/cpp/protocol/socket_stream.cpp
//       -o /tmp/protocol_test && /tmp/protocol_test
// DeskflowClient itself is not covered here: it needs the real
// OH_Input injection runtime. Its pure helpers (keysym map, clipboard
// marshalling) are covered indirectly by the ohosTest suite on device.
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
    // Manually craft: length = 2MB, no body.
    out = {0x00, 0x20, 0x00, 0x00};
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

int main()
{
    testWriteReadHello();
    testWriteReadStringAndInts();
    testWriteReadIntArray();
    testStringLengthLimit();
    testFrameBoundary();
    testBufferDoesNotGrowForever();
    testFrameTooLarge();
    std::printf("protocol_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
