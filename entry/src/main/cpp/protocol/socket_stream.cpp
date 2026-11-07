/*
 * DeskflowPoC -- HarmonyOS Deskflow protocol client
 * POSIX TCP socket stream implementation
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "socket_stream.h"

#include <cerrno>
#include <cstring>
#include <chrono>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
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

namespace {
using Clock = std::chrono::steady_clock;

int waitForSocket(int fd, short events, Clock::time_point deadline)
{
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) {
            return 0;
        }
        pollfd descriptor{fd, events, 0};
        const int rc = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return rc;
    }
}
}

SocketStream::SocketStream() : m_fd(-1), m_readTimedOut(false), m_bufferPos(0) {}

SocketStream::SocketStream(SocketStream&& other) noexcept : SocketStream()
{
    std::unique_lock<std::shared_mutex> lock(other.m_socketMutex);
    m_fd = std::exchange(other.m_fd, -1);
    m_readTimedOut = other.m_readTimedOut;
    m_writeFailed = other.m_writeFailed.load();
    m_buffer = std::move(other.m_buffer);
    m_bufferPos = std::exchange(other.m_bufferPos, 0);
    m_frameActive = std::exchange(other.m_frameActive, false);
    m_lastError = std::move(other.m_lastError);
    m_clientFingerprint = std::move(other.m_clientFingerprint);
    m_tlsRejected = other.m_tlsRejected;
    m_tlsPending = std::exchange(other.m_tlsPending, false);
#ifdef DF_WITH_MBEDTLS
    m_tls = std::move(other.m_tls);
#endif
}

SocketStream::~SocketStream()
{
    close();
}

bool SocketStream::connect(const std::string& host, uint16_t port, int timeoutMs, const TlsOptions& tls)
{
    m_lastError.clear();
    m_clientFingerprint.clear();
    m_tlsRejected = false;
#ifndef DF_WITH_MBEDTLS
    if (tls.enabled) {
        m_lastError = "TLS_CONFIG_ERROR TLS support is missing from this build";
        m_tlsRejected = true;
        return false;
    }
#endif
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

    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (struct addrinfo* ai = result; ai != nullptr && Clock::now() < deadline; ai = ai->ai_next) {
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
            // An interrupted wait keeps the original deadline.
            const int sel = waitForSocket(fd, POLLOUT, deadline);
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
            bool configured = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
            // Dead-peer detection at the TCP layer.
            configured = (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) == 0) && configured;
#ifdef TCP_KEEPIDLE
            int idle = 5;
            configured = (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) == 0) && configured;
#endif
#ifdef TCP_KEEPINTVL
            int intvl = 3;
            configured = (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) == 0) && configured;
#endif
#ifdef TCP_KEEPCNT
            int cnt = 3;
            configured = (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) == 0) && configured;
#endif
            struct timeval rtv;
            rtv.tv_sec = kRecvTimeoutSec;
            rtv.tv_usec = 0;
            configured = (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv)) == 0) && configured;
            struct timeval stv;
            stv.tv_sec = kSendTimeoutSec;
            stv.tv_usec = 0;
            configured = (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv)) == 0) && configured;
            if (!configured) {
                ::close(fd);
                continue;
            }
            std::unique_lock<std::shared_mutex> lock(m_socketMutex);
            m_fd = fd;
            m_tlsPending = tls.enabled;
            m_writeFailed = false;
            m_readTimedOut = false;
            freeaddrinfo(result);
            lock.unlock();
#ifdef DF_WITH_MBEDTLS
            if (tls.enabled) {
                auto secure = std::make_unique<TlsSession>();
                bool ready;
                {
                    // Keep the descriptor alive while interrupt() can still wake the handshake.
                    std::shared_lock<std::shared_mutex> ioLock(m_socketMutex);
                    ready = secure->connect(fd, host, tls, timeoutMs, m_lastError);
                    m_clientFingerprint = secure->clientFingerprint();
                }
                if (!ready) {
                    m_tlsRejected = m_lastError.find("TLS_UNTRUSTED") == 0 ||
                        m_lastError.find("TLS_PIN_MISMATCH") == 0 || m_lastError.find("TLS_CONFIG_ERROR") == 0;
                    close();
                    return false;
                }
                std::unique_lock<std::shared_mutex> tlsLock(m_socketMutex);
                if (m_writeFailed.load()) return false;
                m_tls = std::move(secure);
                m_tlsPending = false;
            }
#endif
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
        return;
    }
    // Still bytes left (only possible when a frame was abandoned):
    // drop the consumed prefix instead of letting the vector grow.
    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<long>(m_bufferPos));
    m_bufferPos = 0;
}

size_t SocketStream::bufferRemaining() const
{
    size_t end = m_buffer.size();
    if (end <= m_bufferPos) {
        return 0;
    }
    return end - m_bufferPos;
}

bool SocketStream::readExactSocket(uint8_t* data, size_t n)
{
    std::shared_lock<std::shared_mutex> lock(m_socketMutex);
    if (m_fd < 0 || m_tlsPending) {
        return false;
    }
    m_readTimedOut = false;
#ifdef DF_WITH_MBEDTLS
    if (m_tls) return m_tls->readExact(data, n, kRecvTimeoutSec * 1000, m_readTimedOut);
#endif
    const auto deadline = Clock::now() + std::chrono::seconds(kRecvTimeoutSec);
    size_t got = 0;
    while (got < n) {
        const int ready = waitForSocket(m_fd, POLLIN, deadline);
        if (ready <= 0) {
            m_readTimedOut = ready == 0;
            return false;
        }
        ssize_t rc = ::recv(m_fd, data + got, n - got, MSG_DONTWAIT);
        if (rc > 0) {
            got += static_cast<size_t>(rc);
            m_readTimedOut = false;
        } else if (rc == 0) {
            return false; // EOF
        } else {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
    }
    return true;
}

bool SocketStream::readExact(uint8_t* data, size_t n)
{
    if (m_frameActive) {
        if (n > bufferRemaining()) {
            return false;
        }
        if (n > 0) {
            std::memcpy(data, m_buffer.data() + m_bufferPos, n);
            m_bufferPos += n;
        }
        if (m_bufferPos >= m_buffer.size()) {
            compactBuffer();
        }
        return true;
    }
    return readExactSocket(data, n);
}

bool SocketStream::writeAllSocket(int fd, const uint8_t* data, size_t n)
{
#ifdef DF_WITH_MBEDTLS
    if (m_tls) return m_tls->writeAll(data, n, kSendTimeoutSec * 1000);
#endif
    const auto deadline = Clock::now() + std::chrono::seconds(kSendTimeoutSec);
    size_t sent = 0;
    while (sent < n) {
        if (waitForSocket(fd, POLLOUT, deadline) <= 0) {
            return false;
        }
        ssize_t rc = ::send(fd, data + sent, n - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (rc > 0) {
            sent += static_cast<size_t>(rc);
        } else if (rc < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            // Includes EAGAIN from SO_SNDTIMEO: peer not reading.
            return false;
        }
    }
    return true;
}

bool SocketStream::writeAll(const uint8_t* data, size_t n)
{
    std::lock_guard<std::mutex> writeLock(m_writeMutex);
    std::shared_lock<std::shared_mutex> socketLock(m_socketMutex);
    if (m_fd < 0 || m_writeFailed.load() || m_tlsPending) {
        return false;
    }
    if (writeAllSocket(m_fd, data, n)) {
        return true;
    }
    m_writeFailed = true;
    ::shutdown(m_fd, SHUT_RDWR);
    return false;
}

bool SocketStream::readFrame(std::vector<uint8_t>& out)
{
    // Any leftover bytes from a previous (abandoned) frame must not leak
    // into the next frame's length prefix.
    m_buffer.clear();
    m_bufferPos = 0;
    m_frameActive = false;
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
    if (n > 4 * 1024 * 1024 || (n > 0 && data == nullptr)) {
        return false;
    }
    uint8_t lenb[4] = {
        static_cast<uint8_t>((n >> 24) & 0xFF),
        static_cast<uint8_t>((n >> 16) & 0xFF),
        static_cast<uint8_t>((n >> 8) & 0xFF),
        static_cast<uint8_t>(n & 0xFF)
    };
    // Keep length and payload together, including when the clipboard
    // and heartbeat threads write at the same time.
    std::lock_guard<std::mutex> writeLock(m_writeMutex);
    std::shared_lock<std::shared_mutex> socketLock(m_socketMutex);
    if (m_fd < 0 || m_writeFailed.load() || m_tlsPending) {
        return false;
    }
    if (writeAllSocket(m_fd, lenb, 4) && (n == 0 || writeAllSocket(m_fd, data, n))) {
        return true;
    }
    // Never append another frame to an incomplete write.
    m_writeFailed = true;
    ::shutdown(m_fd, SHUT_RDWR);
    return false;
}

void SocketStream::pushToBuffer(const uint8_t* data, size_t n)
{
    // A new frame replaces any stale buffered bytes.
    m_buffer.clear();
    m_bufferPos = 0;
    if (n > 0) {
        m_buffer.insert(m_buffer.end(), data, data + n);
    }
    m_frameActive = true;
}

void SocketStream::interrupt()
{
    std::shared_lock<std::shared_mutex> lock(m_socketMutex);
    m_writeFailed = true;
    if (m_fd >= 0) {
        ::shutdown(m_fd, SHUT_RDWR);
    }
}

bool SocketStream::isConnected() const
{
    std::shared_lock<std::shared_mutex> lock(m_socketMutex);
    return m_fd >= 0 && !m_writeFailed.load() && !m_tlsPending;
}

void SocketStream::close()
{
    interrupt();
    std::unique_lock<std::shared_mutex> lock(m_socketMutex);
#ifdef DF_WITH_MBEDTLS
    m_tls.reset();
#endif
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_buffer.clear();
    m_bufferPos = 0;
    m_frameActive = false;
    m_tlsPending = false;
}

} // namespace dfpoc
