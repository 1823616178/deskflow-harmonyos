// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "tls_session.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace dfpoc {
namespace {
using Clock = std::chrono::steady_clock;

int waitForIo(int fd, int tlsResult, Clock::time_point deadline)
{
    const short events = tlsResult == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN;
    for (;;) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (ms <= 0) return 0;
        pollfd descriptor{fd, events, 0};
        int rc = ::poll(&descriptor, 1, static_cast<int>(ms));
        if (rc < 0 && errno == EINTR) continue;
        return rc;
    }
}

bool wantIo(int result)
{
    return result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE;
}

std::string describeError(int result)
{
    char text[256]{};
    mbedtls_strerror(result, text, sizeof(text));
    return text;
}

std::string fingerprint(const mbedtls_x509_crt* certificate)
{
    if (!certificate || !certificate->raw.p) return {};
    unsigned char hash[32];
    if (mbedtls_sha256(certificate->raw.p, certificate->raw.len, hash, 0) != 0) return {};
    const char* hex = "0123456789ABCDEF";
    std::string result;
    for (unsigned char byte : hash) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

int sendTls(void* context, const unsigned char* data, size_t size)
{
    const int fd = *static_cast<int*>(context);
    const ssize_t rc = ::send(fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (rc >= 0) return static_cast<int>(rc);
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

int receiveTls(void* context, unsigned char* data, size_t size)
{
    const int fd = *static_cast<int*>(context);
    const ssize_t rc = ::recv(fd, data, size, MSG_DONTWAIT);
    if (rc >= 0) return static_cast<int>(rc);
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
}

struct TlsSession::Impl {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    std::mutex mutex;
    int fd = -1;
    std::string localFingerprint;

    Impl()
    {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&rng);
        mbedtls_x509_crt_init(&certificate);
        mbedtls_pk_init(&key);
    }
    ~Impl()
    {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&certificate);
        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&rng);
        mbedtls_entropy_free(&entropy);
    }

    int createIdentity(const std::string& path)
    {
        int rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
        if (rc == 0) rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                                             mbedtls_ctr_drbg_random, &rng);
        mbedtls_x509write_cert writer;
        mbedtls_x509write_crt_init(&writer);
        mbedtls_mpi serial;
        mbedtls_mpi_init(&serial);
        unsigned char serialBytes[16]{};
        if (rc == 0) rc = mbedtls_ctr_drbg_random(&rng, serialBytes, sizeof(serialBytes));
        serialBytes[0] &= 0x7F;
        serialBytes[0] |= 1;
        if (rc == 0) rc = mbedtls_mpi_read_binary(&serial, serialBytes, sizeof(serialBytes));
        mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&writer, &key);
        mbedtls_x509write_crt_set_issuer_key(&writer, &key);
        if (rc == 0) rc = mbedtls_x509write_crt_set_serial(&writer, &serial);
        if (rc == 0) rc = mbedtls_x509write_crt_set_subject_name(&writer, "CN=Deskflow HarmonyOS");
        if (rc == 0) rc = mbedtls_x509write_crt_set_issuer_name(&writer, "CN=Deskflow HarmonyOS");
        if (rc == 0) rc = mbedtls_x509write_crt_set_validity(&writer, "20200101000000", "20491231235959");
        if (rc == 0) rc = mbedtls_x509write_crt_set_basic_constraints(&writer, 0, -1);
        if (rc == 0) rc = mbedtls_x509write_crt_set_key_usage(&writer, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
        unsigned char certPem[4096]{};
        unsigned char keyPem[4096]{};
        if (rc == 0) rc = mbedtls_x509write_crt_pem(&writer, certPem, sizeof(certPem),
                                                   mbedtls_ctr_drbg_random, &rng);
        if (rc == 0) rc = mbedtls_pk_write_key_pem(&key, keyPem, sizeof(keyPem));
        mbedtls_x509write_crt_free(&writer);
        mbedtls_mpi_free(&serial);
        if (rc != 0) return rc;

        // Persist once with private permissions; never silently rotate an existing identity.
        std::string pem(reinterpret_cast<char*>(certPem));
        pem += reinterpret_cast<char*>(keyPem);
        const std::string pattern = path + ".tmp-XXXXXX";
        std::vector<char> temporary(pattern.begin(), pattern.end());
        temporary.push_back('\0');
        const int output = ::mkstemp(temporary.data()); // creates mode 0600, with no stale-name collision
        if (output < 0) return MBEDTLS_ERR_PK_FILE_IO_ERROR;
        size_t written = 0;
        while (written < pem.size()) {
            const ssize_t count = ::write(output, pem.data() + written, pem.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            written += static_cast<size_t>(count);
        }
        const bool flushed = ::fsync(output) == 0;
        ::close(output);
        const bool saved = written == pem.size() && flushed && ::rename(temporary.data(), path.c_str()) == 0;
        if (!saved) ::unlink(temporary.data());
        if (!saved) return MBEDTLS_ERR_PK_FILE_IO_ERROR;
        return mbedtls_x509_crt_parse(&certificate, certPem, std::strlen(reinterpret_cast<char*>(certPem)) + 1);
    }

    int loadIdentity(const std::string& path)
    {
        const int input = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
        if (input < 0) return errno == ENOENT ? createIdentity(path) : MBEDTLS_ERR_PK_FILE_IO_ERROR;
        struct stat info{};
        if (::fstat(input, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 16384 ||
            ::fchmod(input, 0600) != 0) {
            ::close(input);
            return MBEDTLS_ERR_PK_FILE_IO_ERROR;
        }
        std::vector<unsigned char> pem(static_cast<size_t>(info.st_size) + 1, 0);
        size_t got = 0;
        while (got < pem.size() - 1) {
            const ssize_t count = ::read(input, pem.data() + got, pem.size() - 1 - got);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            got += static_cast<size_t>(count);
        }
        ::close(input);
        if (got != pem.size() - 1) return MBEDTLS_ERR_PK_FILE_IO_ERROR;
        int rc = mbedtls_x509_crt_parse(&certificate, pem.data(), pem.size());
        if (rc == 0) rc = mbedtls_pk_parse_key(&key, pem.data(), pem.size(), nullptr, 0,
                                               mbedtls_ctr_drbg_random, &rng);
        if (rc == 0) rc = mbedtls_pk_check_pair(&certificate.pk, &key, mbedtls_ctr_drbg_random, &rng);
        return rc;
    }
};

TlsSession::TlsSession() : m_impl(std::make_unique<Impl>()) {}
TlsSession::~TlsSession() = default;
const std::string& TlsSession::clientFingerprint() const { return m_impl->localFingerprint; }

bool TlsSession::connect(int fd, const std::string& host, const TlsOptions& options,
                         int timeoutMs, std::string& error)
{
    auto& state = *m_impl;
    state.fd = fd;
    std::string pin;
    if (!normalizeFingerprint(options.serverFingerprint, pin)) {
        error = "TLS_CONFIG_ERROR invalid SHA-256 fingerprint";
        return false;
    }
    const char* personalization = "deskflow-harmonyos-tls";
    int rc = mbedtls_ctr_drbg_seed(&state.rng, mbedtls_entropy_func, &state.entropy,
        reinterpret_cast<const unsigned char*>(personalization), std::strlen(personalization));
    if (rc == 0) rc = state.loadIdentity(options.identityPath);
    if (rc != 0) {
        error = "TLS_CONFIG_ERROR client identity: " + describeError(rc);
        return false;
    }
    state.localFingerprint = fingerprint(&state.certificate);
    rc = mbedtls_ssl_config_defaults(&state.config, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc == 0) rc = mbedtls_ssl_conf_own_cert(&state.config, &state.certificate, &state.key);
    // Deskflow uses self-signed certificates and pins the DER certificate hash.
    // OPTIONAL only defers the trust decision below; no application I/O is
    // exposed until the pin AND all remaining verification flags are checked.
    mbedtls_ssl_conf_authmode(&state.config, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_min_tls_version(&state.config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&state.config, mbedtls_ctr_drbg_random, &state.rng);
#if defined(MBEDTLS_SSL_RENEGOTIATION)
    mbedtls_ssl_conf_renegotiation(&state.config, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
#endif
    if (rc == 0) rc = mbedtls_ssl_setup(&state.ssl, &state.config);
    if (rc == 0) rc = mbedtls_ssl_set_hostname(&state.ssl, host.c_str());
    mbedtls_ssl_set_bio(&state.ssl, &state.fd, sendTls, receiveTls, nullptr);
    if (rc == 0) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while ((rc = mbedtls_ssl_handshake(&state.ssl)) != 0 && wantIo(rc)) {
            if (waitForIo(fd, rc, deadline) <= 0) {
                error = "TLS_HANDSHAKE_ERROR handshake timed out or interrupted";
                return false;
            }
        }
    }
    const auto* peer = mbedtls_ssl_get_peer_cert(&state.ssl);
    const std::string observed = fingerprint(peer);
    // Report the certificate even if the server rejected our not-yet-trusted
    // client identity. Never send Deskflow Hello / clipboard to an untrusted peer.
    if (!observed.empty() && (pin.empty() || observed != pin)) {
        error = std::string(pin.empty() ? "TLS_UNTRUSTED " : "TLS_PIN_MISMATCH ") + observed;
        return false;
    }
    if (rc != 0) {
        error = "TLS_HANDSHAKE_ERROR " + describeError(rc);
        return false;
    }
    if (observed.empty() || pin.empty()) {
        error = "TLS_CONFIG_ERROR server did not present a certificate";
        return false;
    }
    const uint32_t flags = mbedtls_ssl_get_verify_result(&state.ssl);
    const uint32_t allowed = MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH;
    if ((flags & ~allowed) != 0 || mbedtls_x509_time_is_past(&peer->valid_to) ||
        mbedtls_x509_time_is_future(&peer->valid_from)) {
        error = "TLS_CONFIG_ERROR server certificate is expired, not yet valid or invalid";
        return false;
    }
    return true;
}

bool TlsSession::readExact(uint8_t* data, size_t size, int timeoutMs, bool& timedOut)
{
    auto& state = *m_impl;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    timedOut = false;
    size_t got = 0;
    while (got < size) {
        int rc;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            rc = mbedtls_ssl_read(&state.ssl, data + got, size - got);
        }
        if (Clock::now() >= deadline) { timedOut = true; return false; }
        if (rc > 0) { got += static_cast<size_t>(rc); continue; }
        if (!wantIo(rc)) return false;
        const int ready = waitForIo(state.fd, rc, deadline);
        if (ready <= 0) { timedOut = ready == 0; return false; }
    }
    return true;
}

bool TlsSession::writeAll(const uint8_t* data, size_t size, int timeoutMs)
{
    auto& state = *m_impl;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    size_t sent = 0;
    while (sent < size) {
        int rc;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            rc = mbedtls_ssl_write(&state.ssl, data + sent, std::min(size - sent, size_t{16384}));
        }
        if (Clock::now() >= deadline) return false;
        if (rc > 0) { sent += static_cast<size_t>(rc); continue; }
        if (!wantIo(rc) || waitForIo(state.fd, rc, deadline) <= 0) return false;
    }
    return true;
}
}
