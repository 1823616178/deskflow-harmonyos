# Mbed TLS

The app statically builds the official Mbed TLS **3.6.7** source release
for each target ABI. Bundling source keeps HarmonyOS builds reproducible
and offline; no private system SSL libraries or prebuilt binaries are used.

- Upstream: https://github.com/Mbed-TLS/mbedtls
- Release: https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7
- Archive: https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
- SHA-256: `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`
- License: Apache-2.0 OR GPL-2.0-or-later, preserved in `MBEDTLS-LICENSE.txt` and the archive.

`cmake/MbedTLS.cmake` verifies the archive digest before extraction and
disables upstream programs/tests. To update, replace the archive from an
official security release, update the digest and version in CMake and the
host integration test, preserve notices, and run both ABI builds and the
TLS regression suite.
