// swkbd: the system software keyboard, loaded with OSDynLoad.
//
// On the console it is a library with its own renderer that titles draw
// into their frames. This port has no keyboard UI yet: the keyboard is
// never visible, and when a title asks for text it is answered at once with
// the text the title offered, as if the player confirmed it unchanged.
//
// Exports carry their C++ mangled names (nn::swkbd in namespace Rpl).

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/sysmem.h"

#include <cstdio>
#include <cstring>

namespace cafe::os {
namespace {

enum State : int32_t { kHidden = 0, kFadeIn = 1, kVisible = 2, kFadeOut = 3 };

constexpr uint32_t kMaxText = 0x100; // UTF-16 code units, terminator included

std::mutex g_mutex;
bool g_created = false;
bool g_active = false;
bool g_decided = false;
int32_t g_font_work = 0;
int32_t g_predict_work = 0;
uint32_t g_text = 0; // guest UTF-16BE buffer handed to the title

void set_text(uint32_t source, uint32_t max_length) {
    if (g_text == 0) g_text = system_alloc(kMaxText * 2, 16);
    be<uint16_t>* out = guest<be<uint16_t>>(g_text);
    const be<uint16_t>* in = guest<be<uint16_t>>(source);
    const uint32_t limit = (max_length == 0 || max_length >= kMaxText) ? kMaxText - 1 : max_length;
    uint32_t n = 0;
    for (; in != nullptr && n < limit && in[n] != 0; ++n) out[n] = in[n];
    out[n] = 0;
}

// SwkbdCreate(work memory, region, unknown, FSClient*)
void Create(uint32_t, int32_t, uint32_t, uint32_t) {
    std::lock_guard lock(g_mutex);
    g_created = true;
    g_active = false;
    g_decided = false;
    // The real library loads its font and prediction data on the title's
    // sub-threads; report a little such work so titles that wait for it
    // see it done.
    g_font_work = 3;
    g_predict_work = 3;
}

void Destroy() {
    std::lock_guard lock(g_mutex);
    g_created = false;
    g_active = false;
}

void Calc(uint32_t) {}
void DrawTV() {}
void DrawDRC() {}

bool IsNeedCalcSubThreadFont() { std::lock_guard lock(g_mutex); return g_font_work > 0; }
bool IsNeedCalcSubThreadPredict() { std::lock_guard lock(g_mutex); return g_predict_work > 0; }
void CalcSubThreadFont() { std::lock_guard lock(g_mutex); if (g_font_work > 0) --g_font_work; }
void CalcSubThreadPredict() { std::lock_guard lock(g_mutex); if (g_predict_work > 0) --g_predict_work; }

// AppearArg: KeyboardArg (0xC0 bytes) then InputFormArg; the form's initial
// text pointer is at +0xC8 and its maximum length at +0xD0.
bool AppearInputForm(uint32_t arg) {
    std::lock_guard lock(g_mutex);
    set_text(*guest<be<uint32_t>>(arg + 0xC8), *guest<be<uint32_t>>(arg + 0xD0));
    g_active = true;
    g_decided = true;
    std::fprintf(stderr, "ttt2: swkbd: text input requested; confirming the initial text (no keyboard UI yet)\n");
    return true;
}

bool AppearKeyboard(uint32_t) {
    std::lock_guard lock(g_mutex);
    set_text(0, 0);
    g_active = true;
    g_decided = true;
    std::fprintf(stderr, "ttt2: swkbd: keyboard requested; confirming empty input (no keyboard UI yet)\n");
    return true;
}

bool Disappear() {
    std::lock_guard lock(g_mutex);
    g_active = false;
    return true;
}

int32_t GetState() {
    std::lock_guard lock(g_mutex);
    return g_active ? kVisible : kHidden;
}

GuestAddress GetInputFormString() {
    std::lock_guard lock(g_mutex);
    if (g_text == 0) set_text(0, 0);
    return GuestAddress{g_text};
}

bool IsDecideOkButton(uint32_t) {
    std::lock_guard lock(g_mutex);
    return g_decided;
}
bool IsDecideCancelButton(uint32_t) { return false; }

void SetReceiver(uint32_t) {}
void SetControllerRemo(int32_t) {}
void SetCursorPos(int32_t) {}
void SetSelectFrom(int32_t) {}
void SetEnableOkButton(bool) {}
void SetUserControllerEventObj(uint32_t) {}
void SetUserSoundObj(uint32_t) {}
void ConfirmUnfixAll() {}
void InactivateSelectCursor() {}
bool IsSelectCursorActive() { return false; }
bool IsCoveredWithSubWindow() { return false; }
bool IsKeyboardTarget(uint32_t) { return false; }

void GetKeyboardCondition(uint32_t condition) {
    if (condition != 0) std::memset(guest<uint8_t>(condition), 0, 8);
}

void GetDrawStringInfo(uint32_t info) {
    if (info == 0) return;
    for (uint32_t i = 0; i < 6; ++i) *guest<be<int32_t>>(info + i * 4) = -1;
    *guest<be<int32_t>>(info + 0x18) = 0;
}

// An empty learning (prediction) dictionary, in the layout the library
// keeps in the title's save data: 1000 entries of 0x20 bytes behind two
// index tables, "NJDC" at both ends.
bool InitLearnDic(uint32_t dictionary) {
    if (dictionary == 0) return false;
    constexpr uint32_t kSize = 0xA460, kEntries = 1000, kStride = 0x20, kMagic = 0x4E4A4443;
    constexpr uint32_t kFirstIndex = 0x48;
    constexpr uint32_t kSecondIndex = kFirstIndex + (kEntries + 1) * 2;
    constexpr uint32_t kIndexEnd = kSecondIndex + (kEntries + 1) * 2;
    std::memset(guest<uint8_t>(dictionary), 0, kSize);
    const uint32_t header[] = {kMagic, 0x30000, 0x80020000, kSize - 0x48, 0x2C, 0x6E, 0x6E, kIndexEnd, 0, 0,
                               kEntries, kStride, 0, 0, 0, kFirstIndex, kSecondIndex, kIndexEnd + kEntries * kStride};
    for (uint32_t i = 0; i < std::size(header); ++i) *guest<be<uint32_t>>(dictionary + i * 4) = header[i];
    *guest<be<uint32_t>>(dictionary + kSize - 4) = kMagic;
    return true;
}

} // namespace

CAFE_EXPORT(swkbd, SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient, Create);
CAFE_EXPORT(swkbd, SwkbdDestroy__3RplFv, Destroy);
CAFE_EXPORT(swkbd, SwkbdCalc__3RplFRCQ3_2nn5swkbd14ControllerInfo, Calc);
CAFE_EXPORT(swkbd, SwkbdDrawTV__3RplFv, DrawTV);
CAFE_EXPORT(swkbd, SwkbdDrawDRC__3RplFv, DrawDRC);
CAFE_EXPORT(swkbd, SwkbdIsNeedCalcSubThreadFont__3RplFv, IsNeedCalcSubThreadFont);
CAFE_EXPORT(swkbd, SwkbdIsNeedCalcSubThreadPredict__3RplFv, IsNeedCalcSubThreadPredict);
CAFE_EXPORT(swkbd, SwkbdCalcSubThreadFont__3RplFv, CalcSubThreadFont);
CAFE_EXPORT(swkbd, SwkbdCalcSubThreadPredict__3RplFv, CalcSubThreadPredict);
CAFE_EXPORT(swkbd, SwkbdAppearInputForm__3RplFRCQ3_2nn5swkbd9AppearArg, AppearInputForm);
CAFE_EXPORT(swkbd, SwkbdAppearKeyboard__3RplFRCQ3_2nn5swkbd11KeyboardArg, AppearKeyboard);
CAFE_EXPORT(swkbd, SwkbdDisappearInputForm__3RplFv, Disappear);
CAFE_EXPORT(swkbd, SwkbdDisappearKeyboard__3RplFv, Disappear);
CAFE_EXPORT(swkbd, SwkbdGetStateInputForm__3RplFv, GetState);
CAFE_EXPORT(swkbd, SwkbdGetStateKeyboard__3RplFv, GetState);
CAFE_EXPORT(swkbd, SwkbdGetInputFormString__3RplFv, GetInputFormString);
CAFE_EXPORT(swkbd, SwkbdIsDecideOkButton__3RplFPb, IsDecideOkButton);
CAFE_EXPORT(swkbd, SwkbdIsDecideCancelButton__3RplFPb, IsDecideCancelButton);
CAFE_EXPORT(swkbd, SwkbdSetReceiver__3RplFRCQ3_2nn5swkbd11ReceiverArg, SetReceiver);
CAFE_EXPORT(swkbd, SwkbdSetControllerRemo__3RplFQ3_2nn5swkbd14ControllerType, SetControllerRemo);
CAFE_EXPORT(swkbd, SwkbdSetCursorPos__3RplFi, SetCursorPos);
CAFE_EXPORT(swkbd, SwkbdSetSelectFrom__3RplFi, SetSelectFrom);
CAFE_EXPORT(swkbd, SwkbdSetEnableOkButton__3RplFb, SetEnableOkButton);
CAFE_EXPORT(swkbd, SwkbdSetUserControllerEventObj__3RplFPQ3_2nn5swkbd19IControllerEventObj, SetUserControllerEventObj);
CAFE_EXPORT(swkbd, SwkbdSetUserSoundObj__3RplFPQ3_2nn5swkbd9ISoundObj, SetUserSoundObj);
CAFE_EXPORT(swkbd, SwkbdConfirmUnfixAll__3RplFv, ConfirmUnfixAll);
CAFE_EXPORT(swkbd, SwkbdInactivateSelectCursor__3RplFv, InactivateSelectCursor);
CAFE_EXPORT(swkbd, SwkbdIsSelectCursorActive__3RplFv, IsSelectCursorActive);
CAFE_EXPORT(swkbd, SwkbdIsCoveredWithSubWindow__3RplFv, IsCoveredWithSubWindow);
CAFE_EXPORT(swkbd, SwkbdIsKeyboardTarget__3RplFPQ3_2nn5swkbd14IEventReceiver, IsKeyboardTarget);
CAFE_EXPORT(swkbd, SwkbdGetKeyboardCondition__3RplFPQ3_2nn5swkbd17KeyboardCondition, GetKeyboardCondition);
CAFE_EXPORT(swkbd, SwkbdGetDrawStringInfo__3RplFPQ3_2nn5swkbd14DrawStringInfo, GetDrawStringInfo);
CAFE_EXPORT(swkbd, SwkbdInitLearnDic__3RplFPv, InitLearnDic);

} // namespace cafe::os
