// erreula: the system error viewer, loaded with OSDynLoad.
//
// On the console it draws error codes and messages over the title's frame
// until the player presses a button. Without a UI for it yet, the runtime
// prints what would be shown and acknowledges it with the first button.
// The "HOME button disabled" sign shows for a second, as on the console.

#include "kernel.h"

#include "cafe/export.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace cafe::os {
namespace {

enum State : uint32_t { kHidden = 0, kFadeIn = 1, kVisible = 2, kFadeOut = 3 };
enum ResultType : uint32_t { kResultNone = 0, kResultFinish = 1 };

std::mutex g_mutex;
bool g_created = false;
State g_state = kHidden;
bool g_button_pressed = false;
std::chrono::steady_clock::time_point g_home_sign_until{};

std::string utf16_to_utf8(uint32_t address) {
    std::string out;
    if (address == 0) return out;
    for (const be<uint16_t>* c = guest<be<uint16_t>>(address); *c != 0; ++c) {
        const uint32_t u = *c;
        if (u < 0x80) {
            out += static_cast<char>(u);
        } else if (u < 0x800) {
            out += static_cast<char>(0xC0 | (u >> 6));
            out += static_cast<char>(0x80 | (u & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (u >> 12));
            out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (u & 0x3F));
        }
    }
    return out;
}

// ErrEulaCreate(work memory, region, language, FSClient*)
bool Create(uint32_t, uint32_t, uint32_t, uint32_t) {
    std::lock_guard lock(g_mutex);
    g_created = true;
    g_state = kHidden;
    g_button_pressed = false;
    return true;
}

void Destroy() {
    std::lock_guard lock(g_mutex);
    g_created = false;
    g_state = kHidden;
}

// AppearArg: +0x00 type (code, message, message with 1 or 2 buttons),
// +0x10 error code, +0x18 message, +0x1C/+0x20 button labels (UTF-16).
void AppearError(uint32_t arg) {
    const uint32_t type = *guest<be<uint32_t>>(arg + 0x00);
    const uint32_t code = *guest<be<uint32_t>>(arg + 0x10);
    if (type == 0) {
        std::fprintf(stderr, "ttt2: erreula: the game shows error code %03u-%04u; acknowledging it\n", code / 10000,
                     code % 10000);
    } else {
        std::fprintf(stderr, "ttt2: erreula: the game shows the message \"%s\"; acknowledging it\n",
                     utf16_to_utf8(*guest<be<uint32_t>>(arg + 0x18)).c_str());
    }
    std::lock_guard lock(g_mutex);
    g_state = kVisible;
    g_button_pressed = true;
}

void DisappearError() {
    std::lock_guard lock(g_mutex);
    g_state = kHidden;
}

uint32_t GetStateErrorViewer() {
    std::lock_guard lock(g_mutex);
    return g_state;
}

bool IsDecideSelectButtonError() {
    std::lock_guard lock(g_mutex);
    return g_button_pressed;
}
bool IsDecideSelectLeftButtonError() { return IsDecideSelectButtonError(); }
bool IsDecideSelectRightButtonError() { return false; }

uint32_t GetResultType() {
    std::lock_guard lock(g_mutex);
    return g_button_pressed ? kResultFinish : kResultNone;
}
int32_t GetResultCode() {
    std::lock_guard lock(g_mutex);
    return g_button_pressed ? 0 : -1;
}
uint32_t GetSelectButtonNumError() { return 0; }

void AppearHomeNixSign(uint32_t) {
    std::lock_guard lock(g_mutex);
    g_home_sign_until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
}
bool IsAppearHomeNixSign() {
    std::lock_guard lock(g_mutex);
    return std::chrono::steady_clock::now() < g_home_sign_until;
}
void DisappearHomeNixSign() {
    std::lock_guard lock(g_mutex);
    g_home_sign_until = {};
}

void Calc(uint32_t) {}
void DrawTV() {}
void DrawDRC() {}
void SetControllerRemo(uint32_t) {}
void ChangeLang(uint32_t) {}
bool IsSelectCursorActive() { return false; }

} // namespace

CAFE_EXPORT(erreula, ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient, Create);
CAFE_EXPORT(erreula, ErrEulaDestroy__3RplFv, Destroy);
CAFE_EXPORT(erreula, ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg, AppearError);
CAFE_EXPORT(erreula, ErrEulaDisappearError__3RplFv, DisappearError);
CAFE_EXPORT(erreula, ErrEulaGetStateErrorViewer__3RplFv, GetStateErrorViewer);
CAFE_EXPORT(erreula, ErrEulaIsDecideSelectButtonError__3RplFv, IsDecideSelectButtonError);
CAFE_EXPORT(erreula, ErrEulaIsDecideSelectLeftButtonError__3RplFv, IsDecideSelectLeftButtonError);
CAFE_EXPORT(erreula, ErrEulaIsDecideSelectRightButtonError__3RplFv, IsDecideSelectRightButtonError);
CAFE_EXPORT(erreula, ErrEulaGetResultType__3RplFv, GetResultType);
CAFE_EXPORT(erreula, ErrEulaGetResultCode__3RplFv, GetResultCode);
CAFE_EXPORT(erreula, ErrEulaGetSelectButtonNumError__3RplFv, GetSelectButtonNumError);
CAFE_EXPORT(erreula, ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg, AppearHomeNixSign);
CAFE_EXPORT(erreula, ErrEulaIsAppearHomeNixSign__3RplFv, IsAppearHomeNixSign);
CAFE_EXPORT(erreula, ErrEulaDisappearHomeNixSign__3RplFv, DisappearHomeNixSign);
CAFE_EXPORT(erreula, ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo, Calc);
CAFE_EXPORT(erreula, ErrEulaDrawTV__3RplFv, DrawTV);
CAFE_EXPORT(erreula, ErrEulaDrawDRC__3RplFv, DrawDRC);
CAFE_EXPORT(erreula, ErrEulaSetControllerRemo__3RplFQ3_2nn7erreula14ControllerType, SetControllerRemo);
CAFE_EXPORT(erreula, ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType, ChangeLang);
CAFE_EXPORT(erreula, ErrEulaIsSelectCursorActive__3RplFv, IsSelectCursorActive);

} // namespace cafe::os
