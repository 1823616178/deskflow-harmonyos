/*
 * DeskflowPoC -- HarmonyOS Deskflow protocol client
 * POSIX TCP socket stream implementation
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "socket_stream.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace dfpoc {

// Heartbeat / liveness: the server sends CALV every 3s and considers the
// client dead after ~9s without a response, so a 10s receive timeout
// detects a silently dead peer (cable pulled, server frozen, NAT drop)
// instead of blocking in recv() forever.
constexpr int kRecvTimeoutSec = 10;
constexpr int kSendTimeoutSec = 5;

SocketStream::SocketStream() : m_fd(-1), m_readTimedOut(false), m_bufferPos(0), m_frameEnd(0) {}

SocketStream::~SocketStream()
{
    close();
}

bool SocketStream::connect(const std::string& host, uint16_t port, int timeoutMs)
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
    if (rc != 0) {
        return false;
    }

    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        // non-blocking connect with timeout
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) {
            ::close(fd);
            continue;
        }
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(fd);
            continue;
        }

        rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            // select() may be interrupted by a signal; retry it.
            int sel = -1;
            do {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(fd, &wfds);
                struct timeval tv;
                tv.tv_sec = timeoutMs / 1000;
                tv.tv_usec = (timeoutMs % 1000) * 1000;
                sel = select(fd + 1, nullptr, &wfds, nullptr, &tv);
            } while (sel < 0 && errno == EINTR);
            if (sel > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
                    err = errno;
                }
                rc = (err == 0) ? 0 : -1;
            } else {
                rc = -1;
            }
        }
        if (rc == 0) {
            if (fcntl(fd, F_SETFL, flags) < 0) {
                ::close(fd);
                continue;
            }
            // Low latency for mouse/keyboard traffic: do not let Nagle
            // delay the small CNOP / input frames.
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            // Dead-peer detection at the TCP layer.
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
            int idle = 5;
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
            int intvl = 3;
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
            int cnt = 3;
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
            struct timeval rtv;
            rtv.tv_sec = kRecvTimeoutSec;
            rtv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
            struct timeval stv;
            stv.tv_sec = kSendTimeoutSec;
            stv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
            m_fd = fd;
            m_readTimedOut = false;
            freeaddrinfo(result);
            return true;
        }
        ::close(fd);
    }
    freeaddrinfo(result);
    return false;
}

void SocketStream::compactBuffer()
{
    if (m_bufferPos == 0) {
        return;
    }
    if (m_bufferPos >= m_buffer.size()) {
        m_buffer.clear();
        m_bufferPos = 0;
        m_frameEnd = 0;
        return;
    }
    // Still bytes left (only possible when a frame was abandoned):
    // drop the consumed prefix instead of letting the vector grow.
    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<long>(m_bufferPos));
    if (m_frameEnd >= m_bufferPos) {
        m_frameEnd -= m_bufferPos;
    } else {
        m_frameEnd = 0;
    }
    m_bufferPos = 0;
}

size_t SocketStream::bufferRemaining() const
{
    size_t end = (m_frameEnd != 0) ? m_frameEnd : m_buffer.size();
    if (end <= m_bufferPos) {
        return 0;
    }
    return end - m_bufferPos;
}

bool SocketStream::readExactSocket(uint8_t* data, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t rc = ::recv(m_fd, data + got, n - got, 0);
        if (rc > 0) {
            got += static_cast<size_t>(rc);
            m_readTimedOut = false;
        } else if (rc == 0) {
            return false; // EOF
        } else {
            if (errno == EINTR) {
                continue;
            }
            // EAGAIN/EWOULDBLOCK here means SO_RCVTIMEO fired:
            // the peer is silent (no CALV for 10s) -> treat as dead.
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                m_readTimedOut = true;
            }
            return false;
        }
    }
    return true;
}

bool SocketStream::readExact(uint8_t* data, size_t n)
{
    size_t got = 0;
    // Consume buffered data first, never past the current frame end.
    size_t end = (m_frameEnd != 0) ? m_frameEnd : m_buffer.size();
    while (got < n && m_bufferPos < end) {
        data[got++] = m_buffer[m_bufferPos++];
    }
    if (got == n) {
        if (m_bufferPos >= m_buffer.size()) {
            compactBuffer();
        }
        return true;
    }
    // Frame active but exhausted: asking for more would consume the
    // next frame's bytes. Fail instead of desyncing the stream.
    if (m_frameEnd != 0) {
        return false;
    }
    // No frame active (length-prefix reads in readFrame): read the
    // remainder straight from the socket.
    if (!readExactSocket(data + got, n - got)) {
        return false;
    }
    if (m_bufferPos >= m_buffer.size()) {
        compactBuffer();
    }
    return true;
}

bool SocketStream::writeAll(const uint8_t* data, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t rc = ::send(m_fd, data + sent, n - sent, MSG_NOSIGNAL);
        if (rc > 0) {
            sent += static_cast<size_t>(rc);
        } else if (rc < 0 && errno == EINTR) {
            continue;
        } else {
            // Includes EAGAIN from SO_SNDTIMEO: peer not reading.
            return false;
        }
    }
    return true;
}

bool SocketStream::readFrame(std::vector<uint8_t>& out)
{
    // Any leftover bytes from a previous (abandoned) frame must not leak
    // into the next frame's length prefix.
    if (m_bufferPos < m_buffer.size()) {
        compactBuffer();
    }
    m_frameEnd = 0;
    uint8_t lenb[4] = {0, 0, 0, 0};
    if (!readExactSocket(lenb, 4)) {
        return false;
    }
    uint32_t len = (static_cast<uint32_t>(lenb[0]) << 24) |
                   (static_cast<uint32_t>(lenb[1]) << 16) |
                   (static_cast<uint32_t>(lenb[2]) << 8) |
                   static_cast<uint32_t>(lenb[3]);
    if (len > 4 * 1024 * 1024) {
        return false;
    }
    out.resize(len);
    if (len > 0 && !readExactSocket(out.data(), len)) {
        return false;
    }
    return true;
}

bool SocketStream::writeFrame(const uint8_t* data, size_t n)
{
    if (n > 4 * 1024 * 1024) {
        return false;
    }
    uint8_t lenb[4] = {
        static_cast<uint8_t>((n >> 24) & 0xFF),
        static_cast<uint8_t>((n >> 16) & 0xFF),
        static_cast<uint8_t>((n >> 8) & 0xFF),
        static_cast<uint8_t>(n & 0xFF)
    };
    if (!writeAll(lenb, 4)) {
        return false;
    }
    return data == nullptr || n == 0 || writeAll(data, n);
}

void SocketStream::pushToBuffer(const uint8_t* data, size_t n)
{
    // A new frame replaces any stale buffered bytes.
    m_buffer.clear();
    m_bufferPos = 0;
    m_buffer.insert(m_buffer.end(), data, data + n);
    m_frameEnd = m_buffer.size();
}

void SocketStream::close()
{
    if (m_fd >= 0) {
        // shutdown() wakes a thread blocked in recv() on this socket;
        // close() alone does not reliably do that on POSIX.
        ::shutdown(m_fd, SHUT_RDWR);
        ::close(m_fd);
        m_fd = -1;
    }
    m_buffer.clear();
    m_bufferPos = 0;
    m_frameEnd = 0;
}

} // namespace dfpoc
