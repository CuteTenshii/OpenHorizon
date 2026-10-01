#pragma once

#include "audio/AudioDevice.h"
#include "audio/AudioLibrary.h"
#include "platform/Window.h"
#include "render/PresentTarget.h"
#include "ui/MenuCommon.h"
#include "ui/TitleScreen.h"
#include "vfs/Vfs.h"

#include <filesystem>
#include <memory>

namespace openhorizon {

/// The title screen, before the session loads, with its music on its own audio device closed before the game's.
/// Profile sign-in after Start (front-end request 322) is out of scope; the attract (request 6) is untraced, so an
/// idle screen comes straight back.
class Title {
public:
    /// Throws DataError if the title's scene cannot be loaded.
    Title(Window& window, const std::filesystem::path& gameDir);
    ~Title();
    Title(const Title&) = delete;
    Title& operator=(const Title&) = delete;

    /// True when the player presses Start, false when the window closes.
    bool run();
    /// Draws without presenting. Returns true once the player has pressed Start.
    bool frame(const InputState& controls, float dt);
    bool started() const { return m_started; }
    const ui::TitleScreen& screen() const { return *m_screen; }

private:
    Window& m_window;
    Vfs m_vfs;
    // Declared before the screen, whose music plays on its mixer.
    std::unique_ptr<audio::AudioDevice> m_audio;
    std::unique_ptr<audio::AudioLibrary> m_audioLibrary;
    std::unique_ptr<ui::TitleScreen> m_screen;
    render::PresentTarget m_present;
    /// Half a stick's travel counts as a direction, as in the game's menus.
    ui::MenuInputReader m_menuInput{0.5F};
    bool m_started = false;
};

} // namespace openhorizon
