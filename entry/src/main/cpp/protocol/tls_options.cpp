// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "tls_session.h"

namespace dfpoc {
bool normalizeFingerprint(const std::string& input, std::string& output)
{
    output.clear();
    for (unsigned char c : input) {
        if (c == ':' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
        output += static_cast<char>(c);
    }
    return output.empty() || output.size() == 64;
}
}
