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

/// The title screen the game opens on, before anything of the session
/// loads: ui::TitleScreen in the game's 16:9 picture, with its music on its
/// own audio device, which is closed with it before the game opens its own.
///
/// After Start the game signs in a profile (front-end request 322) and loads
/// the save; that step is out of scope, so Start goes straight on to the
/// game. When the screen idles into its attract it is brought back at once,
/// since what the game's request 6 plays was not traced.
class Title {
public:
    /// Throws DataError if the title's scene cannot be loaded.
    Title(Window& window, const std::filesystem::path& gameDir);
    ~Title();
    Title(const Title&) = delete;
    Title& operator=(const Title&) = delete;

    /// Runs on the window's input and clock until the player presses Start
    /// (true) or closes the window (false).
    bool run();
    /// Runs one frame of `dt` seconds on `controls` and draws it into the
    /// 16:9 picture without presenting it. Returns true once the player has
    /// pressed Start.
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
