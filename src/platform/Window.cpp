#include "Window.h"

#include "base/Log.h"
#include "render/Gl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fh {

namespace {

constexpr float kStickDeadZone = 0.12F;
constexpr float kTriggerDeadZone = 0.03F;

void* loadGlProc(const char* name)
{
    return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
}

void APIENTRY onGlMessage(GLenum /*source*/, GLenum type, GLuint id, GLenum severity, GLsizei /*length*/,
    const GLchar* message, const void* /*user*/)
{
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
        return;
    }
    if (type == GL_DEBUG_TYPE_ERROR) {
        logError("gl {}: {}", id, message);
    } else {
        logDebug("gl {}: {}", id, message);
    }
}

/// Rescales a right stick axis so the dead zone maps to 0 and full travel
/// to 1. The left stick is shaped as the game shapes it (shapeThumbstick).
float shapeStick(Sint16 raw)
{
    const float value = std::clamp(static_cast<float>(raw) / 32767.0F, -1.0F, 1.0F);
    const float magnitude = std::abs(value);
    if (magnitude < kStickDeadZone) {
        return 0.0F;
    }
    return std::copysign((magnitude - kStickDeadZone) / (1.0F - kStickDeadZone), value);
}

float shapeTrigger(Sint16 raw)
{
    const float value = std::clamp(static_cast<float>(raw) / 32767.0F, 0.0F, 1.0F);
    return value < kTriggerDeadZone ? 0.0F : (value - kTriggerDeadZone) / (1.0F - kTriggerDeadZone);
}

struct KeyBinding {
    Key key;
    SDL_Scancode scancode;
};

constexpr KeyBinding kKeys[] = {
    {Key::W, SDL_SCANCODE_W},
    {Key::A, SDL_SCANCODE_A},
    {Key::S, SDL_SCANCODE_S},
    {Key::D, SDL_SCANCODE_D},
    {Key::Q, SDL_SCANCODE_Q},
    {Key::E, SDL_SCANCODE_E},
    {Key::R, SDL_SCANCODE_R},
    {Key::F, SDL_SCANCODE_F},
    {Key::C, SDL_SCANCODE_C},
    {Key::H, SDL_SCANCODE_H},
    {Key::M, SDL_SCANCODE_M},
    {Key::V, SDL_SCANCODE_V},
    {Key::Up, SDL_SCANCODE_UP},
    {Key::Down, SDL_SCANCODE_DOWN},
    {Key::Left, SDL_SCANCODE_LEFT},
    {Key::Right, SDL_SCANCODE_RIGHT},
    {Key::Space, SDL_SCANCODE_SPACE},
    {Key::Shift, SDL_SCANCODE_LSHIFT},
    {Key::Ctrl, SDL_SCANCODE_LCTRL},
    {Key::Enter, SDL_SCANCODE_RETURN},
    {Key::Escape, SDL_SCANCODE_ESCAPE},
    {Key::Tab, SDL_SCANCODE_TAB},
    {Key::PageUp, SDL_SCANCODE_PAGEUP},
    {Key::PageDown, SDL_SCANCODE_PAGEDOWN},
    {Key::F1, SDL_SCANCODE_F1},
    {Key::F2, SDL_SCANCODE_F2},
    {Key::F3, SDL_SCANCODE_F3},
    {Key::F4, SDL_SCANCODE_F4},
    {Key::F5, SDL_SCANCODE_F5},
    {Key::F11, SDL_SCANCODE_F11},
    {Key::F12, SDL_SCANCODE_F12},
};

struct ButtonBinding {
    Button button;
    SDL_GamepadButton pad;
};

constexpr ButtonBinding kButtons[] = {
    {Button::A, SDL_GAMEPAD_BUTTON_SOUTH},
    {Button::B, SDL_GAMEPAD_BUTTON_EAST},
    {Button::X, SDL_GAMEPAD_BUTTON_WEST},
    {Button::Y, SDL_GAMEPAD_BUTTON_NORTH},
    {Button::LeftShoulder, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {Button::RightShoulder, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {Button::Back, SDL_GAMEPAD_BUTTON_BACK},
    {Button::Start, SDL_GAMEPAD_BUTTON_START},
    {Button::LeftStick, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {Button::RightStick, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {Button::DpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {Button::DpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {Button::DpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {Button::DpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

} // namespace

Window::Window(const WindowOptions& options)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 1);
    if (options.glDebug) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    }

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (options.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    m_window = SDL_CreateWindow(options.title.c_str(), options.width, options.height, flags);
    if (!m_window) {
        const std::string reason = SDL_GetError();
        SDL_Quit();
        throw std::runtime_error("SDL_CreateWindow: " + reason);
    }
    m_fullscreen = options.fullscreen;
    m_context = SDL_GL_CreateContext(m_window);
    if (!m_context) {
        const std::string reason = SDL_GetError();
        SDL_DestroyWindow(m_window);
        SDL_Quit();
        throw std::runtime_error("OpenGL 4.5 core context: " + reason);
    }
    SDL_GL_MakeCurrent(m_window, static_cast<SDL_GLContext>(m_context));
    if (const char* missing = gl::load(loadGlProc)) {
        throw std::runtime_error(std::string("the OpenGL driver lacks ") + missing);
    }
    // Adaptive vsync first, since a missed frame should tear rather than
    // halve the frame rate; plain vsync where the driver lacks it.
    if (options.vsync && !SDL_GL_SetSwapInterval(-1)) {
        SDL_GL_SetSwapInterval(1);
    } else if (!options.vsync) {
        SDL_GL_SetSwapInterval(0);
    }
    if (options.glDebug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(onGlMessage, nullptr);
    }
    logInfo("gl: {} / {} / {}", reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
        reinterpret_cast<const char*>(glGetString(GL_RENDERER)), reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    SDL_GetWindowSizeInPixels(m_window, &m_width, &m_height);
    openFirstGamepad();
}

Window::~Window()
{
    if (m_gamepad) {
        SDL_CloseGamepad(m_gamepad);
    }
    if (m_context) {
        SDL_GL_DestroyContext(static_cast<SDL_GLContext>(m_context));
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
    }
    SDL_Quit();
}

void Window::openFirstGamepad()
{
    int count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&count);
    if (pads && count > 0 && !m_gamepad) {
        m_gamepad = SDL_OpenGamepad(pads[0]);
        if (m_gamepad) {
            logInfo("input: gamepad {}", SDL_GetGamepadName(m_gamepad));
        }
    }
    SDL_free(pads);
}

bool Window::pumpEvents()
{
    m_input.previousButtons = m_input.buttons;
    m_input.previousKeys = m_input.keys;
    m_input.mouseDx = 0.0F;
    m_input.mouseDy = 0.0F;
    m_input.wheel = 0.0F;

    bool open = true;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            open = false;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            m_input.mouseDx += event.motion.xrel;
            m_input.mouseDy += event.motion.yrel;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            m_input.wheel += event.wheel.y;
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            openFirstGamepad();
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (m_gamepad && SDL_GetGamepadID(m_gamepad) == event.gdevice.which) {
                SDL_CloseGamepad(m_gamepad);
                m_gamepad = nullptr;
                openFirstGamepad();
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            SDL_GetWindowSizeInPixels(m_window, &m_width, &m_height);
            break;
        default:
            break;
        }
    }

    int keyCount = 0;
    const bool* keyboard = SDL_GetKeyboardState(&keyCount);
    for (const KeyBinding& binding : kKeys) {
        m_input.keys[static_cast<std::size_t>(binding.key)] = binding.scancode < keyCount && keyboard[binding.scancode];
    }
    if (m_input.held(Key::Shift) == false && keyboard[SDL_SCANCODE_RSHIFT]) {
        m_input.keys[static_cast<std::size_t>(Key::Shift)] = true;
    }
    const SDL_MouseButtonFlags mouse = SDL_GetMouseState(nullptr, nullptr);
    m_input.mouseLeft = (mouse & SDL_BUTTON_LMASK) != 0;
    m_input.mouseRight = (mouse & SDL_BUTTON_RMASK) != 0;
    refreshGamepad();
    return open;
}

void Window::refreshGamepad()
{
    m_input.gamepadConnected = m_gamepad != nullptr;
    if (!m_gamepad) {
        m_input.buttons.fill(false);
        m_input.axes.fill(0.0F);
        return;
    }
    for (const ButtonBinding& binding : kButtons) {
        m_input.buttons[static_cast<std::size_t>(binding.button)] = SDL_GetGamepadButton(m_gamepad, binding.pad);
    }
    auto setAxis = [this](Axis axis, float value) { m_input.axes[static_cast<std::size_t>(axis)] = value; };
    const StickPosition left = shapeThumbstick(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFTX),
        SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFTY), m_leftStickDeadZone);
    setAxis(Axis::LeftX, left.x);
    // Up is positive, as on the 360 pad the game read; SDL's y points down.
    setAxis(Axis::LeftY, -left.y);
    setAxis(Axis::RightX, shapeStick(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHTX)));
    setAxis(Axis::RightY, -shapeStick(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHTY)));
    setAxis(Axis::LeftTrigger, shapeTrigger(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)));
    setAxis(Axis::RightTrigger, shapeTrigger(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)));
}

void Window::swapBuffers()
{
    SDL_GL_SwapWindow(m_window);
}

void Window::setMouseCaptured(bool captured)
{
    if (SDL_SetWindowRelativeMouseMode(m_window, captured)) {
        m_mouseCaptured = captured;
    }
}

void Window::toggleFullscreen()
{
    m_fullscreen = !m_fullscreen;
    SDL_SetWindowFullscreen(m_window, m_fullscreen);
}

void Window::setSize(int width, int height)
{
    SDL_SetWindowSize(m_window, width, height);
    // The request is asynchronous on some video drivers; without the sync a
    // frame drawn next could still see the old size.
    SDL_SyncWindow(m_window);
    SDL_GetWindowSizeInPixels(m_window, &m_width, &m_height);
}

void Window::setTitle(const std::string& title)
{
    SDL_SetWindowTitle(m_window, title.c_str());
}

void Window::rumble(float low, float high, int milliseconds)
{
    if (!m_gamepad) {
        return;
    }
    const auto scale = [](float v) { return static_cast<Uint16>(std::clamp(v, 0.0F, 1.0F) * 65535.0F); };
    SDL_RumbleGamepad(m_gamepad, scale(low), scale(high), static_cast<Uint32>(std::max(0, milliseconds)));
}

} // namespace fh
