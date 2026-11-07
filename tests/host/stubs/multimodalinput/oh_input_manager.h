// TEST-ONLY STUB: minimal stand-ins for the real HarmonyOS headers,
// so the host unit tests can compile without a HarmonyOS SDK.
// Do not use in production code.
#pragma once
#include <cstdint>
#include <vector>
#define INPUT_SUCCESS 0
#define INPUT_INJECTION_AUTHORIZED 2
typedef int Input_Result;
typedef int Input_InjectionStatus;
#define UNAUTHORIZED 0
#define AUTHORIZING 1
#define AUTHORIZED 2
typedef int Input_KeyCode;
#define KEYCODE_A 2017
#define KEYCODE_0 2000
#define KEYCODE_SPACE 2050
#define KEYCODE_COMMA 2043
#define KEYCODE_PERIOD 2044
#define KEYCODE_MINUS 2057
#define KEYCODE_EQUALS 2058
#define KEYCODE_LEFT_BRACKET 2059
#define KEYCODE_RIGHT_BRACKET 2060
#define KEYCODE_BACKSLASH 2061
#define KEYCODE_SEMICOLON 2062
#define KEYCODE_APOSTROPHE 2063
#define KEYCODE_SLASH 2064
#define KEYCODE_GRAVE 2056
#define KEYCODE_AT 2065
#define KEYCODE_PLUS 2066
#define KEYCODE_STAR 2010
#define KEYCODE_POUND 2011
#define KEYCODE_DEL 2055
#define KEYCODE_TAB 2049
#define KEYCODE_ENTER 2054
#define KEYCODE_ESCAPE 2070
#define KEYCODE_MOVE_HOME 2081
#define KEYCODE_MOVE_END 2082
#define KEYCODE_DPAD_LEFT 2014
#define KEYCODE_DPAD_UP 2012
#define KEYCODE_DPAD_RIGHT 2015
#define KEYCODE_DPAD_DOWN 2013
#define KEYCODE_PAGE_UP 2068
#define KEYCODE_PAGE_DOWN 2069
#define KEYCODE_INSERT 2083
#define KEYCODE_FORWARD_DEL 2071
#define KEYCODE_LINEFEED 2609
#define KEYCODE_FORWARD 2084
#define KEYCODE_MENU 2067
#define KEYCODE_FIND 2623
#define KEYCODE_CANCEL 2648
#define KEYCODE_HELP 2625
#define KEYCODE_UNDO 2619
#define KEYCODE_REDO 2641
#define KEYCODE_CAPS_LOCK 2074
#define KEYCODE_NUM_LOCK 2102
#define KEYCODE_SCROLL_LOCK 2075
#define KEYCODE_SHIFT_LEFT 2047
#define KEYCODE_SHIFT_RIGHT 2048
#define KEYCODE_CTRL_LEFT 2072
#define KEYCODE_CTRL_RIGHT 2073
#define KEYCODE_ALT_LEFT 2045
#define KEYCODE_ALT_RIGHT 2046
#define KEYCODE_META_LEFT 2076
#define KEYCODE_META_RIGHT 2077
#define KEYCODE_BREAK 2080
#define KEYCODE_SYSRQ 2079
#define KEYCODE_PRINT 2645
#define KEYCODE_SLEEP 2600
#define KEYCODE_MEDIA_EJECT 2088
#define KEYCODE_ZENKAKU_HANKAKU 2601
#define KEYCODE_KATAKANA 2604
#define KEYCODE_KATAKANA_HIRAGANA 2607
#define KEYCODE_HENKAN 2606
#define KEYCODE_HANGUEL 2613
#define KEYCODE_HANJA 2614
#define KEYCODE_F1 2090
#define KEYCODE_VOLUME_MUTE 22
#define KEYCODE_VOLUME_DOWN 17
#define KEYCODE_VOLUME_UP 16
#define KEYCODE_MEDIA_NEXT 12
#define KEYCODE_MEDIA_PREVIOUS 13
#define KEYCODE_MEDIA_STOP 11
#define KEYCODE_MEDIA_PLAY_PAUSE 10
#define KEYCODE_ENVELOPE 2053
#define KEYCODE_BRIGHTNESS_DOWN 41
#define KEYCODE_BRIGHTNESS_UP 40
#define KEYCODE_FORWARDMAIL 2654
#define KEYCODE_HOME 1
#define KEYCODE_REFRESH 2635
#define KEYCODE_NUMPAD_0 2103
#define KEYCODE_NUMPAD_ENTER 2119
#define KEYCODE_NUMPAD_MULTIPLY 2114
#define KEYCODE_NUMPAD_ADD 2116
#define KEYCODE_NUMPAD_COMMA 2118
#define KEYCODE_NUMPAD_SUBTRACT 2115
#define KEYCODE_NUMPAD_DOT 2117
#define KEYCODE_NUMPAD_DIVIDE 2113
#define KEYCODE_NUMPAD_EQUALS 2120
#define MOUSE_BUTTON_LEFT 1
#define MOUSE_BUTTON_MIDDLE 2
#define MOUSE_BUTTON_RIGHT 3
#define MOUSE_ACTION_MOVE 0
#define MOUSE_ACTION_BUTTON_DOWN 1
#define MOUSE_ACTION_BUTTON_UP 2
#define MOUSE_ACTION_AXIS_BEGIN 4
#define MOUSE_ACTION_AXIS_UPDATE 5
#define MOUSE_ACTION_AXIS_END 6
#define MOUSE_AXIS_SCROLL_VERTICAL 0
#define MOUSE_AXIS_SCROLL_HORIZONTAL 1
#define KEY_ACTION_DOWN 0
#define KEY_ACTION_UP 1
struct Input_MouseEvent { int action = 0; int button = 0; };
struct Input_KeyEvent { int action = 0; int code = 0; };
namespace host_input {
inline int keyResult = 0;
inline int mouseResult = 0;
inline std::vector<Input_KeyEvent> keyEvents;
inline std::vector<Input_MouseEvent> mouseEvents;
}
typedef void (*Input_InjectionCallback)(Input_InjectionStatus);
static inline Input_Result OH_Input_RequestInjection(Input_InjectionCallback) { return 0; }
static inline Input_Result OH_Input_QueryAuthorizedStatus(Input_InjectionStatus* s) { *s = AUTHORIZED; return 0; }
static inline void OH_Input_CancelInjection() {}
static inline Input_MouseEvent* OH_Input_CreateMouseEvent() { return new Input_MouseEvent; }
static inline void OH_Input_DestroyMouseEvent(Input_MouseEvent** e) { delete *e; *e = nullptr; }
static inline void OH_Input_SetMouseEventAction(Input_MouseEvent* e, int action) { e->action = action; }
static inline void OH_Input_SetMouseEventDisplayX(Input_MouseEvent*, int) {}
static inline void OH_Input_SetMouseEventDisplayY(Input_MouseEvent*, int) {}
static inline void OH_Input_SetMouseEventGlobalX(Input_MouseEvent*, int) {}
static inline void OH_Input_SetMouseEventGlobalY(Input_MouseEvent*, int) {}
static inline void OH_Input_SetMouseEventButton(Input_MouseEvent* e, int button) { e->button = button; }
static inline void OH_Input_SetMouseEventAxisType(Input_MouseEvent*, int) {}
static inline void OH_Input_SetMouseEventAxisValue(Input_MouseEvent*, float) {}
static inline void OH_Input_SetMouseEventActionTime(Input_MouseEvent*, int64_t) {}
static inline int OH_Input_InjectMouseEventGlobal(Input_MouseEvent* e) {
    host_input::mouseEvents.push_back(*e);
    return host_input::mouseResult;
}
static inline Input_KeyEvent* OH_Input_CreateKeyEvent() { return new Input_KeyEvent; }
static inline void OH_Input_DestroyKeyEvent(Input_KeyEvent** e) { delete *e; *e = nullptr; }
static inline void OH_Input_SetKeyEventAction(Input_KeyEvent* e, int action) { e->action = action; }
static inline void OH_Input_SetKeyEventKeyCode(Input_KeyEvent* e, int code) { e->code = code; }
static inline void OH_Input_SetKeyEventActionTime(Input_KeyEvent*, int64_t) {}
static inline int OH_Input_InjectKeyEvent(Input_KeyEvent* e) {
    host_input::keyEvents.push_back(*e);
    return host_input::keyResult;
}
// --- extra stubs for napi_init.cpp interceptors ---
struct Input_TouchEvent {};
struct Input_AxisEvent {};
typedef void (*Input_KeyEventCallback)(const Input_KeyEvent*);
typedef void (*Input_MouseEventCallback)(const Input_MouseEvent*);
typedef void (*Input_TouchEventCallback)(const Input_TouchEvent*);
typedef void (*Input_AxisEventCallback)(const Input_AxisEvent*);
struct Input_InterceptorEventCallback { Input_MouseEventCallback mouseCallback; Input_TouchEventCallback touchCallback; Input_AxisEventCallback axisCallback; };
typedef int InputEvent_AxisAction;
typedef int InputEvent_SourceType;
typedef int InputEvent_AxisEventType;
#define AXIS_EVENT_TYPE_PINCH 0
#define AXIS_EVENT_TYPE_SCROLL 1
#define AXIS_TYPE_PINCH 0
#define AXIS_TYPE_ROTATE 1
#define AXIS_TYPE_SCROLL_VERTICAL 2
#define AXIS_TYPE_SCROLL_HORIZONTAL 3
static inline int OH_Input_GetKeyEventAction(const Input_KeyEvent*) { return 0; }
static inline int OH_Input_GetKeyEventKeyCode(const Input_KeyEvent*) { return 0; }
static inline int64_t OH_Input_GetKeyEventActionTime(const Input_KeyEvent*) { return 0; }
static inline int OH_Input_GetMouseEventAction(const Input_MouseEvent*) { return 0; }
static inline int OH_Input_GetMouseEventDisplayX(const Input_MouseEvent*) { return 0; }
static inline int OH_Input_GetMouseEventDisplayY(const Input_MouseEvent*) { return 0; }
static inline int OH_Input_GetMouseEventButton(const Input_MouseEvent*) { return 0; }
static inline int OH_Input_GetMouseEventAxisType(const Input_MouseEvent*) { return 0; }
static inline float OH_Input_GetMouseEventAxisValue(const Input_MouseEvent*) { return 0; }
static inline int64_t OH_Input_GetMouseEventActionTime(const Input_MouseEvent*) { return 0; }
static inline int OH_Input_GetMouseEventGlobalX(const Input_MouseEvent*) { return 0; }
static inline int OH_Input_GetMouseEventGlobalY(const Input_MouseEvent*) { return 0; }
static inline int OH_Input_GetMouseEventWindowId(const Input_MouseEvent*) { return 0; }
static inline int OH_Input_GetMouseEventDisplayId(const Input_MouseEvent*) { return 0; }
static inline int OH_Input_GetTouchEventAction(const Input_TouchEvent*) { return 0; }
static inline int OH_Input_GetTouchEventFingerId(const Input_TouchEvent*) { return 0; }
static inline int OH_Input_GetTouchEventDisplayX(const Input_TouchEvent*) { return 0; }
static inline int OH_Input_GetTouchEventDisplayY(const Input_TouchEvent*) { return 0; }
static inline int64_t OH_Input_GetTouchEventActionTime(const Input_TouchEvent*) { return 0; }
static inline int OH_Input_GetTouchEventGlobalX(const Input_TouchEvent*) { return 0; }
static inline int OH_Input_GetTouchEventGlobalY(const Input_TouchEvent*) { return 0; }
static inline int OH_Input_GetTouchEventWindowId(const Input_TouchEvent*) { return 0; }
static inline int64_t OH_Input_GetTouchEventDownTime(const Input_TouchEvent*) { return 0; }
static inline double OH_Input_GetTouchEventPressure(const Input_TouchEvent*) { return 0; }
static inline Input_Result OH_Input_GetAxisEventAction(const Input_AxisEvent*, InputEvent_AxisAction* a) { if (a) *a = 0; return 0; }
static inline Input_Result OH_Input_GetAxisEventDisplayX(const Input_AxisEvent*, float* v) { if (v) *v = 0; return 0; }
static inline Input_Result OH_Input_GetAxisEventDisplayY(const Input_AxisEvent*, float* v) { if (v) *v = 0; return 0; }
static inline Input_Result OH_Input_GetAxisEventActionTime(const Input_AxisEvent*, int64_t* v) { if (v) *v = 0; return 0; }
static inline Input_Result OH_Input_GetAxisEventSourceType(const Input_AxisEvent*, InputEvent_SourceType* v) { if (v) *v = 0; return 0; }
static inline Input_Result OH_Input_GetAxisEventType(const Input_AxisEvent*, InputEvent_AxisEventType* v) { if (v) *v = 0; return 0; }
static inline Input_Result OH_Input_GetAxisEventAxisValue(const Input_AxisEvent*, int, double* v) { if (v) *v = 0; return 0; }
static inline Input_Result OH_Input_AddKeyEventInterceptor(Input_KeyEventCallback, void*) { return 0; }
static inline Input_Result OH_Input_RemoveKeyEventInterceptor() { return 0; }
static inline Input_Result OH_Input_AddInputEventInterceptor(const Input_InterceptorEventCallback*, void*) { return 0; }
static inline Input_Result OH_Input_RemoveInputEventInterceptor() { return 0; }
