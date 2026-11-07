// TEST-ONLY STUB: minimal stand-ins for the real HarmonyOS headers,
// so the host unit tests can compile without a HarmonyOS SDK.
// Do not use in production code.
#pragma once
#define LOG_APP 0
#define LOG_INFO 0
#define LOG_DEBUG 0
#define LOG_ERROR 0
#define LOG_WARN 0
static inline int OH_LOG_Print(int, int, unsigned int, const char*, const char*, ...) { return 0; }
