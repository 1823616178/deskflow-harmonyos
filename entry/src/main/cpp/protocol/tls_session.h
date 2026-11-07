// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dfpoc {

struct TlsOptions {
    bool enabled = false;
    std::string serverFingerprint;
    std::string identityPath;
};

// Accept Deskflow's SHA-256 display format (hex, optional colons/spaces).
bool normalizeFingerprint(const std::string& input, std::string& output);

#ifdef DF_WITH_MBEDTLS
class TlsSession {
public:
    TlsSession();
    ~TlsSession();
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;
    bool connect(int fd, const std::string& host, const TlsOptions& options,
                 int timeoutMs, std::string& error);
    bool readExact(uint8_t* data, size_t size, int timeoutMs, bool& timedOut);
    bool writeAll(const uint8_t* data, size_t size, int timeoutMs);
    const std::string& clientFingerprint() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
#endif
}
