# Host unit tests

Protocol-layer tests that run on a Linux host (including WSL) with only
g++ — no HarmonyOS SDK, no device needed.

```bash
g++ -std=c++17 -pthread -I tests/host/stubs -I entry/src/main/cpp/protocol \
  tests/host/protocol_test.cpp \
  entry/src/main/cpp/protocol/proto_util.cpp \
  entry/src/main/cpp/protocol/socket_stream.cpp \
  -o /tmp/protocol_test && /tmp/protocol_test
```

Covered: `ProtoUtil::writef/readf` round-trips (Hello, strings, int
arrays, negative ints, 1MB string limit), frame-boundary enforcement
(over-reading a frame fails without consuming the next frame), and
read-buffer compaction over 1,000 frames.

The TCP regression cases also cover empty/exhausted frame boundaries,
interrupting a blocked reader, and concurrent clipboard/heartbeat writes.

Client input regressions use controllable test-only input stubs to check
aliased keys, modifier injection failures and retrying failed releases:

```bash
g++ -std=c++17 -pthread -I tests/host/stubs -I entry/src/main/cpp/protocol \
  tests/host/client_input_test.cpp \
  entry/src/main/cpp/protocol/deskflow_client.cpp \
  entry/src/main/cpp/protocol/proto_util.cpp \
  entry/src/main/cpp/protocol/socket_stream.cpp \
  -o /tmp/client_input_test && /tmp/client_input_test
```

Clipboard transport tests run the same TypeScript helper used by the app
with Node.js 24+, without the HarmonyOS SDK:

```bash
node tests/host/clipboard_test.mjs
```

These cover UTF-8 byte limits, JSON escaping, emoji boundaries, reordered
and duplicate packets, invalid headers, expiry, and bounded reassembly.

`tests/host/stubs/` holds minimal stand-ins for the HarmonyOS headers
(`hilog/log.h`, `oh_input_manager.h`) used only by these tests.

## TLS integration

```bash
python3 tests/host/tls_integration_test.py
node tests/host/tls_settings_test.mjs
```

The native integration suite needs Linux/WSL, Python 3.12+, make, g++ and
openssl. It verifies the bundled Mbed TLS archive hash and builds a local
driver in `.hvigor/tls-host/`; no downloads or HarmonyOS SDK are needed.
It connects the production transport to Python's OpenSSL TLS server and
checks TLS 1.2/1.3 mutual authentication, persistent private client identity,
unknown/changed certificate rejection before application data, buffered
TLS reads, concurrent framed writes, handshake deadlines and interruption.
The Node tests cover fingerprint validation, endpoint-scoped trust and
parsing certificate notices.
