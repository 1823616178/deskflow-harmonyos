// TLS transport regression driver, exercised by tls_integration_test.py.
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "socket_stream.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
using namespace std::string_literals;

int main(int argc, char** argv)
{
    if (argc != 6) return 2;
    dfpoc::SocketStream stream;
    dfpoc::TlsOptions tls{true, argv[2], argv[3]};
    const std::string mode = argv[4];
    const int timeout = std::stoi(argv[5]);
    bool connected = false;
    bool plaintextBlocked = true;
    if (mode == "interrupt") {
        std::atomic<bool> finished{false};
        std::thread connector([&] {
            connected = stream.connect("127.0.0.1", std::stoi(argv[1]), timeout, tls);
            finished = true;
        });
        for (int attempt = 0; attempt < 50 && !finished; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const uint8_t forbidden[] = {'B', 'a', 'r', 'r', 'i', 'e', 'r'};
            if (stream.writeFrame(forbidden, sizeof(forbidden))) plaintextBlocked = false;
            stream.interrupt();
        }
        connector.join();
        if (!plaintextBlocked) return 7;
    } else {
        connected = stream.connect("127.0.0.1", std::stoi(argv[1]), timeout, tls);
    }
    if (!connected) {
        std::printf("REJECT %s\nCLIENT %s\n", stream.lastError().c_str(), stream.clientFingerprint().c_str());
        return mode == "reject" || mode == "interrupt" ? 0 : 1;
    }
    if (mode == "reject" || mode == "interrupt") return 3;
    std::vector<uint8_t> frame;
    // Length and payload are in one TLS record: the second read must
    // consume buffered plaintext without waiting for another TCP packet.
    if (!stream.readFrame(frame) || std::string(frame.begin(), frame.end()) != "Barrier\0\1\0\10"s) return 4;
    const std::string response = "Barrier\0\1\0\10\0\0\0\11HarmonyPC"s;
    if (!stream.writeFrame(reinterpret_cast<const uint8_t*>(response.data()), response.size())) return 5;
    std::atomic<bool> writesOk{true};
    auto writer = [&](uint8_t value) {
        std::vector<uint8_t> body(4096, value);
        for (int i = 0; i < 100; ++i)
            if (!stream.writeFrame(body.data(), body.size())) { writesOk = false; return; }
    };
    std::thread first(writer, 'A');
    std::thread second(writer, 'B');
    bool readsOk = true;
    for (int i = 0; i < 160; ++i) {
        if (!stream.readFrame(frame) || std::string(frame.begin(), frame.end()) != "CALV") { readsOk = false; break; }
    }
    first.join();
    second.join();
    if (!readsOk || !writesOk) return 6;
    std::printf("OK CLIENT %s\n", stream.clientFingerprint().c_str());
    return 0;
}
