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

/// What the host should do after the title screen's update.
enum class TitleAction {
    None,
    /// A or Start was pressed on the PRESS START prompt. The game then plays
    /// PressStartSlideOff (a silent cue), fires START_OUTRO and sends the
    /// front end's request 322, the profile sign-in step; with a profile it
    /// loads the save and carries on into the game. The screen keeps
    /// drawing its outro and ignores input until resume().
    Start,
    /// No button had started the game for 53 seconds: the screen faded to
    /// black over 2 seconds and stopped its music. The game then sends the
    /// front end's request 6 (0x8285c6b4), which was not traced further; it
    /// is skipped when the "noattractvideo" setting is on. Call
    /// returnFromAttract() to bring the screen back.
    Attract,
};

/// CSplash's logic on the PRESS START prompt, apart from its scene and
/// sound: which scene events it fires, when its music starts and stops, and
/// what it asks of the host. Times are in seconds; one update() is one of
/// the game's screen updates.
class TitleFlow {
public:
    /// Updates between entering and FADE_IN (CSplash +616, set to 8 at
    /// 0x82856454), and between coming back from the attract and FADE_IN (5,
    /// 0x828349e4).
    static constexpr int kFadeInDelay = 8;
    static constexpr int kReturnFadeInDelay = 5;
    /// The idle time before the attract (the double at 0x8206cbf8, compared
    /// at 0x8285c8b0) and the wait after FADE_OUT (the float at 0x82069f70,
    /// stored at 0x8285c904 and counted down at 0x8285c620).
    static constexpr double kAttractSeconds = 53.0;
    static constexpr float kFadeOutSeconds = 2.0F;

    /// What one call asks for, in order: the scene events to fire at the
    /// root contract, the UI sounds to play (ui4audio.xml names), then the
    /// music change, then the action.
    struct Step {
        std::vector<std::string_view> events;
        std::vector<std::string_view> sounds;
        bool startMusic = false;
        bool stopMusic = false;
        TitleAction action = TitleAction::None;
    };

    /// Entering the screen without a profile (0x82855fa0, 0x8283d0e0):
    /// CONTROLLER_ONLY, PressStartSlideOn (0x82856418), the music, FADE_IN
    /// due in kFadeInDelay updates, the idle timer from zero.
    Step enter();
    /// `fadeOutComplete` is whether the scene fired FADE_OUT_COMPLETE at the
    /// root contract since the last update.
    Step update(const MenuInput& input, float dt, bool fadeOutComplete);
    /// START_INTRO, when the screen gets input back after Start.
    Step resume();
    /// After TitleAction::Attract (0x828349b0): the music again, FADE_IN
    /// due in kReturnFadeInDelay updates, the idle timer from zero.
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
    /// Between Start and resume(): another screen (the sign-in step) has
    /// the input.
    bool m_waiting = false;
};

/// Where the title's music goes. Without a mixer the screen is silent.
struct TitleAudio {
    audio::Mixer* mixer = nullptr;
    audio::AudioLibrary* library = nullptr;
    audio::BusId bus = audio::kMasterBus;
};

/// The game's title screen, the one that asks to PRESS START: CSplash
/// (ForzaUI::CSplash, vtable 0x8206aeec) on media/ui/scenes/ui4/
/// 120_p_pre_splash, screen 327 of the table built at 0x82740540, in the
/// "PreGame" package of media/ui/respack.xml.
///
/// Verified from the executable and the data:
/// - the background is the scene's CUSTREND_MOVIERENDERER (COMPONENT_VIDEO)
///   playing GAME:\Media\UI\Videos\PressStart.wmv, looping and autoplaying,
///   on the model its MODEL_VIEWPORT names; its .def selects the video
///   stream only (the file's audio track is silent anyway);
/// - TEXT_PRESS_START shows IDS_PressStart from the Main table (0x82855fc0);
/// - on entering (0x82855fa0): the front-end ambience starts with its entry
///   0, the "SplashScreen" sound of media/audio/ui/ui4audio.xml (0x826f1890,
///   table at 0x828e24a8), which plays UI/FrontEnd/FE_Ambience of
///   UIInGame.fev looping, whose only sound is /FrontEnd/PressStart_Music;
///   PressStartSlideOn plays (a silent "Blank" cue); CONTROLLER_ONLY is fired
///   (KINECT_ENABLED when a Kinect is connected, 0x82839088); FADE_IN is
///   fired 8 updates later (0x8285c6d0);
/// - the prompt reacts to input actions 93 (A) and 103 (Start) of front-end
///   input context 10 (0x8285c56c); B is polled and ignored;
/// - Start (0x828564b0): PressStartSlideOff, request 322, START_OUTRO, and
///   START_INTRO when the screen gets input back;
/// - attract (0x8285c89c): once 53 s have passed since the screen came up
///   (the timer restarts while another screen is over it), FADE_OUT, the
///   ambience stops, a 2 s wait that also needs the Fader's
///   FADE_OUT_COMPLETE, then request 6; coming back (0x828349b0) restarts
///   the ambience and fires FADE_IN 5 updates later.
///
/// Left out: the Kinect prompt (no sensor), the E3/kiosk DEMO slide, the
/// install-to-hard-drive panel and the SPLASH_MENU that a signed-in profile
/// gets on this same scene (those belong to the profile flow, out of scope).
///
/// Construct with a current OpenGL 4.5 context.
class TitleScreen {
public:
    /// Throws openhorizon::DataError if the scene or its strings cannot be loaded. A
    /// movie or music that cannot be loaded is logged and left out.
    TitleScreen(const Vfs& vfs, TitleAudio audio = {}, const std::string& language = "en");
    ~TitleScreen();
    TitleScreen(const TitleScreen&) = delete;
    TitleScreen& operator=(const TitleScreen&) = delete;

    /// Shows the screen from the start: black, fading in, the movie from
    /// its first frame, the music playing.
    void show();
    /// Leaves the screen and stops its music, as the game's CSplash does
    /// when it goes away and as its loading screens do.
    void hide();
    bool isShown() const;

    TitleAction update(const MenuInput& input, float dt);
    /// Draws the full screen over the bound framebuffer.
    void draw(int width, int height);

    /// Takes input again after Start, bringing the prompt back
    /// (START_INTRO), as the game does when the sign-in step is left.
    void resume();
    /// Brings the screen back after TitleAction::Attract.
    void returnFromAttract();

    /// The text of the PRESS START prompt.
    std::string pressStartText() const;
    /// Whether the background movie is playing, and its time in seconds.
    bool hasMovie() const;
    double movieTime() const;
    /// Whether the title music is playing.
    bool musicPlaying() const;
    /// The UI sounds the screen has played, in order; empty without sound.
    std::vector<std::string> soundsPlayed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace openhorizon::ui
