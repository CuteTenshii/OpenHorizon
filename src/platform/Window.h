#pragma once

#include "Input.h"
#include "PadInput.h"

#include <stdexcept>
#include <string>

struct SDL_Window;
struct SDL_Gamepad;

namespace openhorizon {

struct WindowOptions {
    std::string title = "OpenHorizon";
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    bool vsync = true;
    /// Requests a debug context and logs driver messages.
    bool glDebug = false;
    /// A window that is never shown, for rendering to files.
    bool hidden = false;
};

/// The graphics driver cannot give the game what it needs: no OpenGL 4.5
/// core context, or one without an entry point the game calls. Something
/// the player fixes by updating the driver, unlike a missing window system.
class GraphicsDriverError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The game window with its OpenGL 4.5 core context, and the source of
/// input. One per process.
class Window {
public:
    /// Creates the window and context and loads the GL entry points. Throws
    /// GraphicsDriverError when the context or an entry point is missing,
    /// std::runtime_error with the reason for any other failure.
    explicit Window(const WindowOptions& options);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// Handles pending events and refreshes input(). Returns false once the
    /// window has been asked to close.
    bool pumpEvents();
    void swapBuffers();

    const InputState& input() const { return m_input; }

    /// Size of the drawable area in pixels.
    int width() const { return m_width; }
    int height() const { return m_height; }

    /// Hides the cursor and reports relative motion, for looking around.
    void setMouseCaptured(bool captured);
    bool mouseCaptured() const { return m_mouseCaptured; }
    void toggleFullscreen();
    /// Resizes a windowed window to `width` x `height` and waits for the
    /// change, so width() and height() report the new drawable size at once.
    void setSize(int width, int height);
    void setTitle(const std::string& title);

    /// The SDL window, for a message box to open over it.
    SDL_Window* sdlWindow() const { return m_window; }

    /// Sets the pad's rumble motors, each 0 to 1, for `milliseconds`.
    void rumble(float low, float high, int milliseconds);

    /// The left stick's dead zone, in raw pad units; the game sets it from
    /// the profile's controller options (controllerSteeringDeadZone()).
    void setLeftStickDeadZone(const StickDeadZone& deadZone) { m_leftStickDeadZone = deadZone; }

private:
    void openFirstGamepad();
    void refreshGamepad();

    SDL_Window* m_window = nullptr;
    void* m_context = nullptr;
    SDL_Gamepad* m_gamepad = nullptr;
    InputState m_input;
    StickDeadZone m_leftStickDeadZone = kPadDefaultStickDeadZone;
    int m_width = 0;
    int m_height = 0;
    bool m_mouseCaptured = false;
    bool m_fullscreen = false;
};

} // namespace openhorizon
