#include "Title.h"

#include "base/Log.h"
#include "platform/Input.h"
#include "render/Gl.h"

#include <algorithm>
#include <chrono>

namespace openhorizon {

Title::Title(Window& window, const std::filesystem::path& gameDir)
    : m_window(window)
{
    m_vfs.mountDisc(gameDir);
    ui::TitleAudio audio;
    try {
        m_audio = std::make_unique<audio::AudioDevice>();
        m_audioLibrary = std::make_unique<audio::AudioLibrary>(m_vfs);
        audio.mixer = &m_audio->mixer();
        audio.library = m_audioLibrary.get();
    } catch (const std::exception& e) {
        logWarning("title: no sound ({})", e.what());
        m_audioLibrary.reset();
        m_audio.reset();
    }
    m_screen = std::make_unique<ui::TitleScreen>(m_vfs, audio, "en");
    m_screen->show();
}

Title::~Title()
{
    // The music stops before its device closes.
    m_screen.reset();
}

bool Title::run()
{
    auto last = std::chrono::steady_clock::now();
    while (m_window.pumpEvents()) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1F);
        last = now;
        const bool start = frame(m_window.input(), dt);
        m_present.present();
        m_window.swapBuffers();
        if (start) {
            return true;
        }
    }
    return false;
}

bool Title::frame(const InputState& controls, float dt)
{
    ui::MenuInput input = m_menuInput.read(controls);
    // Enter stands in for A, as it does in the game's other menus.
    input.accept = input.accept || controls.pressed(Key::Enter);
    switch (m_screen->update(input, dt)) {
    case ui::TitleAction::Start:
        // The game's loading screens stop the title music (CBaseLoading).
        m_screen->hide();
        m_started = true;
        break;
    case ui::TitleAction::Attract:
        m_screen->returnFromAttract();
        break;
    case ui::TitleAction::None:
        break;
    }
    m_present.resize(m_window.width(), m_window.height());
    m_present.bind();
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_screen->isShown()) {
        m_screen->draw(m_present.width(), m_present.height());
    }
    return m_started;
}

} // namespace openhorizon
