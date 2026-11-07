// Regression tests with controllable, test-only input injection failures.
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "socket_stream.h"
// Inspect session state without adding a test API to the production client.
#define private public
#include "deskflow_client.h"
#undef private
#include "proto_util.h"
#include "protocol_types.h"
#include "multimodalinput/oh_input_manager.h"

using namespace dfpoc;
static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
    std::printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

static void testAliases()
{
    DeskflowClient client;
    host_input::keyEvents.clear();
    CHECK(client.setKeyState(kKeyMetaL, true));
    CHECK(client.setKeyState(kKeySuperL, true));
    CHECK(host_input::keyEvents.size() == 1);
    CHECK(client.setKeyState(kKeyMetaL, false));
    CHECK(host_input::keyEvents.size() == 1);
    CHECK(client.setKeyState(kKeySuperL, false));
    CHECK(host_input::keyEvents.size() == 2);
    CHECK(host_input::keyEvents.back().action == KEY_ACTION_UP);
    // Protocol int16 sign extension must not introduce another source key.
    CHECK(client.setKeyState(0xFFFF0000u | kKeyShiftL, true));
    CHECK(client.setKeyState(kKeyShiftL, false));
    CHECK(!client.m_keyStates[kKeyShiftL]);
}

static void testModifierFailure()
{
    DeskflowClient client;
    std::vector<uint8_t> payload;
    ProtoUtil::writef(payload, "%2i%2i%2i", '!', kKeyModifierShift, 1);
    host_input::keyEvents.clear();
    host_input::keyResult = 401;
    client.m_stream.pushToBuffer(payload.data(), payload.size());
    CHECK(client.handleMessage("DKDN"));
    CHECK(!client.m_keyStates[kKeyShiftL]);
    CHECK(!client.m_keyStates['!']);
    CHECK(host_input::keyEvents.size() == 1); // no unshifted punctuation
    host_input::keyResult = INPUT_SUCCESS;
    client.m_stream.pushToBuffer(payload.data(), payload.size());
    CHECK(client.handleMessage("DKDN"));
    CHECK(client.m_keyStates[kKeyShiftL]);
    CHECK(client.m_keyStates['!']);
    CHECK(host_input::keyEvents.size() == 3); // retry Shift before punctuation
    CHECK(host_input::keyEvents[1].code == KEYCODE_SHIFT_LEFT);
    CHECK(client.handleMessage("COUT"));
    CHECK(!client.m_keyStates[kKeyShiftL]);
    CHECK(!client.m_keyStates['!']);
}

static void testReleaseFailure()
{
    DeskflowClient client;
    CHECK(client.setKeyState('a', true));
    CHECK(client.injectMouseButton(1, true));
    host_input::keyResult = 401;
    host_input::mouseResult = 401;
    CHECK(client.handleMessage("COUT"));
    CHECK(client.m_keyStates['a']);
    CHECK(client.m_mouseButtonsDown[1]);
    client.resetSessionState(true);
    CHECK(client.m_keyStates['a']);
    CHECK(client.m_mouseButtonsDown[1]);
    host_input::keyResult = INPUT_SUCCESS;
    host_input::mouseResult = INPUT_SUCCESS;
    client.resetSessionState(true);
    CHECK(client.m_keyStates.empty());
    CHECK(client.m_mouseButtonsDown.empty());
    host_input::mouseResult = 401;
    CHECK(!client.injectMouseButton(1, true));
    CHECK(!client.m_mouseButtonsDown[1]);
    host_input::mouseResult = INPUT_SUCCESS;
}

int main()
{
    testAliases();
    testModifierFailure();
    testReleaseFailure();
    std::printf("client_input_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
