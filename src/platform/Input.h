#pragma once

#include <array>
#include <cstdint>

namespace fh {

/// Logical controls, named after the Xbox 360 pad the game was designed for.
/// Keyboard and mouse map onto the same controls so game code reads one
/// state whatever the player holds.
enum class Button : std::uint8_t {
    A,
    B,
    X,
    Y,
    LeftShoulder,
    RightShoulder,
    Back,
    Start,
    LeftStick,
    RightStick,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Count,
};

enum class Axis : std::uint8_t {
    LeftX,
    LeftY,
    RightX,
    RightY,
    /// Triggers run from 0 (released) to 1.
    LeftTrigger,
    RightTrigger,
    Count,
};

/// Keys the engine's debug and free-camera code reads directly.
enum class Key : std::uint8_t {
    W,
    A,
    S,
    D,
    Q,
    E,
    R,
    F,
    C,
    H,
    M,
    Up,
    Down,
    Left,
    Right,
    Space,
    Shift,
    Ctrl,
    Enter,
    Escape,
    Tab,
    F1,
    F2,
    F3,
    F4,
    F5,
    F11,
    F12,
    Count,
};

/// The controls as of the current frame, and what changed since the last.
struct InputState {
    std::array<bool, static_cast<std::size_t>(Button::Count)> buttons{};
    std::array<bool, static_cast<std::size_t>(Button::Count)> previousButtons{};
    std::array<float, static_cast<std::size_t>(Axis::Count)> axes{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> keys{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> previousKeys{};
    /// Mouse movement this frame in pixels, while the mouse is captured.
    float mouseDx = 0.0F;
    float mouseDy = 0.0F;
    float wheel = 0.0F;
    bool mouseLeft = false;
    bool mouseRight = false;
    bool gamepadConnected = false;

    bool held(Button b) const { return buttons[static_cast<std::size_t>(b)]; }
    bool pressed(Button b) const
    {
        const auto i = static_cast<std::size_t>(b);
        return buttons[i] && !previousButtons[i];
    }
    float axis(Axis a) const { return axes[static_cast<std::size_t>(a)]; }
    bool held(Key k) const { return keys[static_cast<std::size_t>(k)]; }
    bool pressed(Key k) const
    {
        const auto i = static_cast<std::size_t>(k);
        return keys[i] && !previousKeys[i];
    }
};

} // namespace fh
