# Host unit tests

Protocol-layer tests that run on a normal host (Linux/macOS) with only
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

Clipboard transport tests run the same TypeScript helper used by the app
with Node.js 24+, without the HarmonyOS SDK:

```bash
node tests/host/clipboard_test.mjs
```

These cover UTF-8 byte limits, JSON escaping, emoji boundaries, reordered
and duplicate packets, invalid headers, expiry, and bounded reassembly.

`tests/host/stubs/` holds minimal stand-ins for the HarmonyOS headers
(`hilog/log.h`, `oh_input_manager.h`) used only by these tests.
