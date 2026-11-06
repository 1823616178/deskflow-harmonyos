/*
 * DeskflowPoC -- HarmonyOS Deskflow protocol client
 * POSIX TCP socket stream with buffered reads
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dfpoc {

class SocketStream
{
public:
    SocketStream();
    ~SocketStream();

    // Connect to host:port. Returns true on success.
    // On success the socket gets TCP_NODELAY, TCP keepalive and
    // SO_RCVTIMEO/SO_SNDTIMEO so a silently dead peer is detected
    // instead of blocking recv/write forever.
    bool connect(const std::string& host, uint16_t port, int timeoutMs = 5000);

    // Read exactly n bytes.
    // When a frame is active (after pushToBuffer) reads are confined to
    // that frame: asking for more than the frame holds returns false
    // WITHOUT reading from the socket, so a truncated / mismatched
    // message can never consume the next frame's bytes.
    // When no frame is active, reads fall through to the socket.
    // Returns true on success, false on EOF/error/timeout/overrun.
    bool readExact(uint8_t* data, size_t n);

    // Write bytes. Returns true on success.
    bool writeAll(const uint8_t* data, size_t n);

    // Read one length-prefixed frame (4-byte NBO length + payload).
    // Returns false on EOF/error or oversize frame.
    bool readFrame(std::vector<uint8_t>& out);

    // Write one length-prefixed frame.
    bool writeFrame(const uint8_t* data, size_t n);

    // Push bytes into the internal read buffer (consumed by readExact first).
    // Starts a new frame: subsequent readExact calls are limited to
    // exactly these bytes until they are fully consumed.
    void pushToBuffer(const uint8_t* data, size_t n);

    // Bytes still waiting in the current frame (for diagnostics).
    size_t bufferRemaining() const;

    void close();

    bool isConnected() const { return m_fd >= 0; }

    // True if the most recent socket read failed because SO_RCVTIMEO
    // fired (peer silent). Cleared on the next successful read/connect.
    bool readTimedOut() const { return m_readTimedOut; }

private:
    // Socket-only exact read, used by readFrame for the length prefix.
    // Never touches m_buffer.
    bool readExactSocket(uint8_t* data, size_t n);
    // Drop already-consumed bytes so m_buffer does not grow forever.
    void compactBuffer();

    int m_fd;
    bool m_readTimedOut{false};
    std::vector<uint8_t> m_buffer;
    size_t m_bufferPos;
    // End of the current frame inside m_buffer (== m_buffer.size()
    // while a frame is active). 0 means "no frame active".
    size_t m_frameEnd;
};

} // namespace dfpoc
