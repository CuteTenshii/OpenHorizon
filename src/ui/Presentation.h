#pragma once

#include "AnarkFormats.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fh::ui {

/// Attribute names the runtime evaluates, as hashes.
namespace attr {
extern const std::uint32_t PositionX, PositionY, PositionZ;
extern const std::uint32_t RotationX, RotationY, RotationZ;
extern const std::uint32_t ScaleX, ScaleY, ScaleZ;
extern const std::uint32_t PivotX, PivotY, PivotZ;
extern const std::uint32_t Opacity;
extern const std::uint32_t DiffuseR, DiffuseG, DiffuseB, DiffuseA;
extern const std::uint32_t TextColorR, TextColorG, TextColorB, TextColorA;
extern const std::uint32_t Size, Tracking, Leading, HorzAlign, VertAlign;
extern const std::uint32_t TextString, Font;
extern const std::uint32_t RotationUv, PositionU, PositionV, ScaleU, ScaleV, PivotU, PivotV;
} // namespace attr

/// A model ready to draw: its mesh in layer space (1280x720 units, origin
/// at the centre, y up), colour and texture layers.
struct DrawModel {
    const Mesh* mesh = nullptr;
    glm::mat4 world{1.0F};
    glm::vec4 color{1.0F};
    BlendFactor sourceBlend = BlendFactor::SourceAlpha;
    BlendFactor destinationBlend = BlendFactor::InverseSourceAlpha;
    struct Layer {
        /// Normalized game path of the .xds file, or a name the host
        /// substitutes (such as the minimap's render target).
        std::string texture;
        std::string imageName;
        glm::vec3 uvRow0{1.0F, 0.0F, 0.0F};
        glm::vec3 uvRow1{0.0F, -1.0F, 1.0F};
        TextureWrap wrapU = TextureWrap::Clamp;
        TextureWrap wrapV = TextureWrap::Clamp;
    };
    std::vector<Layer> layers;
};

struct DrawText {
    glm::mat4 world{1.0F};
    std::string text;
    std::string font;
    float size = 12.0F;
    float tracking = 0.0F;
    float leading = 0.0F;
    int horizontalAlign = 0;
    int verticalAlign = 1;
    glm::vec4 color{1.0F};
};

struct DrawItem {
    int layer = 0;
    float depth = 0.0F;
    int order = 0;
    bool isText = false;
    /// For models, the element index of the material they are drawn with.
    int material = -1;
    DrawModel model;
    DrawText text;
};

/// A running instance of one Anark scene: which slide each component shows,
/// each component's timeline, and values the host sets (text, rotations).
/// Evaluating it gives the draw list; it holds no GPU state.
///
/// Evaluation follows the presentation's model: an element takes its
/// exported transform and style, then the presentation's master values,
/// then those of the master slide and the current slide of every component
/// that lists it, then the animation tracks of those slides at the
/// component's time, then host overrides. An element is present when every
/// component that lists it lists it in its master slide or current slide.
/// Opacity multiplies down the tree; transforms compose as
/// parent * translate(position) * rotate(z, y, x) * translate(-pivot) * scale.
///
/// What is verified against the executable: the timeline rules (the slide
/// flags as the slide entry at 0x82fec688 packs them and the tick at
/// 0x82febdb0 plays them; COMMAND_GOTONEXTSLIDE at the end of a flagged
/// slide, 0x82feeab0), the event handlers and their four commands (logic
/// loader 0x82fe7ee0), GoToProgress and AnimateProgress (0x82f36650,
/// 0x82f36e08). What is inferred from the data: that a component starts on
/// its first slide after the master; that Anark's rotations about x and y
/// turn the other way in these right-handed matrices (the redline arc, a
/// half disc turned about x, only lines up with the dial that way); and
/// the draw order, layers from the last in the file to the first, then far
/// to near (the renderer's sort was not traced).
class Presentation {
public:
    explicit Presentation(SceneFiles files);

    const SceneFiles& files() const { return m_files; }
    std::size_t elementCount() const { return m_nodes.size(); }

    /// The element at a path of names from the scene root ("Layer/HUD/
    /// GAUGE_ANALOG"), each step searching the previous one's descendants
    /// depth-first. -1 if not found.
    int find(std::string_view path) const;
    /// The first descendant of `element` with this name, depth-first.
    int findBelow(int element, std::string_view name) const;
    /// Direct children with this name, in order.
    std::vector<int> childrenNamed(int element, std::string_view name) const;
    const std::vector<int>& children(int element) const { return m_nodes[static_cast<std::size_t>(element)].children; }
    const std::string& name(int element) const { return m_nodes[static_cast<std::size_t>(element)].name; }
    ElementKind kind(int element) const { return m_files.bgf.elements[static_cast<std::size_t>(element)].kind; }
    int parent(int element) const { return m_nodes[static_cast<std::size_t>(element)].parent; }

    /// Slide names of a component, the master slide first.
    std::vector<std::string> slideNames(int component) const;
    /// Shows a slide by name (not the master) and restarts its timeline.
    /// Returns false if the component has no such slide.
    bool goToSlide(int component, std::string_view slide);
    std::string currentSlide(int component) const;
    /// Shows a slide by its index, the master slide counting as 0 (the
    /// game's slide-by-index call, 0x82f36588). Returns false when out of
    /// range.
    bool goToSlideIndex(int component, int index);
    /// Puts a component's timeline at `ms` (COMMAND_GOTOTIME); a paused
    /// timeline stays there, a playing one carries on from there.
    void seek(int component, float ms);
    /// Starts a paused timeline (COMMAND_PLAY).
    void play(int component);
    float time(int component) const;
    float duration(int component) const;
    /// The game's GoToProgress (0x82f36650): time = (int)(duration *
    /// fraction + 0.5) milliseconds, fraction not clamped.
    void goToProgress(int component, float fraction);
    /// time / duration.
    float progress(int component) const;
    /// The game's AnimateProgress (0x82f367a0, ticked at 0x82f36e08): moves
    /// the progress from its current value to `target` over `seconds`,
    /// easing by `curve`: 1 linear, 2 sin(t pi/2), 3 1 - cos(t pi/2),
    /// 4 (1 - cos(t pi)) / 2; any other curve shows progress 0 until the end.
    void animateProgress(int component, float target, float seconds, int curve);

    /// Fires an event at a logic owner (normally a contract behaviour
    /// element) and runs its handler's actions: slide changes by name,
    /// attribute sets, time jumps and play. Returns false when the owner has
    /// no handler for the event. Other commands are ignored.
    bool fireEvent(int owner, std::string_view event);
    /// The behaviour element below `element` whose _AKContractClass is
    /// `className`, or -1.
    int contract(int element, std::string_view className) const;
    /// The element a contract parameter names: its path ("parent.A.B",
    /// "this") is walked from the contract element, "parent" stepping up and
    /// each name stepping to a descendant, direct children first. -1 when
    /// the parameter or an element on the path is missing.
    int contractParameter(int contractElement, std::string_view parameter) const;
    /// The value of a contract's numeric parameter (types 17 and 19).
    std::optional<float> contractNumber(int contractElement, std::string_view parameter) const;
    /// The element a path such as "parent.parent.HUD_RADIO_CONTRACT" names,
    /// walked from `from` as contract paths are. -1 if missing.
    int resolvePath(int from, std::string_view path) const;

    /// An event an event behaviour fired at an element.
    struct FiredEvent {
        int target = -1;
        std::string event;
    };
    /// The events the scene's event behaviours (the "eventfire" script in
    /// media/ui/behaviors/ui4, which fires its eventName at its TARGET when
    /// it activates) fired since the last call, in order. They also run the
    /// target's logic handlers; the host handles the rest, as the game's
    /// controls do. A behaviour activates when its slide's timeline reaches
    /// its timeoffset (inferred from Anark's time ranges: the radio panel's
    /// START_HIDE at 2.3 s and HIDE_COMPLETE at 2.7 s of a 3 s slide).
    std::vector<FiredEvent> takeFiredEvents();

    void setText(int element, std::string text);
    void setAttribute(int element, std::uint32_t hash, float value);
    void clearAttribute(int element, std::uint32_t hash);
    /// Hides an element and everything below it regardless of slides.
    void setHidden(int element, bool hidden);

    /// Puts the scene back as it loads: every component on its first slide,
    /// no host values, nothing hidden. The game builds a new HUD for each
    /// session; this does the same without reloading.
    void reset();

    /// Advances the timelines of playing components.
    void update(float dtSeconds);

    /// Draw items of every present element, in draw order: layers from the
    /// last in the file to the first, then far to near, then tree order.
    std::vector<DrawItem> evaluate() const;

    /// The value of the first animation track of `element` for `hash`, at
    /// `ms`: what the element shows when its component's timeline is there.
    std::optional<float> trackValue(int element, std::uint32_t hash, float ms) const;
    /// The value an attribute has now, after slides, tracks and overrides.
    std::optional<float> attribute(int element, std::uint32_t hash) const;
    /// The text shown: the host's, else the current slide's, else the
    /// presentation's.
    std::string text(int element) const;
    /// The element's world transform in layer space as last evaluated.
    glm::mat4 worldTransform(int element) const;
    bool present(int element) const;

private:
    /// Values of the attributes the runtime renders from, in fixed slots so
    /// a frame's state copies cheaply. Other attributes (timing, behaviour
    /// parameters) do not change what is drawn and are not kept.
    class Values {
    public:
        static constexpr int kSlots = 35;

        const float* find(std::uint32_t hash) const
        {
            const int slot = slotOf(hash);
            return slot >= 0 && (m_set & (std::uint64_t{1} << slot)) ? &m_values[static_cast<std::size_t>(slot)]
                                                                     : nullptr;
        }
        float get(std::uint32_t hash, float fallback) const
        {
            const float* value = find(hash);
            return value ? *value : fallback;
        }
        void set(std::uint32_t hash, float value)
        {
            const int slot = slotOf(hash);
            if (slot >= 0) {
                m_values[static_cast<std::size_t>(slot)] = value;
                m_set |= std::uint64_t{1} << slot;
            }
        }
        void erase(std::uint32_t hash)
        {
            const int slot = slotOf(hash);
            if (slot >= 0) {
                m_set &= ~(std::uint64_t{1} << slot);
            }
        }
        /// Copies every value `other` holds over this one's.
        void apply(const Values& other)
        {
            for (int slot = 0; slot < kSlots; ++slot) {
                if (other.m_set & (std::uint64_t{1} << slot)) {
                    m_values[static_cast<std::size_t>(slot)] = other.m_values[static_cast<std::size_t>(slot)];
                }
            }
            m_set |= other.m_set;
        }
        bool empty() const { return m_set == 0; }

        /// The slot of a rendered attribute, or -1.
        static int slotOf(std::uint32_t hash);

    private:
        std::array<float, kSlots> m_values{};
        std::uint64_t m_set = 0;
    };

    struct Node {
        std::string name;
        int parent = -1;
        std::vector<int> children;
        const FbfRecord* record = nullptr;
        const BsgNode* sceneNode = nullptr;
        /// Base values: exported transform and style, then master values.
        Values base;
        std::string baseText;
        std::string font;
        /// Components whose slides list this element.
        std::vector<int> listedBy;
        Values overrides;
        std::optional<std::string> textOverride;
        bool hidden = false;
    };
    struct EventBehaviour {
        int element = -1;
        float timeMs = 0.0F;
        std::string event;
        std::string target;
    };
    struct Timeline {
        /// Indices into BgfFile::slides, the master slide first.
        std::vector<int> slides;
        /// Per slide, the elements it lists, sorted and without repeats.
        std::vector<std::vector<std::uint32_t>> listed;
        int current = 1;
        /// Milliseconds played since the slide was entered or the last seek.
        float elapsedMs = 0.0F;
        /// What the tracks are evaluated at, derived from elapsedMs by the
        /// slide's play count and ping-pong flag.
        float timeMs = 0.0F;
        bool paused = false;
        /// The play count ran out; the timeline holds.
        bool stopped = false;
        struct ProgressAnimation {
            float from = 0.0F;
            float to = 0.0F;
            float seconds = 0.0F;
            float elapsed = 0.0F;
            int curve = 1;
        };
        std::optional<ProgressAnimation> animation;
        /// Per slide, the event behaviours active in it.
        std::vector<std::vector<EventBehaviour>> eventBehaviours;
    };
    struct State {
        std::vector<Values> values;
        /// Text and font set by the slides shown, when they set one.
        std::unordered_map<std::uint32_t, std::string> texts;
        std::unordered_map<std::uint32_t, std::string> fonts;
        std::vector<char> present;
    };

    void buildBaseValues(int index);
    void enterSlide(Timeline& timeline, int slideIndex);
    /// Recomputes timeMs from elapsedMs as the runtime's tick does.
    void resolveTime(Timeline& timeline);
    const Slide& slideOf(const Timeline& timeline) const;
    std::vector<EventBehaviour> eventBehavioursOf(const Slide& slide) const;
    void fireEventBehaviours(Timeline& timeline, float fromMs, float toMs);
    /// String value of a behaviour element's attribute, or nullptr.
    const std::string* stringAttribute(int element, std::uint32_t hash) const;
    State computeState() const;
    int layerOf(int element) const;

    SceneFiles m_files;
    std::vector<Node> m_nodes;
    std::unordered_map<int, Timeline> m_timelines;
    /// Components in tree order, outermost first.
    std::vector<int> m_componentOrder;
    std::vector<int> m_layers;
    /// Logic owners by element index.
    std::unordered_map<int, const LogicOwner*> m_logic;
    std::vector<FiredEvent> m_firedEvents;
    mutable std::vector<glm::mat4> m_world;
    mutable std::vector<char> m_present;
};

} // namespace fh::ui
