#pragma once

#include "MenuCommon.h"

#include "audio/Mixer.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
}
namespace openhorizon::audio {
class AudioLibrary;
}

namespace openhorizon::ui {

enum class TitleAction {
    None,
    /// A or Start on the prompt; the game then sends request 322 (sign-in).
    /// Input is ignored until resume().
    Start,
    /// 53 s idle, faded out; the game then sends request 6 (0x8285c6b4), unless
    /// "noattractvideo" is set. Call returnFromAttract() to come back.
    Attract,
};

/// CSplash's logic on the PRESS START prompt, without scene and sound. One
/// update() is one of the game's screen updates.
class TitleFlow {
public:
    /// CSplash +616 (0x82856454), and 5 after the attract (0x828349e4).
    static constexpr int kFadeInDelay = 8;
    static constexpr int kReturnFadeInDelay = 5;
    /// The double at 0x8206cbf8 (compared at 0x8285c8b0) and the float at
    /// 0x82069f70 (0x8285c904, counted down at 0x8285c620).
    static constexpr double kAttractSeconds = 53.0;
    static constexpr float kFadeOutSeconds = 2.0F;

    /// In order: root events, UI sounds (ui4audio.xml names), music, action.
    struct Step {
        std::vector<std::string_view> events;
        std::vector<std::string_view> sounds;
        bool startMusic = false;
        bool stopMusic = false;
        TitleAction action = TitleAction::None;
    };

    /// Without a profile (0x82855fa0, 0x8283d0e0; PressStartSlideOn 0x82856418).
    Step enter();
    /// `fadeOutComplete`: the scene fired FADE_OUT_COMPLETE since the last update.
    Step update(const MenuInput& input, float dt, bool fadeOutComplete);
    Step resume();
    /// 0x828349b0.
    Step returnFromAttract();

    bool waitingAfterStart() const { return m_waiting; }
    bool inAttract() const { return m_inAttract; }
    double idleSeconds() const { return m_idle; }

private:
    bool m_fadeInPending = false;
    int m_fadeInDelay = 0;
    /// The timer at CSplash +392.
    double m_idle = 0.0;
    /// CSplash +629: fading out to the attract, input ignored.
    bool m_fadingOut = false;
    /// CSplash +620: the Fader reported FADE_OUT_COMPLETE.
    bool m_fadeOutComplete = false;
    /// CSplash +624.
    float m_fadeOutTimer = 0.0F;
    bool m_inAttract = false;
    /// Another screen (the sign-in step) has the input.
    bool m_waiting = false;
};

/// Silent without a mixer.
struct TitleAudio {
    audio::Mixer* mixer = nullptr;
    audio::AudioLibrary* library = nullptr;
    audio::BusId bus = audio::kMasterBus;
};

/// The PRESS START title screen: CSplash (vtable 0x8206aeec) on
/// media/ui/scenes/ui4/120_p_pre_splash, screen 327 (table 0x82740540),
/// "PreGame" package of media/ui/respack.xml.
///
/// Verified: background PressStart.wmv on COMPONENT_VIDEO, video stream only;
/// IDS_PressStart (0x82855fc0); entering (0x82855fa0) starts "SplashScreen"
/// of ui4audio.xml (0x826f1890, table 0x828e24a8; UIInGame.fev's
/// UI/FrontEnd/FE_Ambience, looping), fires CONTROLLER_ONLY
/// (KINECT_ENABLED, 0x82839088), FADE_IN 8 updates later (0x8285c6d0);
/// actions 93 (A) and 103 (Start) of context 10 (0x8285c56c); Start
/// 0x828564b0; attract 0x8285c89c, its timer restarting under other screens.
///
/// Left out: Kinect prompt, DEMO slide, install panel, signed-in SPLASH_MENU.
///
/// Construct with a current OpenGL 4.5 context.
class TitleScreen {
public:
    /// Throws openhorizon::DataError if the scene or its strings cannot be
    /// loaded; a missing movie or music is logged and left out.
    TitleScreen(const Vfs& vfs, TitleAudio audio = {}, const std::string& language = "en");
    ~TitleScreen();
    TitleScreen(const TitleScreen&) = delete;
    TitleScreen& operator=(const TitleScreen&) = delete;

    /// From black, the movie from its first frame.
    void show();
    /// Stops the music too, as CSplash and the loading screens do.
    void hide();
    bool isShown() const;

    TitleAction update(const MenuInput& input, float dt);
    void draw(int width, int height);

    /// START_INTRO, when the sign-in step is left.
    void resume();
    void returnFromAttract();

    std::string pressStartText() const;
    /// The background movie's time in seconds.
    bool hasMovie() const;
    double movieTime() const;
    bool musicPlaying() const;
    /// In order; empty without sound.
    std::vector<std::string> soundsPlayed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace openhorizon::ui
