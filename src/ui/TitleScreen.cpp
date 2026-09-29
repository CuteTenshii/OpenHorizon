#include "TitleScreen.h"

#include "MovieRenderer.h"
#include "Presentation.h"
#include "UiRenderer.h"
#include "VectorFont.h"

#include "audio/AudioLibrary.h"
#include "audio/EventPlayer.h"
#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <optional>

namespace fh::ui {

namespace {

constexpr const char* kScene = "media/ui/scenes/ui4/120_p_pre_splash";
/// The UI sound table the game's sound names resolve through.
constexpr const char* kUiSounds = "media/audio/ui/ui4audio.xml";
/// The project of the "UIInGame" sound groups, as the executable names it.
constexpr const char* kUiProject = "Game:\\Media\\Audio\\UI\\UIInGame\\UIInGame.fev";
constexpr std::string_view kUiProjectName = "UIInGame/";
/// Entry 0 of the front-end ambience table (0x828e24a8), which CSplash
/// starts on entering.
constexpr const char* kAmbience = "SplashScreen";

/// The event a UI sound name plays: its ui4audio.xml group, without the
/// project name, and cue.
struct UiSound {
    std::string event;
    bool looping = false;
};

std::optional<UiSound> findUiSound(const Vfs& vfs, std::string_view play)
{
    const std::optional<std::string> text = vfs.readText(kUiSounds);
    if (!text) {
        return std::nullopt;
    }
    pugi::xml_document document;
    if (!document.load_buffer(text->data(), text->size())) {
        throw DataError("{}: not XML", kUiSounds);
    }
    for (const pugi::xml_node node : document.child("ui4audio").child("defaults").children("event")) {
        if (play != node.attribute("play").as_string()) {
            continue;
        }
        const std::string_view group = node.attribute("group").as_string();
        if (!group.starts_with(kUiProjectName)) {
            return std::nullopt;
        }
        UiSound sound;
        sound.event = std::string(group.substr(kUiProjectName.size())) + "/" + node.attribute("cue").as_string();
        sound.looping = node.attribute("isLooping").as_bool();
        return sound;
    }
    return std::nullopt;
}

/// The scene's root contract: the behaviour under the scene element that
/// carries TEXT_PRESS_START. Its class name is stored as ")_Splash".
int findRootContract(const Presentation& scene)
{
    for (const int child : scene.children(0)) {
        if (scene.kind(child) == ElementKind::Behavior && scene.contractParameter(child, "TEXT_PRESS_START") >= 0) {
            return child;
        }
    }
    return -1;
}

} // namespace

struct TitleScreen::Impl {
    Impl(const Vfs& vfs, TitleAudio audio, const std::string& language);

    void startMusic();
    void stopMusic();
    TitleAction apply(const TitleFlow::Step& step);

    const Vfs& files;
    TitleAudio audio;
    UiStrings strings;
    FontLibrary fonts;
    Presentation scene;
    UiRenderer renderer;

    int rootContract = -1;
    int pressStart = -1;
    HelpBar help;
    int viewportMaterial = -1;
    std::string movieKey;
    std::unique_ptr<MovieRenderer> movie;
    std::optional<UiSound> ambience;
    std::unique_ptr<audio::EventInstance> music;

    TitleFlow flow;
    bool shown = false;
};

TitleScreen::Impl::Impl(const Vfs& vfs, TitleAudio titleAudio, const std::string& language)
    : files(vfs)
    , audio(titleAudio)
    , strings(vfs, language)
    , fonts(vfs, language)
    , scene(SceneFiles::load(vfs, kScene))
    , renderer(vfs, fonts)
{
    rootContract = findRootContract(scene);
    if (rootContract < 0) {
        throw DataError("{}: no root contract with TEXT_PRESS_START", kScene);
    }
    pressStart = scene.contractParameter(rootContract, "TEXT_PRESS_START");
    help.bind(scene, scene.contractParameter(rootContract, "COMPONENT_HELP_BUTTON_BAR"));

    const int video = scene.contractParameter(rootContract, "COMPONENT_VIDEO");
    const int videoContract = scene.contract(video, "CUSTREND_MOVIERENDERER");
    const int viewport = scene.contractParameter(videoContract, "MODEL_VIEWPORT");
    for (const int child : viewport >= 0 ? scene.children(viewport) : std::vector<int>{}) {
        if (scene.kind(child) == ElementKind::Material) {
            viewportMaterial = child;
            break;
        }
    }
    const BgfFile& bgf = scene.files().bgf;
    std::string videoPath;
    bool loop = false;
    if (videoContract >= 0) {
        for (const Attribute& attribute : bgf.elements[static_cast<std::size_t>(videoContract)].attributes) {
            if (attribute.hash == anarkHash("VIDEOPATH") && attribute.raw < bgf.strings.size()) {
                videoPath = bgf.strings[attribute.raw];
            }
        }
        loop = scene.contractFlag(videoContract, "VIDEOLOOP").value_or(false);
    }
    if (!videoPath.empty() && viewportMaterial >= 0) {
        try {
            movie = std::make_unique<MovieRenderer>(vfs, videoPath, loop);
            movieKey = "movie:" + videoPath;
        } catch (const DataError& e) {
            logError("title screen: {}", e.what());
        }
    } else {
        logError("{}: no movie renderer with a viewport", kScene);
    }

    if (audio.mixer && audio.library) {
        ambience = findUiSound(vfs, kAmbience);
        if (!ambience) {
            logError("{}: no {} sound in the UIInGame project", kUiSounds, kAmbience);
        }
    }
}

void TitleScreen::Impl::startMusic()
{
    // The ambience manager (0x828e3130) leaves a sound that is already
    // playing alone.
    if (!ambience || (music && music->isStarted())) {
        return;
    }
    try {
        const std::shared_ptr<const audio::FevProject> project = audio.library->project(kUiProject);
        const audio::FevEvent* event = project->findEvent(ambience->event);
        if (!event) {
            logError("{}: no event {}", kUiProject, ambience->event);
            ambience.reset();
            return;
        }
        audio::EventSettings settings;
        settings.bus = audio.bus;
        music = std::make_unique<audio::EventInstance>(
            *audio.mixer, *audio.library, kUiProject, project, *event, settings);
        music->start();
    } catch (const DataError& e) {
        logError("title music: {}", e.what());
        ambience.reset();
    }
}

void TitleScreen::Impl::stopMusic()
{
    if (music) {
        music->stop();
    }
}

TitleFlow::Step TitleFlow::enter()
{
    *this = TitleFlow();
    Step step;
    // No Kinect sensor: the prompt asks for the controller only.
    step.events.emplace_back("CONTROLLER_ONLY");
    step.startMusic = true;
    m_fadeInPending = true;
    m_fadeInDelay = kFadeInDelay;
    return step;
}

TitleFlow::Step TitleFlow::update(const MenuInput& input, float dt, bool fadeOutComplete)
{
    Step step;
    m_fadeOutComplete = m_fadeOutComplete || fadeOutComplete;
    m_fadeOutTimer = std::max(0.0F, m_fadeOutTimer - dt);
    if (m_fadeOutComplete && m_fadeOutTimer <= 0.0F) {
        m_fadeOutComplete = false;
        m_inAttract = true;
        step.action = TitleAction::Attract;
    }

    if (m_fadeInDelay > 0) {
        --m_fadeInDelay;
    }
    if (m_fadeInPending && m_fadeInDelay == 0) {
        step.events.emplace_back("FADE_IN");
        m_fadeInPending = false;
    }

    // Another screen over this one keeps the idle timer at zero
    // (0x8285ca78).
    m_idle = m_waiting || m_inAttract ? 0.0 : m_idle + static_cast<double>(dt);
    if (!m_fadingOut && m_idle > kAttractSeconds) {
        m_idle = 0.0;
        m_fadingOut = true;
        step.events.emplace_back("FADE_OUT");
        step.stopMusic = true;
        m_fadeOutTimer = kFadeOutSeconds;
    }

    // The input handler (0x8285c290) does nothing while +629 is set; B is
    // polled after A and Start but its result is not used.
    if (!m_waiting && !m_fadingOut && (input.accept || input.start)) {
        step.events.emplace_back("START_OUTRO");
        m_waiting = true;
        step.action = TitleAction::Start;
    }
    return step;
}

TitleFlow::Step TitleFlow::resume()
{
    Step step;
    if (m_waiting) {
        m_waiting = false;
        step.events.emplace_back("START_INTRO");
    }
    return step;
}

TitleFlow::Step TitleFlow::returnFromAttract()
{
    Step step;
    if (!m_inAttract) {
        return step;
    }
    m_inAttract = false;
    m_fadingOut = false;
    m_fadeInPending = true;
    m_fadeInDelay = kReturnFadeInDelay;
    m_idle = 0.0;
    step.startMusic = true;
    return step;
}

TitleAction TitleScreen::Impl::apply(const TitleFlow::Step& step)
{
    for (const std::string_view event : step.events) {
        scene.fireEvent(rootContract, event);
    }
    if (step.startMusic) {
        startMusic();
    }
    if (step.stopMusic) {
        stopMusic();
    }
    return step.action;
}

TitleScreen::TitleScreen(const Vfs& vfs, TitleAudio audio, const std::string& language)
    : m_impl(std::make_unique<Impl>(vfs, audio, language))
{
}

TitleScreen::~TitleScreen() = default;

void TitleScreen::show()
{
    Impl& impl = *m_impl;
    impl.scene.reset();
    if (impl.pressStart >= 0) {
        impl.scene.setText(impl.pressStart, impl.strings.get("Main", "IDS_PressStart"));
    }
    // CSplash adds no help entries on the prompt, so the bar hides every
    // slot.
    impl.help.set(impl.scene, {});
    if (impl.movie) {
        impl.movie->restart();
    }
    impl.apply(impl.flow.enter());
    impl.scene.takeFiredEvents();
    impl.shown = true;
}

void TitleScreen::hide()
{
    m_impl->stopMusic();
    m_impl->shown = false;
}

bool TitleScreen::isShown() const
{
    return m_impl->shown;
}

TitleAction TitleScreen::update(const MenuInput& input, float dt)
{
    Impl& impl = *m_impl;
    if (!impl.shown) {
        return TitleAction::None;
    }
    impl.scene.update(dt);
    bool fadeOutComplete = false;
    for (const Presentation::FiredEvent& fired : impl.scene.takeFiredEvents()) {
        fadeOutComplete = fadeOutComplete || (fired.target == impl.rootContract && fired.event == "FADE_OUT_COMPLETE");
    }
    if (impl.movie && !impl.flow.inAttract()) {
        impl.movie->update(dt);
    }
    return impl.apply(impl.flow.update(input, dt, fadeOutComplete));
}

void TitleScreen::draw(int width, int height)
{
    Impl& impl = *m_impl;
    if (!impl.shown) {
        return;
    }
    std::vector<DrawItem> items = impl.scene.evaluate();
    if (impl.movie && impl.movie->texture() != 0) {
        impl.renderer.setTextureOverride(impl.movieKey, impl.movie->texture());
        for (DrawItem& item : items) {
            if (item.isText || item.material != impl.viewportMaterial) {
                continue;
            }
            if (item.model.layers.empty()) {
                item.model.layers.emplace_back();
            }
            item.model.layers.front().texture = impl.movieKey;
            item.model.layers.front().imageName = impl.movieKey;
        }
    }
    applyTextScale(items);
    impl.renderer.begin(width, height);
    impl.renderer.draw(items);
    impl.renderer.end();
}

void TitleScreen::resume()
{
    m_impl->apply(m_impl->flow.resume());
}

void TitleScreen::returnFromAttract()
{
    m_impl->apply(m_impl->flow.returnFromAttract());
}

std::string TitleScreen::pressStartText() const
{
    return m_impl->pressStart >= 0 ? m_impl->scene.text(m_impl->pressStart) : std::string();
}

bool TitleScreen::hasMovie() const
{
    return m_impl->movie && m_impl->movie->texture() != 0;
}

double TitleScreen::movieTime() const
{
    return m_impl->movie ? m_impl->movie->time() : 0.0;
}

bool TitleScreen::musicPlaying() const
{
    return m_impl->music && m_impl->music->isStarted();
}

} // namespace fh::ui
