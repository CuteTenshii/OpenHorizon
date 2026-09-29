#include "Presentation.h"

#include "base/Error.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <numbers>
#include <utility>

namespace fh::ui {

namespace attr {
const std::uint32_t PositionX = anarkHash("position.x");
const std::uint32_t PositionY = anarkHash("position.y");
const std::uint32_t PositionZ = anarkHash("position.z");
const std::uint32_t RotationX = anarkHash("rotation.x");
const std::uint32_t RotationY = anarkHash("rotation.y");
const std::uint32_t RotationZ = anarkHash("rotation.z");
const std::uint32_t ScaleX = anarkHash("scale.x");
const std::uint32_t ScaleY = anarkHash("scale.y");
const std::uint32_t ScaleZ = anarkHash("scale.z");
const std::uint32_t PivotX = anarkHash("pivot.x");
const std::uint32_t PivotY = anarkHash("pivot.y");
const std::uint32_t PivotZ = anarkHash("pivot.z");
const std::uint32_t Opacity = anarkHash("opacity");
const std::uint32_t DiffuseR = anarkHash("diffuse.r");
const std::uint32_t DiffuseG = anarkHash("diffuse.g");
const std::uint32_t DiffuseB = anarkHash("diffuse.b");
const std::uint32_t DiffuseA = anarkHash("diffuse.a");
const std::uint32_t TextColorR = anarkHash("textcolor.r");
const std::uint32_t TextColorG = anarkHash("textcolor.g");
const std::uint32_t TextColorB = anarkHash("textcolor.b");
const std::uint32_t TextColorA = anarkHash("textcolor.a");
const std::uint32_t Size = anarkHash("size");
const std::uint32_t Tracking = anarkHash("tracking");
const std::uint32_t Leading = anarkHash("leading");
const std::uint32_t HorzAlign = anarkHash("horzalign");
const std::uint32_t VertAlign = anarkHash("vertalign");
const std::uint32_t TextString = anarkHash("textstring");
const std::uint32_t Font = anarkHash("font");
const std::uint32_t RotationUv = anarkHash("rotationuv");
const std::uint32_t PositionU = anarkHash("positionu");
const std::uint32_t PositionV = anarkHash("positionv");
const std::uint32_t ScaleU = anarkHash("scaleu");
const std::uint32_t ScaleV = anarkHash("scalev");
const std::uint32_t PivotU = anarkHash("pivotu");
const std::uint32_t PivotV = anarkHash("pivotv");
} // namespace attr

namespace {

const std::uint32_t kContractClass = anarkHash("_AKContractClass");

bool isStringAttribute(const Attribute& attribute)
{
    return attribute.type == static_cast<std::uint8_t>(AttributeType::String);
}

} // namespace

int Presentation::Values::slotOf(std::uint32_t hash)
{
    // A perfect hash over the rendered attributes: the first multiplier that
    // sends each of them to its own bucket of 256.
    struct Table {
        std::uint32_t multiplier = 0;
        std::array<std::uint32_t, 256> hashes{};
        std::array<std::int8_t, 256> slots{};
    };
    static const Table table = [] {
        const std::array<std::uint32_t, kSlots> rendered{attr::PositionX, attr::PositionY, attr::PositionZ,
            attr::RotationX, attr::RotationY, attr::RotationZ, attr::ScaleX, attr::ScaleY, attr::ScaleZ, attr::PivotX,
            attr::PivotY, attr::PivotZ, attr::Opacity, attr::DiffuseR, attr::DiffuseG, attr::DiffuseB, attr::DiffuseA,
            attr::TextColorR, attr::TextColorG, attr::TextColorB, attr::TextColorA, attr::Size, attr::Tracking,
            attr::Leading, attr::HorzAlign, attr::VertAlign, attr::RotationUv, attr::PositionU, attr::PositionV,
            attr::ScaleU, attr::ScaleV, attr::PivotU, attr::PivotV, attr::TextString, attr::Font};
        for (std::uint32_t multiplier = 0x9E3779B1U;; multiplier += 2) {
            Table candidate;
            candidate.multiplier = multiplier;
            candidate.slots.fill(-1);
            bool clash = false;
            for (std::size_t i = 0; i < rendered.size() && !clash; ++i) {
                const std::uint32_t bucket = (rendered[i] * multiplier) >> 24;
                clash = candidate.slots[bucket] >= 0;
                candidate.hashes[bucket] = rendered[i];
                candidate.slots[bucket] = static_cast<std::int8_t>(i);
            }
            if (!clash) {
                return candidate;
            }
        }
    }();
    const std::uint32_t bucket = (hash * table.multiplier) >> 24;
    return table.hashes[bucket] == hash ? table.slots[bucket] : -1;
}

Presentation::Presentation(SceneFiles files)
    : m_files(std::move(files))
{
    const BgfFile& bgf = m_files.bgf;
    std::unordered_map<std::uint32_t, const FbfRecord*> records;
    for (const FbfRecord& record : m_files.fbf.records) {
        records.emplace(record.id, &record);
    }
    std::unordered_map<std::uint32_t, const BsgNode*> sceneNodes;
    for (const BsgNode& node : m_files.bsg.nodes) {
        sceneNodes.emplace(node.id, &node);
    }

    m_nodes.resize(bgf.elements.size());
    for (std::size_t i = 0; i < bgf.elements.size(); ++i) {
        const BgfElement& element = bgf.elements[i];
        Node& node = m_nodes[i];
        node.parent = element.parent;
        if (const auto it = records.find(element.id); it != records.end() && element.kind != ElementKind::Behavior) {
            node.record = it->second;
            node.name = it->second->name;
        }
        if (const auto it = sceneNodes.find(element.id);
            it != sceneNodes.end() && element.kind != ElementKind::Behavior) {
            node.sceneNode = it->second;
        }
        if (element.parent >= 0) {
            m_nodes[static_cast<std::size_t>(element.parent)].children.push_back(static_cast<int>(i));
        }
    }
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        buildBaseValues(static_cast<int>(i));
    }

    for (std::size_t s = 0; s < bgf.slides.size(); ++s) {
        const Slide& slide = bgf.slides[s];
        Timeline& timeline = m_timelines[static_cast<int>(slide.owner)];
        timeline.slides.push_back(static_cast<int>(s));
        for (const SlideEntry& entry : slide.entries) {
            std::vector<int>& listedBy = m_nodes[entry.element].listedBy;
            if (std::find(listedBy.begin(), listedBy.end(), static_cast<int>(slide.owner)) == listedBy.end()) {
                listedBy.push_back(static_cast<int>(slide.owner));
            }
        }
    }
    for (auto& [owner, timeline] : m_timelines) {
        // The master slide comes first in the files; keep it first even if
        // a presentation lists it later.
        std::stable_partition(timeline.slides.begin(), timeline.slides.end(),
            [&](int s) { return bgf.slides[static_cast<std::size_t>(s)].name == "Master Slide"; });
        for (const int s : timeline.slides) {
            std::vector<std::uint32_t> elements;
            for (const SlideEntry& entry : bgf.slides[static_cast<std::size_t>(s)].entries) {
                elements.push_back(entry.element);
            }
            std::sort(elements.begin(), elements.end());
            elements.erase(std::unique(elements.begin(), elements.end()), elements.end());
            timeline.listed.push_back(std::move(elements));
        }
        for (const int s : timeline.slides) {
            timeline.eventBehaviours.push_back(eventBehavioursOf(bgf.slides[static_cast<std::size_t>(s)]));
        }
        enterSlide(timeline, timeline.slides.size() > 1 ? 1 : 0);
    }

    const std::function<void(int)> order = [&](int element) {
        if (m_timelines.contains(element)) {
            m_componentOrder.push_back(element);
        }
        const Node& node = m_nodes[static_cast<std::size_t>(element)];
        if (node.record && node.record->kind == FbfKind::Layer) {
            m_layers.push_back(element);
        }
        for (const int child : node.children) {
            order(child);
        }
    };
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].parent < 0) {
            order(static_cast<int>(i));
        }
    }
    for (const LogicOwner& owner : bgf.logic) {
        m_logic.emplace(static_cast<int>(owner.element), &owner);
    }
    m_world.assign(m_nodes.size(), glm::mat4(1.0F));
    m_present.assign(m_nodes.size(), 0);
}

void Presentation::buildBaseValues(int index)
{
    const BgfElement& element = m_files.bgf.elements[static_cast<std::size_t>(index)];
    Node& node = m_nodes[static_cast<std::size_t>(index)];
    auto& base = node.base;
    if (const BsgNode* scene = node.sceneNode) {
        base.set(attr::PositionX, scene->position.x);
        base.set(attr::PositionY, scene->position.y);
        base.set(attr::PositionZ, scene->position.z);
        base.set(attr::RotationX, scene->rotation.x);
        base.set(attr::RotationY, scene->rotation.y);
        base.set(attr::RotationZ, scene->rotation.z);
        base.set(attr::ScaleX, scene->scale.x);
        base.set(attr::ScaleY, scene->scale.y);
        base.set(attr::ScaleZ, scene->scale.z);
        base.set(attr::Opacity, scene->opacity * 100.0F);
    }
    if (const FbfRecord* record = node.record) {
        switch (record->kind) {
        case FbfKind::Material:
            base.set(attr::DiffuseR, record->material.diffuse.r * 255.0F);
            base.set(attr::DiffuseG, record->material.diffuse.g * 255.0F);
            base.set(attr::DiffuseB, record->material.diffuse.b * 255.0F);
            base.set(attr::DiffuseA, record->material.diffuse.a * 255.0F);
            break;
        case FbfKind::Image:
            base.set(attr::RotationUv, record->image.uv.rotationDegrees);
            base.set(attr::PositionU, record->image.uv.position.x);
            base.set(attr::PositionV, record->image.uv.position.y);
            base.set(attr::ScaleU, record->image.uv.scale.x);
            base.set(attr::ScaleV, record->image.uv.scale.y);
            base.set(attr::PivotU, record->image.uv.pivot.x);
            base.set(attr::PivotV, record->image.uv.pivot.y);
            break;
        case FbfKind::Text:
            base.set(attr::Size, static_cast<float>(record->text.size));
            base.set(attr::HorzAlign, static_cast<float>(record->text.horizontalAlign));
            base.set(attr::VertAlign, static_cast<float>(record->text.verticalAlign));
            base.set(attr::Tracking, record->text.tracking);
            base.set(attr::Leading, record->text.leading);
            base.set(attr::TextColorR, 255.0F);
            base.set(attr::TextColorG, 255.0F);
            base.set(attr::TextColorB, 255.0F);
            base.set(attr::TextColorA, 255.0F);
            node.font = record->text.font;
            node.baseText = record->text.text;
            break;
        default:
            break;
        }
    }

    bool hasPosition = false;
    bool hasPivot = false;
    for (const Attribute& attribute : element.attributes) {
        if (isStringAttribute(attribute)) {
            const std::string& value
                = attribute.raw < m_files.bgf.strings.size() ? m_files.bgf.strings[attribute.raw] : std::string();
            if (attribute.hash == attr::TextString) {
                node.baseText = value;
            } else if (attribute.hash == attr::Font) {
                node.font = value;
            }
            continue;
        }
        if (attribute.type > 5) {
            continue;
        }
        base.set(attribute.hash, attribute.asFloat());
        hasPosition = hasPosition || attribute.hash == attr::PositionX || attribute.hash == attr::PositionY;
        hasPivot = hasPivot || attribute.hash == attr::PivotX || attribute.hash == attr::PivotY;
    }
    // The scene graph bakes the pivot into the translation; with only a
    // pivot from the presentation, undo that so the two combine again.
    if (hasPivot && !hasPosition && node.sceneNode) {
        base.set(attr::PositionX, node.sceneNode->position.x + base.get(attr::PivotX, 0.0F));
        base.set(attr::PositionY, node.sceneNode->position.y + base.get(attr::PivotY, 0.0F));
        base.set(attr::PositionZ, node.sceneNode->position.z + base.get(attr::PivotZ, 0.0F));
    }
}

const Slide& Presentation::slideOf(const Timeline& timeline) const
{
    return m_files.bgf.slides[static_cast<std::size_t>(timeline.slides[static_cast<std::size_t>(timeline.current)])];
}

std::vector<Presentation::EventBehaviour> Presentation::eventBehavioursOf(const Slide& slide) const
{
    const BgfFile& bgf = m_files.bgf;
    static const std::uint32_t kEventName = anarkHash("eventName");
    static const std::uint32_t kTarget = anarkHash("TARGET");
    static const std::uint32_t kTimeOffset = anarkHash("timeoffset");
    std::vector<EventBehaviour> behaviours;
    for (const SlideEntry& entry : slide.entries) {
        if (!entry.active || bgf.elements[entry.element].kind != ElementKind::Behavior) {
            continue;
        }
        // The slide's values first, then the element's own.
        EventBehaviour behaviour;
        behaviour.element = static_cast<int>(entry.element);
        bool hasEvent = false;
        bool hasTime = false;
        for (const std::vector<Attribute>* attributes : {&entry.attributes, &bgf.elements[entry.element].attributes}) {
            for (const Attribute& attribute : *attributes) {
                const bool text = attribute.type == static_cast<std::uint8_t>(AttributeType::ContractPath)
                    || attribute.type == static_cast<std::uint8_t>(AttributeType::ContractString);
                if (text && attribute.raw < bgf.strings.size()) {
                    if (attribute.hash == kEventName && !hasEvent) {
                        behaviour.event = bgf.strings[attribute.raw];
                        hasEvent = true;
                    } else if (attribute.hash == kTarget && behaviour.target.empty()) {
                        behaviour.target = bgf.strings[attribute.raw];
                    }
                } else if (attribute.hash == kTimeOffset && !hasTime) {
                    behaviour.timeMs = static_cast<float>(static_cast<std::int32_t>(attribute.raw));
                    hasTime = true;
                }
            }
        }
        if (hasEvent && !behaviour.target.empty()) {
            behaviours.push_back(std::move(behaviour));
        }
    }
    return behaviours;
}

void Presentation::fireEventBehaviours(Timeline& timeline, float fromMs, float toMs)
{
    const std::size_t slide = static_cast<std::size_t>(timeline.current);
    if (slide >= timeline.eventBehaviours.size()) {
        return;
    }
    // Copied: an event can change the slide and so the list.
    const std::vector<EventBehaviour> behaviours = timeline.eventBehaviours[slide];
    for (const EventBehaviour& behaviour : behaviours) {
        if (behaviour.timeMs > fromMs && behaviour.timeMs <= toMs) {
            const int target = resolvePath(behaviour.element, behaviour.target);
            if (target >= 0) {
                m_firedEvents.push_back({target, behaviour.event});
                fireEvent(target, behaviour.event);
            }
        }
    }
}

std::vector<Presentation::FiredEvent> Presentation::takeFiredEvents()
{
    return std::exchange(m_firedEvents, {});
}

void Presentation::enterSlide(Timeline& timeline, int slideIndex)
{
    timeline.current = slideIndex;
    timeline.elapsedMs = 0.0F;
    timeline.timeMs = 0.0F;
    timeline.stopped = false;
    timeline.animation.reset();
    timeline.paused = slideOf(timeline).startsPaused;
}

void Presentation::resolveTime(Timeline& timeline)
{
    const Slide& slide = slideOf(timeline);
    const float duration = static_cast<float>(slide.durationMs);
    if (duration <= 0.0F) {
        timeline.timeMs = 0.0F;
        return;
    }
    if (timeline.elapsedMs < 0.0F) {
        timeline.elapsedMs = 0.0F;
    }
    if (slide.playCount > 0) {
        const float end = duration * static_cast<float>(slide.playCount);
        if (timeline.elapsedMs >= end) {
            timeline.elapsedMs = end;
            if (!timeline.paused) {
                timeline.stopped = true;
            }
            // Holding at the end: forwards passes end at the duration,
            // ping-pong ones at the end of the last pass's direction.
            timeline.timeMs = slide.pingPong && slide.playCount % 2 == 0 ? 0.0F : duration;
            return;
        }
    }
    const float passes = std::floor(timeline.elapsedMs / duration);
    const float within = timeline.elapsedMs - passes * duration;
    const bool backwards = slide.pingPong && static_cast<long long>(passes) % 2 == 1;
    timeline.timeMs = backwards ? duration - within : within;
}

int Presentation::findBelow(int element, std::string_view name) const
{
    for (const int child : m_nodes[static_cast<std::size_t>(element)].children) {
        if (m_nodes[static_cast<std::size_t>(child)].name == name) {
            return child;
        }
        const int found = findBelow(child, name);
        if (found >= 0) {
            return found;
        }
    }
    return -1;
}

int Presentation::find(std::string_view path) const
{
    int current = -1;
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].parent < 0) {
            current = static_cast<int>(i);
            break;
        }
    }
    while (!path.empty() && current >= 0) {
        const std::size_t slash = path.find('/');
        const std::string_view step = path.substr(0, slash);
        current = findBelow(current, step);
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
    }
    return current;
}

std::vector<int> Presentation::childrenNamed(int element, std::string_view name) const
{
    std::vector<int> out;
    for (const int child : m_nodes[static_cast<std::size_t>(element)].children) {
        if (m_nodes[static_cast<std::size_t>(child)].name == name) {
            out.push_back(child);
        }
    }
    return out;
}

std::vector<std::string> Presentation::slideNames(int component) const
{
    std::vector<std::string> names;
    if (const auto it = m_timelines.find(component); it != m_timelines.end()) {
        for (const int s : it->second.slides) {
            names.push_back(m_files.bgf.slides[static_cast<std::size_t>(s)].name);
        }
    }
    return names;
}

bool Presentation::goToSlide(int component, std::string_view slide)
{
    const auto it = m_timelines.find(component);
    if (it == m_timelines.end()) {
        return false;
    }
    Timeline& timeline = it->second;
    for (std::size_t i = 1; i < timeline.slides.size(); ++i) {
        if (m_files.bgf.slides[static_cast<std::size_t>(timeline.slides[i])].name == slide) {
            enterSlide(timeline, static_cast<int>(i));
            return true;
        }
    }
    return false;
}

bool Presentation::goToSlideIndex(int component, int index)
{
    const auto it = m_timelines.find(component);
    if (it == m_timelines.end() || index < 0 || static_cast<std::size_t>(index) >= it->second.slides.size()) {
        return false;
    }
    enterSlide(it->second, index);
    return true;
}

std::string Presentation::currentSlide(int component) const
{
    const auto it = m_timelines.find(component);
    if (it == m_timelines.end()) {
        return {};
    }
    return m_files.bgf.slides[static_cast<std::size_t>(it->second.slides[static_cast<std::size_t>(it->second.current)])]
        .name;
}

void Presentation::seek(int component, float ms)
{
    const auto it = m_timelines.find(component);
    if (it != m_timelines.end()) {
        it->second.elapsedMs = ms;
        it->second.stopped = false;
        resolveTime(it->second);
    }
}

void Presentation::play(int component)
{
    const auto it = m_timelines.find(component);
    if (it != m_timelines.end()) {
        it->second.paused = false;
    }
}

float Presentation::time(int component) const
{
    const auto it = m_timelines.find(component);
    return it == m_timelines.end() ? 0.0F : it->second.timeMs;
}

float Presentation::duration(int component) const
{
    const auto it = m_timelines.find(component);
    return it == m_timelines.end() ? 0.0F : static_cast<float>(slideOf(it->second).durationMs);
}

void Presentation::goToProgress(int component, float fraction)
{
    const float ms = std::floor(duration(component) * fraction + 0.5F);
    seek(component, ms);
}

float Presentation::progress(int component) const
{
    const float length = duration(component);
    return length > 0.0F ? time(component) / length : 0.0F;
}

void Presentation::animateProgress(int component, float target, float seconds, int curve)
{
    const auto it = m_timelines.find(component);
    if (it == m_timelines.end()) {
        return;
    }
    Timeline::ProgressAnimation animation;
    animation.from = progress(component);
    animation.to = target;
    animation.seconds = seconds;
    animation.curve = curve;
    it->second.animation = animation;
    if (seconds <= 0.0F) {
        it->second.animation.reset();
        goToProgress(component, target);
    }
}

const std::string* Presentation::stringAttribute(int element, std::uint32_t hash) const
{
    const BgfFile& bgf = m_files.bgf;
    for (const Attribute& attribute : bgf.elements[static_cast<std::size_t>(element)].attributes) {
        if (attribute.hash == hash
            && (attribute.type == static_cast<std::uint8_t>(AttributeType::ContractPath)
                || attribute.type == static_cast<std::uint8_t>(AttributeType::ContractString)
                || attribute.type == static_cast<std::uint8_t>(AttributeType::String))
            && attribute.raw < bgf.strings.size()) {
            return &bgf.strings[attribute.raw];
        }
    }
    return nullptr;
}

int Presentation::contract(int element, std::string_view className) const
{
    if (element < 0) {
        return -1;
    }
    for (const int child : m_nodes[static_cast<std::size_t>(element)].children) {
        if (kind(child) != ElementKind::Behavior) {
            continue;
        }
        if (const std::string* value = stringAttribute(child, kContractClass); value && *value == className) {
            return child;
        }
    }
    return -1;
}

int Presentation::contractParameter(int contractElement, std::string_view parameter) const
{
    if (contractElement < 0) {
        return -1;
    }
    const std::string* path = stringAttribute(contractElement, anarkHash(parameter));
    return path ? resolvePath(contractElement, *path) : -1;
}

int Presentation::resolvePath(int from, std::string_view path) const
{
    int current = from;
    std::string_view rest = path;
    while (!rest.empty() && current >= 0) {
        const std::size_t dot = rest.find('.');
        const std::string_view step = rest.substr(0, dot);
        rest = dot == std::string_view::npos ? std::string_view() : rest.substr(dot + 1);
        if (step == "this") {
            continue;
        }
        if (step == "parent") {
            current = parent(current);
            continue;
        }
        // Paths use the names as authored; the render data's copies of
        // them sometimes differ ("Small Hatches" became "Small_Hatches"),
        // so match the presentation's name hash as well.
        const std::uint32_t hash = anarkHash(step);
        const auto matches = [&](int element) {
            return m_files.bgf.elements[static_cast<std::size_t>(element)].nameHash == hash || name(element) == step;
        };
        int found = -1;
        for (const int child : children(current)) {
            if (matches(child)) {
                found = child;
                break;
            }
        }
        if (found < 0) {
            const std::function<int(int)> search = [&](int element) {
                for (const int child : children(element)) {
                    if (matches(child)) {
                        return child;
                    }
                    const int below = search(child);
                    if (below >= 0) {
                        return below;
                    }
                }
                return -1;
            };
            found = search(current);
        }
        current = found;
    }
    return current;
}

std::optional<float> Presentation::contractNumber(int contractElement, std::string_view parameter) const
{
    if (contractElement < 0) {
        return std::nullopt;
    }
    const std::uint32_t hash = anarkHash(parameter);
    for (const Attribute& attribute : m_files.bgf.elements[static_cast<std::size_t>(contractElement)].attributes) {
        if (attribute.hash != hash) {
            continue;
        }
        if (attribute.type == static_cast<std::uint8_t>(AttributeType::ContractFloat)) {
            return std::bit_cast<float>(attribute.raw);
        }
        if (attribute.type == static_cast<std::uint8_t>(AttributeType::ContractInteger)) {
            return static_cast<float>(static_cast<std::int32_t>(attribute.raw));
        }
    }
    return std::nullopt;
}

bool Presentation::fireEvent(int owner, std::string_view event)
{
    const auto it = m_logic.find(owner);
    if (it == m_logic.end()) {
        return false;
    }
    const std::uint32_t hash = anarkHash(event);
    bool handled = false;
    for (const LogicHandler& handler : it->second->handlers) {
        if (handler.event != hash) {
            continue;
        }
        handled = true;
        for (const LogicAction& action : handler.actions) {
            const int target = static_cast<int>(action.target);
            if (action.command == command::GoToSlideName) {
                const auto timeline = m_timelines.find(target);
                if (timeline == m_timelines.end()) {
                    continue;
                }
                for (std::size_t i = 1; i < timeline->second.slides.size(); ++i) {
                    const Slide& slide = m_files.bgf.slides[static_cast<std::size_t>(timeline->second.slides[i])];
                    if (anarkHash(slide.name) == (action.key & 0x07FFFFFFU)) {
                        enterSlide(timeline->second, static_cast<int>(i));
                        break;
                    }
                }
            } else if (action.command == command::SetProperty) {
                Attribute attribute;
                attribute.hash = action.key & 0x07FFFFFFU;
                attribute.type = static_cast<std::uint8_t>(action.key >> 27);
                attribute.raw = action.value;
                // Actions store their values with the contract types (17, 19).
                switch (attribute.type) {
                case static_cast<std::uint8_t>(AttributeType::Integer):
                case static_cast<std::uint8_t>(AttributeType::Float):
                case static_cast<std::uint8_t>(AttributeType::Boolean):
                case static_cast<std::uint8_t>(AttributeType::ContractInteger):
                case static_cast<std::uint8_t>(AttributeType::ContractFloat):
                    setAttribute(target, attribute.hash, attribute.asFloat());
                    break;
                default:
                    break;
                }
            } else if (action.command == command::GoToTime) {
                seek(target, static_cast<float>(action.key));
            } else if (action.command == command::Play) {
                play(target);
            }
        }
    }
    return handled;
}

void Presentation::setText(int element, std::string text)
{
    m_nodes[static_cast<std::size_t>(element)].textOverride = std::move(text);
}

void Presentation::setAttribute(int element, std::uint32_t hash, float value)
{
    m_nodes[static_cast<std::size_t>(element)].overrides.set(hash, value);
}

void Presentation::clearAttribute(int element, std::uint32_t hash)
{
    m_nodes[static_cast<std::size_t>(element)].overrides.erase(hash);
}

void Presentation::setHidden(int element, bool hidden)
{
    m_nodes[static_cast<std::size_t>(element)].hidden = hidden;
}

void Presentation::reset()
{
    for (Node& node : m_nodes) {
        node.overrides = Values();
        node.textOverride.reset();
        node.hidden = false;
    }
    for (auto& [owner, timeline] : m_timelines) {
        enterSlide(timeline, timeline.slides.size() > 1 ? 1 : 0);
    }
}

void Presentation::update(float dtSeconds)
{
    const float dtMs = dtSeconds * 1000.0F;
    for (auto& [owner, timeline] : m_timelines) {
        if (timeline.animation) {
            Timeline::ProgressAnimation& animation = *timeline.animation;
            animation.elapsed += dtSeconds;
            float fraction = animation.to;
            if (animation.elapsed < animation.seconds) {
                const float t = animation.elapsed / animation.seconds;
                const float span = animation.to - animation.from;
                constexpr float kPi = std::numbers::pi_v<float>;
                switch (animation.curve) {
                case 1:
                    fraction = animation.from + span * t;
                    break;
                case 2:
                    fraction = animation.from + span * std::sin(t * kPi / 2.0F);
                    break;
                case 3:
                    fraction = animation.from + span * (1.0F - std::cos(t * kPi / 2.0F));
                    break;
                case 4:
                    fraction = animation.from + span * 0.5F * (1.0F - std::cos(t * kPi));
                    break;
                default:
                    // The tick starts from 0 and only overwrites it for the
                    // four known curves.
                    fraction = 0.0F;
                    break;
                }
            } else {
                timeline.animation.reset();
            }
            const float length = static_cast<float>(slideOf(timeline).durationMs);
            timeline.elapsedMs = std::floor(length * fraction + 0.5F);
            timeline.stopped = false;
            resolveTime(timeline);
            continue;
        }
        if (timeline.paused || timeline.stopped) {
            continue;
        }
        const int slideBefore = timeline.current;
        // A slide just entered has not passed its start yet.
        const float before = timeline.elapsedMs > 0.0F ? timeline.timeMs : -1.0F;
        timeline.elapsedMs += dtMs;
        resolveTime(timeline);
        if (timeline.timeMs >= before) {
            fireEventBehaviours(timeline, before, timeline.timeMs);
        }
        if (timeline.current != slideBefore) {
            continue;
        }
        const Slide& slide = slideOf(timeline);
        if (timeline.stopped && slide.nextAtEnd && !slide.pingPong
            && static_cast<std::size_t>(timeline.current) + 1 < timeline.slides.size()) {
            enterSlide(timeline, timeline.current + 1);
        }
    }
}

Presentation::State Presentation::computeState() const
{
    const BgfFile& bgf = m_files.bgf;
    State state;
    state.values.resize(m_nodes.size());
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        state.values[i] = m_nodes[i].base;
    }
    // listed[e] counts the components that list e in a slide shown now.
    std::vector<int> listed(m_nodes.size(), 0);
    for (const int component : m_componentOrder) {
        const Timeline& timeline = m_timelines.at(component);
        const int shown[2] = {timeline.slides.front(), timeline.slides[static_cast<std::size_t>(timeline.current)]};
        const int passes = shown[0] == shown[1] ? 1 : 2;
        for (int pass = 0; pass < passes; ++pass) {
            const Slide& slide = bgf.slides[static_cast<std::size_t>(shown[pass])];
            const float t = std::min(timeline.timeMs, static_cast<float>(slide.durationMs));
            for (const SlideEntry& entry : slide.entries) {
                auto& values = state.values[entry.element];
                for (const Attribute& attribute : entry.attributes) {
                    if (isStringAttribute(attribute)) {
                        if (attribute.raw >= bgf.strings.size()) {
                            continue;
                        }
                        if (attribute.hash == attr::TextString) {
                            state.texts[entry.element] = bgf.strings[attribute.raw];
                        } else if (attribute.hash == attr::Font) {
                            state.fonts[entry.element] = bgf.strings[attribute.raw];
                        }
                    } else if (attribute.type <= 5) {
                        values.set(attribute.hash, attribute.asFloat());
                    }
                }
                for (const std::uint16_t trackIndex : entry.tracks) {
                    const Track& track = bgf.tracks[trackIndex];
                    state.values[track.element].set(track.attribute, track.evaluate(t));
                }
            }
        }
        // Count each element once per component even if both slides list it.
        const std::vector<std::uint32_t>& master = timeline.listed.front();
        for (const std::uint32_t element : master) {
            ++listed[element];
        }
        if (passes == 2) {
            for (const std::uint32_t element : timeline.listed[static_cast<std::size_t>(timeline.current)]) {
                if (!std::binary_search(master.begin(), master.end(), element)) {
                    ++listed[element];
                }
            }
        }
    }
    state.present.assign(m_nodes.size(), 0);
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        const Node& node = m_nodes[i];
        if (!node.overrides.empty()) {
            state.values[i].apply(node.overrides);
        }
        state.present[i] = !node.hidden && listed[i] == static_cast<int>(node.listedBy.size()) ? 1 : 0;
    }
    return state;
}

int Presentation::layerOf(int element) const
{
    for (int e = element; e >= 0; e = m_nodes[static_cast<std::size_t>(e)].parent) {
        const auto it = std::find(m_layers.begin(), m_layers.end(), e);
        if (it != m_layers.end()) {
            return static_cast<int>(it - m_layers.begin());
        }
    }
    return 0;
}

std::vector<DrawItem> Presentation::evaluate() const
{
    const State state = computeState();
    std::vector<DrawItem> items;
    std::vector<float> opacity(m_nodes.size(), 1.0F);
    int order = 0;

    const std::function<void(int, const glm::mat4&, float, bool)> visit
        = [&](int element, const glm::mat4& parentWorld, float parentOpacity, bool parentPresent) {
        const auto index = static_cast<std::size_t>(element);
        const Node& node = m_nodes[index];
        const auto& values = state.values[index];
        const bool present = parentPresent && state.present[index];
        m_present[index] = present ? 1 : 0;

        const glm::vec3 position(
            values.get(attr::PositionX, 0.0F), values.get(attr::PositionY, 0.0F), values.get(attr::PositionZ, 0.0F));
        const glm::vec3 rotation(
            values.get(attr::RotationX, 0.0F), values.get(attr::RotationY, 0.0F), values.get(attr::RotationZ, 0.0F));
        const glm::vec3 scale(
            values.get(attr::ScaleX, 1.0F), values.get(attr::ScaleY, 1.0F), values.get(attr::ScaleZ, 1.0F));
        const glm::vec3 pivot(
            values.get(attr::PivotX, 0.0F), values.get(attr::PivotY, 0.0F), values.get(attr::PivotZ, 0.0F));
        // Anark is left-handed (z into the screen); in the right-handed
        // matrices used here that turns rotations about x and y around.
        glm::mat4 local = glm::translate(glm::mat4(1.0F), position);
        if (rotation.z != 0.0F) {
            local = glm::rotate(local, rotation.z, glm::vec3(0.0F, 0.0F, 1.0F));
        }
        if (rotation.y != 0.0F) {
            local = glm::rotate(local, -rotation.y, glm::vec3(0.0F, 1.0F, 0.0F));
        }
        if (rotation.x != 0.0F) {
            local = glm::rotate(local, -rotation.x, glm::vec3(1.0F, 0.0F, 0.0F));
        }
        if (pivot != glm::vec3(0.0F)) {
            local = glm::translate(local, -pivot);
        }
        local = glm::scale(local, scale);
        const glm::mat4 world = parentWorld * local;
        m_world[index] = world;
        const float nodeOpacity = parentOpacity * std::clamp(values.get(attr::Opacity, 100.0F) / 100.0F, 0.0F, 1.0F);
        opacity[index] = nodeOpacity;

        if (present && nodeOpacity > 0.0F && node.record) {
            if (node.record->kind == FbfKind::Model) {
                DrawItem item;
                item.layer = layerOf(element);
                item.depth = world[3].z;
                item.order = order++;
                DrawModel& model = item.model;
                model.mesh = &m_files.fbf.meshes[node.record->mesh];
                model.world = world;
                model.color = glm::vec4(1.0F, 1.0F, 1.0F, nodeOpacity);
                for (const int child : node.children) {
                    const Node& materialNode = m_nodes[static_cast<std::size_t>(child)];
                    if (!materialNode.record || materialNode.record->kind != FbfKind::Material) {
                        continue;
                    }
                    item.material = child;
                    const auto& material = state.values[static_cast<std::size_t>(child)];
                    model.color *= glm::vec4(material.get(attr::DiffuseR, 255.0F), material.get(attr::DiffuseG, 255.0F),
                                       material.get(attr::DiffuseB, 255.0F), material.get(attr::DiffuseA, 255.0F))
                        / 255.0F;
                    model.color.a *= std::clamp(material.get(attr::Opacity, 100.0F) / 100.0F, 0.0F, 1.0F);
                    model.sourceBlend = materialNode.record->material.sourceBlend;
                    model.destinationBlend = materialNode.record->material.destinationBlend;
                    for (const int imageIndex : materialNode.children) {
                        const Node& imageNode = m_nodes[static_cast<std::size_t>(imageIndex)];
                        if (!imageNode.record || imageNode.record->kind != FbfKind::Image) {
                            continue;
                        }
                        const auto& image = state.values[static_cast<std::size_t>(imageIndex)];
                        UvTransform uv;
                        uv.rotationDegrees = image.get(attr::RotationUv, 0.0F);
                        uv.position = {image.get(attr::PositionU, 0.0F), image.get(attr::PositionV, 0.0F)};
                        uv.scale = {image.get(attr::ScaleU, 1.0F), image.get(attr::ScaleV, 1.0F)};
                        uv.pivot = {image.get(attr::PivotU, 0.0F), image.get(attr::PivotV, 0.0F)};
                        DrawModel::Layer layer;
                        layer.texture = texturePathForScene(imageNode.record->image.path);
                        layer.imageName = imageNode.name;
                        uv.matrix(layer.uvRow0, layer.uvRow1);
                        layer.wrapU = imageNode.record->image.wrapU;
                        layer.wrapV = imageNode.record->image.wrapV;
                        model.layers.push_back(std::move(layer));
                    }
                    break;
                }
                if (model.color.a > 0.0F) {
                    items.push_back(std::move(item));
                }
            } else if (node.record->kind == FbfKind::Text) {
                DrawItem item;
                item.isText = true;
                item.layer = layerOf(element);
                item.depth = world[3].z;
                item.order = order++;
                DrawText& text = item.text;
                text.world = world;
                if (node.textOverride) {
                    text.text = *node.textOverride;
                } else if (const auto slideText = state.texts.find(static_cast<std::uint32_t>(element));
                    slideText != state.texts.end()) {
                    text.text = slideText->second;
                } else {
                    text.text = node.baseText;
                }
                const auto slideFont = state.fonts.find(static_cast<std::uint32_t>(element));
                text.font = slideFont != state.fonts.end() ? slideFont->second : node.font;
                text.size = values.get(attr::Size, 12.0F);
                text.tracking = values.get(attr::Tracking, 0.0F);
                text.leading = values.get(attr::Leading, 0.0F);
                text.horizontalAlign = static_cast<int>(values.get(attr::HorzAlign, 0.0F));
                text.verticalAlign = static_cast<int>(values.get(attr::VertAlign, 1.0F));
                text.color = glm::vec4(values.get(attr::TextColorR, 255.0F), values.get(attr::TextColorG, 255.0F),
                                 values.get(attr::TextColorB, 255.0F), values.get(attr::TextColorA, 255.0F))
                    / 255.0F;
                text.color.a *= nodeOpacity;
                if (text.color.a > 0.0F && !text.text.empty()) {
                    items.push_back(std::move(item));
                }
            }
        }
        for (const int child : node.children) {
            visit(child, world, nodeOpacity, present);
        }
    };
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].parent < 0) {
            visit(static_cast<int>(i), glm::mat4(1.0F), 1.0F, true);
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
        if (a.layer != b.layer) {
            return a.layer > b.layer;
        }
        if (a.depth != b.depth) {
            return a.depth > b.depth;
        }
        return a.order < b.order;
    });
    return items;
}

std::optional<float> Presentation::attribute(int element, std::uint32_t hash) const
{
    const State state = computeState();
    if (const float* value = state.values[static_cast<std::size_t>(element)].find(hash)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<float> Presentation::trackValue(int element, std::uint32_t hash, float ms) const
{
    for (const Track& track : m_files.bgf.tracks) {
        if (track.element == static_cast<std::uint32_t>(element) && track.attribute == hash) {
            return track.evaluate(ms);
        }
    }
    return std::nullopt;
}

std::string Presentation::text(int element) const
{
    const Node& node = m_nodes[static_cast<std::size_t>(element)];
    if (node.textOverride) {
        return *node.textOverride;
    }
    const State state = computeState();
    const auto it = state.texts.find(static_cast<std::uint32_t>(element));
    return it != state.texts.end() ? it->second : node.baseText;
}

glm::mat4 Presentation::worldTransform(int element) const
{
    return m_world[static_cast<std::size_t>(element)];
}

bool Presentation::present(int element) const
{
    return m_present[static_cast<std::size_t>(element)] != 0;
}

} // namespace fh::ui
