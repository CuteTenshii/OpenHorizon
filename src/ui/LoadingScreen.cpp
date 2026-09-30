#include "LoadingScreen.h"

#include "AnarkFormats.h"
#include "Presentation.h"
#include "SuperStacker.h"
#include "UiRenderer.h"
#include "VectorFont.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openhorizon::ui {

namespace {

constexpr const char* kScene = "media/ui/scenes/ui4/2_c_ini_loading";
constexpr const char* kDefinitions = "media/ui/LoadingDefs.xml";
constexpr const char* kBackgroundFolder = "GAME:\\Media\\UI\\Textures\\Horizon\\Loading\\BGLoadingImages\\";

/// The background names the picker indexes (the table at 0x8332c6b8); the
/// festival game draws from its first thirteen.
constexpr std::array<const char*, 13> kBackgrounds{"Image1.tga", "Image2.tga", "Image3.tga", "Image4.tga", "Image5.tga",
    "Image6.tga", "Image7.tga", "Image8.tga", "Image9.tga", "Image10.tga", "Image11.tga", "Image14.tga", "Image16.tga"};

constexpr int kLines = 8;

/// The tick caps its time step (0x82762c30 compares with 0.05).
constexpr float kMaxStep = 0.05F;

/// "Table:IDS_Id" to its text.
std::string resolve(UiStrings& strings, std::string_view reference)
{
    const std::size_t colon = reference.find(':');
    if (colon == std::string_view::npos) {
        return std::string(reference);
    }
    return strings.get(reference.substr(0, colon), reference.substr(colon + 1));
}

} // namespace

LoadingDefinitions LoadingDefinitions::load(const Vfs& vfs, UiStrings& strings)
{
    const Bytes data = vfs.readOrThrow(kDefinitions);
    pugi::xml_document document;
    if (!document.load_buffer(data.data(), data.size())) {
        throw DataError("{}: not XML", kDefinitions);
    }
    LoadingDefinitions definitions;
    for (const pugi::xml_node groupNode : document.child("LoadingDefinitions").children("LoadingGroup")) {
        LoadingTipGroup group;
        group.name = groupNode.attribute("name").as_string();
        group.tipCount = groupNode.attribute("tipcount").as_int(1);
        // The order names map to 0 "sequential" and 1 "random" (0x82745e28);
        // an unknown name is taken as sequential, as that lookup returns 0.
        group.random = std::string_view(groupNode.attribute("order").as_string()) == "random";
        for (const pugi::xml_node tipNode : groupNode.children("Tip")) {
            LoadingTip tip;
            tip.layout = tipNode.attribute("layout").as_int(1);
            tip.displayTime = tipNode.attribute("displaytime").as_float();
            tip.title = resolve(strings, tipNode.attribute("title").as_string());
            tip.message = resolve(strings, tipNode.attribute("message").as_string());
            group.tips.push_back(std::move(tip));
        }
        definitions.groups.push_back(std::move(group));
    }
    return definitions;
}

const LoadingTipGroup* LoadingDefinitions::find(std::string_view name) const
{
    for (const LoadingTipGroup& group : groups) {
        if (group.name == name) {
            return &group;
        }
    }
    return nullptr;
}

struct LoadingScreen::Impl {
    Impl(const Vfs& vfs, const std::string& language, std::uint32_t seed);

    void pickBackground();
    void nextTip();
    void showTip();
    void updateProgress();

    const Vfs& files;
    UiStrings strings;
    FontLibrary fonts;
    Presentation scene;
    UiRenderer renderer;
    LoadingDefinitions definitions;
    std::mt19937 random;
    /// Every SUPER_STACKER of the scene: the title's (its crisp tape, smudge
    /// tape and dirt), the statistic's and each line's (its Pill_Big).
    SuperStackers stackers;

    int rootContract = -1;
    int title = -1;
    int tipLayout = -1;
    int backgroundMaterial = -1;
    int messageText = -1;
    /// The text TEXT_MESSAGE has in the scene.
    std::string sceneMessage;
    std::optional<std::string> status;
    std::array<int, kLines> lines{};
    /// Each line's brush bar, -1 where the scene has none.
    std::array<int, kLines> bars{};
    std::size_t linesUsed = 0;
    float lineWidth = 0.0F;
    TextStyle lineStyle;

    /// Times each background has been picked this session.
    std::array<int, kBackgrounds.size()> backgroundUses{};
    std::string backgroundPath;

    struct GroupState {
        int shown = 0;
        int current = 0;
    };
    std::vector<GroupState> groupStates;
    const LoadingTipGroup* group = nullptr;
    GroupState* groupState = nullptr;
    const LoadingTip* tip = nullptr;
    float timer = 0.0F;
    bool whooshed = false;
    bool shown = false;
    /// Whether the scene has been laid out since it was shown, so its
    /// world transforms are those drawn.
    bool drawn = false;
};

LoadingScreen::Impl::Impl(const Vfs& vfs, const std::string& language, std::uint32_t seed)
    : files(vfs)
    , strings(vfs, language)
    , fonts(vfs, language)
    , scene(SceneFiles::load(vfs, kScene))
    , renderer(vfs, fonts)
    , random(seed)
{
    definitions = LoadingDefinitions::load(vfs, strings);
    groupStates.resize(definitions.groups.size());
    rootContract = scene.contract(0, "UIContract");
    if (rootContract < 0) {
        throw DataError("{}: no UIContract", kScene);
    }
    title = scene.contractParameter(rootContract, "TEXT_TIP_TITLE");
    tipLayout = scene.contractParameter(rootContract, "COMPONENT_TIP_INFO");
    backgroundMaterial = scene.contractParameter(rootContract, "MATERIAL_BACKGROUND");
    messageText = scene.contractParameter(rootContract, "TEXT_MESSAGE");
    if (messageText >= 0) {
        sceneMessage = scene.text(messageText);
    }
    for (int i = 0; i < kLines; ++i) {
        const auto index = static_cast<std::size_t>(i);
        lines[index] = scene.contractParameter(rootContract, std::format("TEXT_LINE_{}", i + 1));
        bars[index] = -1;
        if (lines[index] >= 0) {
            const std::vector<int> pills = scene.childrenNamed(scene.parent(lines[index]), "Pill_Big");
            if (!pills.empty()) {
                bars[index] = pills.front();
            }
        }
    }
    lineWidth = textBoxWidth(scene, lines[0]);
    lineStyle = textStyle(scene, lines[0]);
    stackers.bind(scene, renderer);
}

void LoadingScreen::Impl::pickBackground()
{
    // 0x827613e8: among the backgrounds used least so far, one at random;
    // its count grows by one, or by two when it was the only one.
    int least = std::numeric_limits<int>::max();
    int tied = 0;
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < kBackgrounds.size(); ++i) {
        if (!files.exists(texturePathForScene(std::string(kBackgroundFolder) + kBackgrounds[i]))) {
            continue;
        }
        if (backgroundUses[i] < least) {
            least = backgroundUses[i];
            candidates.clear();
        }
        if (backgroundUses[i] == least) {
            candidates.push_back(i);
        }
    }
    tied = static_cast<int>(candidates.size());
    if (tied == 0) {
        backgroundPath.clear();
        return;
    }
    const std::size_t chosen = candidates[random() % candidates.size()];
    backgroundUses[chosen] += tied == 1 ? 2 : 1;
    backgroundPath = texturePathForScene(std::string(kBackgroundFolder) + kBackgrounds[chosen]);
}

void LoadingScreen::Impl::nextTip()
{
    // 0x827479c0.
    const int count = static_cast<int>(group->tips.size());
    ++groupState->shown;
    if (count == 0) {
        tip = nullptr;
        return;
    }
    if (group->random) {
        if (count > 1) {
            int index = groupState->current;
            while (index == groupState->current) {
                index = static_cast<int>(random() % static_cast<std::uint32_t>(count));
            }
            groupState->current = index;
        } else {
            groupState->current = 0;
        }
    } else {
        groupState->current = (groupState->current + 1) % count;
    }
    tip = &group->tips[static_cast<std::size_t>(groupState->current)];
}

void LoadingScreen::Impl::showTip()
{
    timer = 0.0F;
    whooshed = false;
    if (!tip) {
        return;
    }
    scene.goToSlideIndex(tipLayout, tip->layout);
    if (title >= 0) {
        scene.setText(title, tip->title);
    }
    // Each line takes what fits its box, at the size the text is drawn.
    const std::string wrapped = wrapText(tip->message, lineWidth, [&](const std::string& text) {
        return renderer.measure(lineStyle.font, text, lineStyle.size * kTextPointsToUnits);
    });
    std::vector<std::string> split;
    std::size_t start = 0;
    while (start <= wrapped.size()) {
        const std::size_t end = std::min(wrapped.find('\n', start), wrapped.size());
        split.push_back(wrapped.substr(start, end - start));
        start = end + 1;
    }
    linesUsed = std::min(split.size(), lines.size());
    for (int i = 0; i < kLines; ++i) {
        const bool used = static_cast<std::size_t>(i) < split.size();
        if (used && lines[static_cast<std::size_t>(i)] >= 0) {
            scene.setText(lines[static_cast<std::size_t>(i)], split[static_cast<std::size_t>(i)]);
        }
        scene.fireEvent(rootContract, used ? std::format("SHOW_LINE_{}", i + 1) : std::format("HIDE_LINE_{}", i + 1));
    }
    updateProgress();
}

void LoadingScreen::Impl::updateProgress()
{
    if (!tip) {
        return;
    }
    const float fade = tip->displayTime * 0.25F;
    float progress = 0.5F;
    if (timer < fade && fade > 0.0F) {
        const float remaining = 1.0F - timer / fade;
        progress = 0.5F - remaining * remaining * 0.5F;
    }
    if (timer > tip->displayTime - fade && fade > 0.0F) {
        if (!whooshed) {
            scene.fireEvent(rootContract, "UserDataWhooshOff");
            whooshed = true;
        }
        const float t = (timer - (tip->displayTime - fade)) / fade;
        progress = (t * t + 1.0F) * 0.5F;
    }
    scene.goToProgress(tipLayout, progress);
}

LoadingScreen::LoadingScreen(const Vfs& vfs, const std::string& language, std::uint32_t seed)
    : m_impl(std::make_unique<Impl>(vfs, language, seed))
{
}

LoadingScreen::~LoadingScreen() = default;

void LoadingScreen::show(std::string_view groupName)
{
    Impl& impl = *m_impl;
    impl.scene.reset();
    impl.drawn = false;
    impl.linesUsed = 0;
    if (impl.messageText >= 0 && impl.status) {
        impl.scene.setText(impl.messageText, *impl.status);
    }
    impl.scene.fireEvent(impl.rootContract, "SHOW");
    impl.pickBackground();
    impl.group = impl.definitions.find(groupName);
    impl.groupState = nullptr;
    impl.tip = nullptr;
    if (impl.group) {
        impl.groupState = &impl.groupStates[static_cast<std::size_t>(impl.group - impl.definitions.groups.data())];
        impl.groupState->shown = 0;
        impl.nextTip();
    }
    if (!impl.tip) {
        impl.scene.setHidden(impl.tipLayout, true);
    }
    impl.showTip();
    impl.shown = true;
}

void LoadingScreen::hide()
{
    m_impl->shown = false;
}

bool LoadingScreen::isShown() const
{
    return m_impl->shown;
}

void LoadingScreen::update(float dt)
{
    Impl& impl = *m_impl;
    if (!impl.shown) {
        return;
    }
    const float step = std::min(dt, kMaxStep);
    impl.timer += step;
    if (impl.tip && impl.timer >= impl.tip->displayTime) {
        if (impl.groupState->shown < impl.group->tipCount) {
            impl.nextTip();
            impl.showTip();
        } else {
            // The game moves on to the "stats" group here; with no
            // statistics the last tip stays, held at its full display.
            impl.timer = impl.tip->displayTime * 0.5F;
        }
    }
    impl.updateProgress();
    impl.scene.update(step);
    impl.scene.takeFiredEvents();
}

void LoadingScreen::draw(int width, int height)
{
    Impl& impl = *m_impl;
    if (!impl.shown) {
        return;
    }
    impl.stackers.layout(impl.scene, impl.renderer);
    std::vector<DrawItem> items = impl.scene.evaluate();
    impl.drawn = true;
    // The background image goes into the background material's texture
    // (MATERIAL_BACKGROUND).
    for (DrawItem& item : items) {
        if (item.isText || item.material != impl.backgroundMaterial || impl.backgroundPath.empty()) {
            continue;
        }
        if (item.model.layers.empty()) {
            item.model.layers.emplace_back();
        }
        item.model.layers.front().texture = impl.backgroundPath;
        item.model.layers.front().imageName = impl.backgroundPath;
    }
    applyTextScale(items);
    impl.renderer.begin(width, height);
    impl.renderer.draw(items);
    impl.renderer.end();
}

void LoadingScreen::setStatus(std::optional<std::string> status)
{
    Impl& impl = *m_impl;
    impl.status = std::move(status);
    if (impl.messageText >= 0) {
        impl.scene.setText(impl.messageText, impl.status ? *impl.status : impl.sceneMessage);
    }
}

std::string LoadingScreen::message() const
{
    const Impl& impl = *m_impl;
    return impl.messageText >= 0 ? impl.scene.text(impl.messageText) : std::string();
}

const LoadingTip* LoadingScreen::tip() const
{
    return m_impl->tip;
}

const std::string& LoadingScreen::background() const
{
    return m_impl->backgroundPath;
}

std::vector<LoadingTipLine> LoadingScreen::tipLines() const
{
    const Impl& impl = *m_impl;
    std::vector<LoadingTipLine> out;
    if (!impl.drawn || !impl.tip) {
        return out;
    }
    std::vector<DrawItem> items = impl.scene.evaluate();
    applyTextScale(items);
    for (std::size_t i = 0; i < impl.linesUsed; ++i) {
        const auto text = std::ranges::find_if(
            items, [&](const DrawItem& item) { return item.isText && item.element == impl.lines[i]; });
        const auto bar = std::ranges::find_if(
            items, [&](const DrawItem& item) { return !item.isText && item.element == impl.bars[i]; });
        if (text == items.end() || bar == items.end()) {
            continue;
        }
        out.push_back({text->text, bar->model});
    }
    return out;
}

} // namespace openhorizon::ui
