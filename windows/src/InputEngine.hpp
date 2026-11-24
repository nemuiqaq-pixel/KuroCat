#pragma once
#define NOMINMAX
#include <windows.h>
#include <unordered_set>
#include <vector>
#include <deque>
#include <functional>
#include <algorithm>
#include <chrono>
#include "KeyMap.hpp"

/// High-precision monotonic clock in seconds.
inline double nowSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    auto cur = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(cur - start).count();
}

/// Hardware-level full-state keyboard & mouse engine via Windows Raw Input API (RIDEV_INPUTSINK).
/// Captures true N-key rollover make/break scancodes across typing (any IME) and games!
class InputEngine {
public:
    std::unordered_set<PhysicalKey> activeKeys;
    std::vector<PhysicalKey> pressedOrder;

    bool leftMouseDown = false;
    bool rightMouseDown = false;
    bool otherMouseDown = false;
    double lastMouseMoveTime = 0.0;
    double lastMouseClickTime = 0.0;
    double lastKeyActivityTime = 0.0;

    POINT lastCursorPos = { 0, 0 };
    Pt mouseVelocity = { 0.0f, 0.0f };
    float rawDeltaAccumX = 0.0f;
    float rawDeltaAccumY = 0.0f;

    int totalKeystrokesToday = 0;
    bool isRawInputRegistered = false;

    std::function<void(PhysicalKey)> onKeyDown;
    std::function<void(PhysicalKey)> onKeyUp;
    std::function<void(int, bool)> onMouseClick;

    bool registerRawInput(HWND hwnd) {
        RAWINPUTDEVICE rid[2] = {};
        // Keyboard (Generic Desktop 0x01, Usage 0x06)
        rid[0].usUsagePage = 0x01;
        rid[0].usUsage = 0x06;
        rid[0].dwFlags = RIDEV_INPUTSINK;
        rid[0].hwndTarget = hwnd;
        // Mouse (Generic Desktop 0x01, Usage 0x02)
        rid[1].usUsagePage = 0x01;
        rid[1].usUsage = 0x02;
        rid[1].dwFlags = RIDEV_INPUTSINK;
        rid[1].hwndTarget = hwnd;

        isRawInputRegistered = (RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)) != FALSE);
        GetCursorPos(&lastCursorPos);
        return isRawInputRegistered;
    }

    /// Drops every held key / mouse button and re-syncs the cursor. Called after Win+L unlock,
    /// sleep/resume and display changes: the key-up events for Win+L (and anything held while the
    /// secure desktop was up) never reach us, so stale state would otherwise pin the cat's paws.
    void resetAllState() {
        std::vector<PhysicalKey> held(activeKeys.begin(), activeKeys.end());
        rawKeys_.clear();
        asyncKeys_.clear();
        for (PhysicalKey k : held) recomputeUnifiedKeys(k);
        activeKeys.clear();
        pressedOrder.clear();
        if (leftMouseDown) updateMouseButton(0, false);
        if (rightMouseDown) updateMouseButton(1, false);
        if (otherMouseDown) updateMouseButton(2, false);
        rawDeltaAccumX = 0.0f;
        rawDeltaAccumY = 0.0f;
        mouseVelocity = { 0.0f, 0.0f };
        GetCursorPos(&lastCursorPos);
        lastRawKbTime_ = 0.0;
        lastRawMouseTime_ = 0.0;
    }

    /// Seconds since the last keyboard WM_INPUT arrived (used to spot UIPI-blocked games).
    double secondsSinceRawKeyboard(double now) const {
        return lastRawKbTime_ > 0.0 ? now - lastRawKbTime_ : 1e9;
    }

    void handleRawInput(LPARAM lParam) {
        UINT size = 0;
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
            return;
        }
        // Gaming mice can post 1000–8000 WM_INPUT/s: avoid a heap allocation per message.
        alignas(8) BYTE stackBuf[256];
        std::vector<BYTE> heapBuf;
        BYTE* data = stackBuf;
        if (size > sizeof(stackBuf)) {
            heapBuf.resize(size);
            data = heapBuf.data();
        }
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, data, &size, sizeof(RAWINPUTHEADER)) != size) {
            return;
        }

        const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(data);
        if (raw->header.dwType == RIM_TYPEKEYBOARD) {
            const RAWKEYBOARD& kb = raw->data.keyboard;
            // Ignore synthetic overrun / pause prefix
            if (kb.MakeCode == 0 && kb.VKey == 0xFF) return;
            PhysicalKey key = mapRawInputKey(kb.MakeCode, kb.Flags, kb.VKey);
            if (key == PhysicalKey::None) return;

            bool isBreak = (kb.Flags & RI_KEY_BREAK) != 0;
            lastRawKbTime_ = nowSeconds();
            setRawKeyState(key, !isBreak);
        } else if (raw->header.dwType == RIM_TYPEMOUSE) {
            const RAWMOUSE& m = raw->data.mouse;
            double now = nowSeconds();
            lastRawMouseTime_ = now;
            if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && (m.lLastX != 0 || m.lLastY != 0)) {
                rawDeltaAccumX += (float)m.lLastX;
                rawDeltaAccumY += (float)m.lLastY;
                lastMouseMoveTime = now;
            }
            if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)   updateMouseButton(0, true);
            if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)     updateMouseButton(0, false);
            if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)  updateMouseButton(1, true);
            if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)    updateMouseButton(1, false);
            if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) updateMouseButton(2, true);
            if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)   updateMouseButton(2, false);
            if (m.usButtonFlags & RI_MOUSE_WHEEL)              lastMouseMoveTime = now;
        }
    }

    /// Called at 60Hz from main loop to compute smooth cursor velocity (works in desktop & locked-cursor games!).
    void pollMouse(float dt) {
        POINT cur = lastCursorPos;
        GetCursorPos(&cur);
        float dx = (float)(cur.x - lastCursorPos.x);
        float dy = (float)(cur.y - lastCursorPos.y);
        lastCursorPos = cur;

        // If cursor is locked by a game (dx==0, dy==0), fall back to raw mouse hardware deltas!
        if (std::fabs(dx) < 0.5f && std::fabs(dy) < 0.5f && (std::fabs(rawDeltaAccumX) > 0.5f || std::fabs(rawDeltaAccumY) > 0.5f)) {
            dx = rawDeltaAccumX;
            dy = rawDeltaAccumY;
        }
        rawDeltaAccumX = 0.0f;
        rawDeltaAccumY = 0.0f;

        float dist = std::hypot(dx, dy);
        if (dist > 0.4f && dt > 0.001f) {
            lastMouseMoveTime = nowSeconds();
            mouseVelocity.x = dx / dt;
            mouseVelocity.y = dy / dt;
        } else {
            mouseVelocity.x *= 0.82f;
            mouseVelocity.y *= 0.82f;
        }

        double now = nowSeconds();
        pollAsyncFallback(now);
        while (!recentKeyTimes_.empty() && now - recentKeyTimes_.front() > 10.0) {
            recentKeyTimes_.pop_front();
        }
    }

    int currentKPM() const {
        return (int)recentKeyTimes_.size() * 6;
    }

private:
    std::deque<double> recentKeyTimes_;
    std::unordered_set<PhysicalKey> rawKeys_;
    std::unordered_set<PhysicalKey> asyncKeys_;
    double lastRawKbTime_ = 0.0;
    double lastRawMouseTime_ = 0.0;

    void setRawKeyState(PhysicalKey key, bool isDown) {
        if (isDown) {
            rawKeys_.insert(key);
        } else {
            rawKeys_.erase(key);
            asyncKeys_.erase(key);
        }
        recomputeUnifiedKeys(key);
    }

    void pollAsyncFallback(double now) {
        struct VkEntry { int vk; PhysicalKey key; };
        static constexpr VkEntry kTable[] = {
            {'A', PhysicalKey::A}, {'B', PhysicalKey::B}, {'C', PhysicalKey::C}, {'D', PhysicalKey::D},
            {'E', PhysicalKey::E}, {'F', PhysicalKey::F}, {'G', PhysicalKey::G}, {'H', PhysicalKey::H},
            {'I', PhysicalKey::I}, {'J', PhysicalKey::J}, {'K', PhysicalKey::K}, {'L', PhysicalKey::L},
            {'M', PhysicalKey::M}, {'N', PhysicalKey::N}, {'O', PhysicalKey::O}, {'P', PhysicalKey::P},
            {'Q', PhysicalKey::Q}, {'R', PhysicalKey::R}, {'S', PhysicalKey::S}, {'T', PhysicalKey::T},
            {'U', PhysicalKey::U}, {'V', PhysicalKey::V}, {'W', PhysicalKey::W}, {'X', PhysicalKey::X},
            {'Y', PhysicalKey::Y}, {'Z', PhysicalKey::Z},
            {'0', PhysicalKey::K0}, {'1', PhysicalKey::K1}, {'2', PhysicalKey::K2}, {'3', PhysicalKey::K3},
            {'4', PhysicalKey::K4}, {'5', PhysicalKey::K5}, {'6', PhysicalKey::K6}, {'7', PhysicalKey::K7},
            {'8', PhysicalKey::K8}, {'9', PhysicalKey::K9},
            {VK_SPACE, PhysicalKey::Space}, {VK_RETURN, PhysicalKey::Enter}, {VK_BACK, PhysicalKey::Backspace},
            {VK_TAB, PhysicalKey::Tab}, {VK_ESCAPE, PhysicalKey::Escape}, {VK_CAPITAL, PhysicalKey::CapsLock},
            {VK_LSHIFT, PhysicalKey::LeftShift}, {VK_RSHIFT, PhysicalKey::RightShift},
            {VK_LCONTROL, PhysicalKey::LeftControl}, {VK_RCONTROL, PhysicalKey::RightControl},
            {VK_LMENU, PhysicalKey::LeftOption}, {VK_RMENU, PhysicalKey::RightOption},
            {VK_LWIN, PhysicalKey::LeftCommand}, {VK_RWIN, PhysicalKey::RightCommand},
            {VK_UP, PhysicalKey::ArrowUp}, {VK_DOWN, PhysicalKey::ArrowDown},
            {VK_LEFT, PhysicalKey::ArrowLeft}, {VK_RIGHT, PhysicalKey::ArrowRight},
            {VK_F1, PhysicalKey::F1}, {VK_F2, PhysicalKey::F2}, {VK_F3, PhysicalKey::F3}, {VK_F4, PhysicalKey::F4},
            {VK_F5, PhysicalKey::F5}, {VK_F6, PhysicalKey::F6}, {VK_F7, PhysicalKey::F7}, {VK_F8, PhysicalKey::F8},
            {VK_F9, PhysicalKey::F9}, {VK_F10, PhysicalKey::F10}, {VK_F11, PhysicalKey::F11}, {VK_F12, PhysicalKey::F12},
            {VK_OEM_MINUS, PhysicalKey::Minus}, {VK_OEM_PLUS, PhysicalKey::Equal},
            {VK_OEM_4, PhysicalKey::LeftBracket}, {VK_OEM_6, PhysicalKey::RightBracket},
            {VK_OEM_5, PhysicalKey::Backslash}, {VK_OEM_1, PhysicalKey::Semicolon},
            {VK_OEM_7, PhysicalKey::Quote}, {VK_OEM_COMMA, PhysicalKey::Comma},
            {VK_OEM_PERIOD, PhysicalKey::Period}, {VK_OEM_2, PhysicalKey::Slash},
            {VK_OEM_3, PhysicalKey::Grave}, {VK_DELETE, PhysicalKey::ForwardDelete}
        };

        bool rawIdle = (now - lastRawKbTime_) > 0.25;
        for (const auto& e : kTable) {
            bool isDown = (GetAsyncKeyState(e.vk) & 0x8000) != 0;
            bool inRaw = rawKeys_.count(e.key) > 0;
            bool inAsync = asyncKeys_.count(e.key) > 0;
            if (isDown && !inRaw && !inAsync) {
                asyncKeys_.insert(e.key);
                recomputeUnifiedKeys(e.key);
            } else if (!isDown && (inAsync || (rawIdle && inRaw))) {
                asyncKeys_.erase(e.key);
                if (rawIdle) rawKeys_.erase(e.key);
                recomputeUnifiedKeys(e.key);
            }
        }

        if ((now - lastRawMouseTime_) > 0.25) {
            bool lDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            bool rDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            bool mDown = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
            if (lDown != leftMouseDown) updateMouseButton(0, lDown);
            if (rDown != rightMouseDown) updateMouseButton(1, rDown);
            if (mDown != otherMouseDown) updateMouseButton(2, mDown);
        }
    }

    void recomputeUnifiedKeys(PhysicalKey key) {
        bool wasActive = activeKeys.count(key) > 0;
        bool isNowActive = (rawKeys_.count(key) > 0) || (asyncKeys_.count(key) > 0);
        if (isNowActive && !wasActive) {
            activeKeys.insert(key);
            pressedOrder.erase(std::remove(pressedOrder.begin(), pressedOrder.end(), key), pressedOrder.end());
            pressedOrder.push_back(key);
            double now = nowSeconds();
            lastKeyActivityTime = now;
            recentKeyTimes_.push_back(now);
            totalKeystrokesToday++;
            if (onKeyDown) onKeyDown(key);
        } else if (!isNowActive && wasActive) {
            activeKeys.erase(key);
            pressedOrder.erase(std::remove(pressedOrder.begin(), pressedOrder.end(), key), pressedOrder.end());
            if (onKeyUp) onKeyUp(key);
        }
    }

    void updateMouseButton(int button, bool isDown) {
        bool prev = false;
        if (button == 0) { prev = leftMouseDown; leftMouseDown = isDown; }
        else if (button == 1) { prev = rightMouseDown; rightMouseDown = isDown; }
        else { prev = otherMouseDown; otherMouseDown = isDown; }

        if (prev != isDown) {
            double now = nowSeconds();
            lastMouseMoveTime = now;
            if (isDown) lastMouseClickTime = now;
            if (onMouseClick) onMouseClick(button, isDown);
        }
    }
};
