# Host unit tests

Protocol-layer tests that run on a normal host (Linux/macOS) with only
g++ — no HarmonyOS SDK, no device needed.

```bash
g++ -std=c++17 -I tests/host/stubs -I entry/src/main/cpp/protocol \
  tests/host/protocol_test.cpp \
  entry/src/main/cpp/protocol/proto_util.cpp \
  entry/src/main/cpp/protocol/socket_stream.cpp \
  -o /tmp/protocol_test && /tmp/protocol_test
```

Covered: `ProtoUtil::writef/readf` round-trips (Hello, strings, int
arrays, negative ints, 1MB string limit), frame-boundary enforcement
(over-reading a frame fails without consuming the next frame), and
read-buffer compaction over 1,000 frames.

`tests/host/stubs/` holds minimal stand-ins for the HarmonyOS headers
(`hilog/log.h`, `oh_input_manager.h`) used only by these tests.
