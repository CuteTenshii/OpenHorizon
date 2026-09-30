#include "Cutscenes.h"

#include "base/Bytes.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <glm/gtc/matrix_transform.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <numbers>

namespace openhorizon {

namespace {

CutsceneSpace spaceOf(std::string_view name)
{
    if (name == "PartSpace") {
        return CutsceneSpace::Part;
    }
    if (name == "WorldSpace") {
        return CutsceneSpace::World;
    }
    return CutsceneSpace::Car;
}

std::optional<FadeOptions> readFade(const pugi::xml_node& node)
{
    FadeOptions fade;
    fade.duration = node.attribute("Duration").as_float();
    if (fade.duration <= 0.0F) {
        return std::nullopt;
    }
    fade.hold = node.attribute("HoldDuration").as_float();
    fade.sCurve = std::string_view(node.attribute("CurveType").as_string()) == "S";
    fade.colour = glm::vec3(node.attribute("Color.x").as_float(), node.attribute("Color.y").as_float(),
        node.attribute("Color.z").as_float());
    return fade;
}

CutsceneCamera readCamera(const pugi::xml_node& node)
{
    CutsceneCamera camera;
    camera.type = node.attribute("Type").as_string();
    camera.start = node.attribute("StartCut").as_float();
    camera.duration = node.attribute("duration").as_float();
    camera.target = node.attribute("TargetName").as_string();
    if (const pugi::xml_node anim = node.child("Anim")) {
        camera.positionSpace = spaceOf(anim.attribute("PosAnimType").as_string());
        camera.rotationSpace = spaceOf(anim.attribute("RotAnimType").as_string());
        camera.easeIn = std::clamp(anim.attribute("EaseIn").as_float(), 0.0F, 1.0F);
        camera.easeOut = std::clamp(anim.attribute("EaseOut").as_float(), 0.0F, 1.0F);
        for (const pugi::xml_node key : anim.children("Key")) {
            CutsceneKey k;
            k.time = key.attribute("Time").as_float();
            k.carPart = key.attribute("CarPart").as_string();
            k.position = glm::vec3(
                key.attribute("PosX").as_float(), key.attribute("PosY").as_float(), key.attribute("PosZ").as_float());
            k.yaw = key.attribute("Yaw").as_float();
            k.pitch = key.attribute("Pitch").as_float();
            k.roll = key.attribute("Roll").as_float();
            k.fov = key.attribute("FOV").as_float();
            camera.keys.push_back(std::move(k));
        }
    }
    camera.fadeIn = readFade(node.child("FadeInOptions"));
    camera.fadeOut = readFade(node.child("FadeOutOptions"));
    return camera;
}

Cutscene readCutscene(const pugi::xml_node& node)
{
    Cutscene cutscene;
    cutscene.name = node.attribute("name").as_string();
    cutscene.looping = std::string_view(node.attribute("PlaybackMode").as_string()) == "Looping";
    cutscene.duration = node.attribute("Duration").as_float();
    cutscene.loopFrom = node.attribute("LoopFrom").as_float();
    for (const pugi::xml_node cams : node.children("Cams")) {
        for (const pugi::xml_node cam : cams.children("Cam")) {
            cutscene.cameras.push_back(readCamera(cam));
        }
    }
    for (const pugi::xml_node events : node.children("Events")) {
        for (const pugi::xml_node event : events.children("Event")) {
            const pugi::xml_node trigger = event.child("EventTrigger");
            if (!trigger) {
                continue;
            }
            CutsceneEvent e;
            e.time = event.attribute("time").as_float();
            for (const pugi::xml_attribute attribute : trigger.attributes()) {
                const std::string_view name = attribute.name();
                if (name == "id") {
                    e.trigger = attribute.as_string();
                } else if (name == "name") {
                    e.name = attribute.as_string();
                } else {
                    e.attributes.emplace(std::string(name), attribute.as_string());
                }
            }
            cutscene.events.push_back(std::move(e));
        }
    }
    std::stable_sort(cutscene.events.begin(), cutscene.events.end(),
        [](const CutsceneEvent& a, const CutsceneEvent& b) { return a.time < b.time; });
    return cutscene;
}

} // namespace

std::optional<float> Cutscene::firstEvent(std::string_view trigger) const
{
    for (const CutsceneEvent& event : events) {
        if (event.trigger == trigger) {
            return event.time;
        }
    }
    return std::nullopt;
}

float Cutscene::timeAt(float elapsed) const
{
    if (elapsed < duration) {
        return std::max(elapsed, 0.0F);
    }
    if (!looping || duration <= loopFrom) {
        return duration;
    }
    return loopFrom + std::fmod(elapsed - duration, duration - loopFrom);
}

const CutsceneCamera* Cutscene::cameraAt(float time) const
{
    const CutsceneCamera* found = nullptr;
    for (const CutsceneCamera& camera : cameras) {
        if (camera.start <= time) {
            found = &camera;
        }
    }
    if (found != nullptr && time > found->start + found->duration) {
        return nullptr;
    }
    return found;
}

CutsceneLibrary CutsceneLibrary::load(
    const Vfs& vfs, std::string_view track, bool raceSession, std::string_view trackDevName)
{
    CutsceneLibrary library;
    const std::string folder = std::format("media/gamemodes/{}/cutscenes", toLower(track));
    std::vector<std::string> files{folder + "/cutscenes_default_both.xml",
        folder + (raceSession ? "/cutscenes_default_race.xml" : "/cutscenes_default_freeroam.xml")};
    if (!trackDevName.empty()) {
        files.push_back(std::format("{}/tracks/cutscenes_{}.xml", folder, toLower(trackDevName)));
    }
    for (const std::string& path : files) {
        const std::optional<std::string> text = vfs.readText(path);
        pugi::xml_document doc;
        if (!text || !doc.load_buffer(text->data(), text->size())) {
            logWarning("cutscenes: cannot read {}", path);
            continue;
        }
        // A later file's cutscene of the same name takes the place of the
        // earlier one, as loading it into the manager's table by name does
        // (inferred; no name repeats across the files one session reads).
        for (const pugi::xml_node node : doc.child("CutsceneManager").children("Cutscene")) {
            Cutscene cutscene = readCutscene(node);
            library.m_cutscenes.insert_or_assign(toLower(cutscene.name), std::move(cutscene));
        }
    }
    return library;
}

const Cutscene* CutsceneLibrary::find(std::string_view name) const
{
    const auto it = m_cutscenes.find(toLower(name));
    return it == m_cutscenes.end() ? nullptr : &it->second;
}

const Cutscene* CutsceneLibrary::raceSpecific(
    std::string_view base, std::string_view trackDevName, std::string_view horizonEventId, int eventId) const
{
    std::vector<std::string> names;
    if (!trackDevName.empty() && !horizonEventId.empty()) {
        names.push_back(std::format("{}_{}_{}", base, trackDevName, horizonEventId));
    }
    if (!trackDevName.empty()) {
        names.push_back(std::format("{}_{}", base, trackDevName));
    }
    if (!horizonEventId.empty()) {
        names.push_back(std::format("{}_{}", base, horizonEventId));
    }
    names.push_back(std::format("{}_event{}", base, eventId));
    names.emplace_back(base);
    for (const std::string& name : names) {
        if (const Cutscene* found = find(name)) {
            return found;
        }
    }
    return nullptr;
}

namespace {

/// The eased clock of a cut (0x8290a8b0): `u` is the fraction of the keys'
/// span played, `a` and `b` the EaseIn and EaseOut.
float ease(float u, float a, float b)
{
    const float k1 = a == 0.0F ? 0.0F : 1.0F / (((1.0F - b - a) * 2.0F + a + b) * a);
    float k2 = 0.0F;
    if (b != 0.0F) {
        k2 = a == 0.0F ? 1.0F / (((1.0F - b) * 2.0F + b) * b) : k1 * a / b;
    }
    float v = 1.0F;
    if (a != 0.0F) {
        v = 2.0F * k1 * a;
    } else if (b != 0.0F) {
        v = 2.0F * k2 * b;
    }
    if (a == 0.0F && b == 0.0F) {
        return u;
    }
    if (u < a) {
        return k1 * u * u;
    }
    if (u > 1.0F - b) {
        return 1.0F - k2 * (1.0F - u) * (1.0F - u);
    }
    return (u - a) * v + k1 * a * a;
}

/// The Hermite curve through keyed values (0x82909990, 0x82900f70).
float hermite(const std::vector<float>& times, const std::vector<float>& values, float t)
{
    const std::size_t n = values.size();
    if (n == 1) {
        return values.front();
    }
    std::size_t i = 0;
    while (i + 2 < n && t > times[i + 1]) {
        ++i;
    }
    const float span = times[i + 1] - times[i];
    const float u = span > 0.0F ? std::clamp((t - times[i]) / span, 0.0F, 1.0F) : 1.0F;
    const auto tangent = [&](std::size_t j) {
        if (j == 0 || j + 1 >= n) {
            return values[i + 1] - values[i];
        }
        const float before = times[j] - times[j - 1];
        const float after = times[j + 1] - times[j];
        const float whole = times[j + 1] - times[j - 1];
        const float slopeBefore = before > 0.0F ? (values[j] - values[j - 1]) / before : 0.0F;
        const float slopeAfter = after > 0.0F ? (values[j + 1] - values[j]) / after : 0.0F;
        return whole > 0.0F ? span * (after / whole * slopeBefore + before / whole * slopeAfter) : 0.0F;
    };
    const float m0 = tangent(i);
    const float m1 = tangent(i + 1);
    const float u2 = u * u;
    const float u3 = u2 * u;
    return (2.0F * u3 - 3.0F * u2 + 1.0F) * values[i] + (u3 - 2.0F * u2 + u) * m0
        + (3.0F * u2 - 2.0F * u3) * values[i + 1] + (u3 - u2) * m1;
}

/// Key angles in radians, each moved by whole turns to within half a turn
/// of the key before it (0x8290ff80).
std::vector<float> unwrapped(const std::vector<CutsceneKey>& keys, float CutsceneKey::* angle)
{
    std::vector<float> out;
    out.reserve(keys.size());
    constexpr float kTurn = 2.0F * std::numbers::pi_v<float>;
    for (const CutsceneKey& key : keys) {
        float value = glm::radians(key.*angle);
        if (!out.empty()) {
            value -= kTurn * std::round((value - out.back()) / kTurn);
        }
        out.push_back(value);
    }
    return out;
}

constexpr std::array<std::string_view, kCutsceneCarParts> kCarPartNames{"headlightL", "headlightR", "taillightL",
    "taillightR", "steering", "wheelLF", "wheelRF", "wheelRR", "wheelLR", "handsAnchor"};

} // namespace

int cutsceneCarPart(std::string_view name)
{
    for (std::size_t i = 0; i < kCarPartNames.size(); ++i) {
        if (kCarPartNames[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void CutscenePlayer::play(const Cutscene& cutscene)
{
    m_cutscene = &cutscene;
    m_elapsed = 0.0F;
    m_time = 0.0F;
    m_countdown = false;
    m_startCountdown = false;
    m_ended = false;
    m_started = false;
    m_fired.clear();
    m_nodes.clear();
}

void CutscenePlayer::stop()
{
    m_cutscene = nullptr;
    m_fired.clear();
    m_nodes.clear();
}

void CutscenePlayer::advance(float dt)
{
    if (m_cutscene == nullptr) {
        return;
    }
    const float from = m_elapsed;
    m_elapsed += std::abs(dt);
    // Events fire once, as the first pass through the cutscene reaches them;
    // a looping cutscene does not fire them again (inferred: the looping
    // parts of the race cutscenes carry no events).
    for (const CutsceneEvent& event : m_cutscene->events) {
        const bool reached = event.time <= m_elapsed && (event.time > from || (!m_started && event.time <= from));
        if (!reached || event.time > m_cutscene->duration) {
            continue;
        }
        if (event.trigger == "CCutsceneCountdownTrigger") {
            m_countdown = true;
        } else if (event.trigger == "CCutsceneStartCountdownTrigger") {
            m_startCountdown = true;
        } else if (event.trigger == "CCutsceneCameraNodeTrigger") {
            const auto number = [&](std::string_view key) {
                const auto it = event.attributes.find(key);
                return it == event.attributes.end() ? 0.0F : std::strtof(it->second.c_str(), nullptr);
            };
            m_nodes[event.name] = PlacedNode{
                glm::vec3(number("x"), number("y"), number("z")), number("rotation"), number("snapToGround") != 0.0F};
        }
        m_fired.push_back(event);
    }
    m_started = true;
    m_time = m_cutscene->timeAt(m_elapsed);
    if (!m_cutscene->looping && m_elapsed >= m_cutscene->duration) {
        m_ended = true;
    }
}

std::vector<CutsceneEvent> CutscenePlayer::takeEvents()
{
    std::vector<CutsceneEvent> out;
    out.swap(m_fired);
    return out;
}

const CutsceneCamera* CutscenePlayer::camera() const
{
    return m_cutscene == nullptr ? nullptr : m_cutscene->cameraAt(m_time);
}

std::optional<glm::mat4> CutscenePlayer::targetTransform(const CutsceneCamera& camera, const CutsceneScene& scene) const
{
    if (camera.target.empty()) {
        return scene.car;
    }
    if (const auto placed = m_nodes.find(camera.target); placed != m_nodes.end()) {
        glm::vec3 position = placed->second.position;
        if (placed->second.snapToGround && scene.ground) {
            if (const std::optional<float> height = scene.ground(position.x, position.z, position.y + 50.0F)) {
                position.y = *height;
            }
        }
        return glm::rotate(glm::translate(glm::mat4(1.0F), position), glm::radians(placed->second.headingDegrees),
            glm::vec3(0.0F, 1.0F, 0.0F));
    }
    if (const auto node = scene.nodes.find(camera.target); node != scene.nodes.end()) {
        return node->second;
    }
    return std::nullopt;
}

std::optional<CutsceneView> CutscenePlayer::view(const CutsceneScene& scene) const
{
    const CutsceneCamera* cut = camera();
    if (cut == nullptr || cut->type != "Animateable" || cut->keys.empty()) {
        return std::nullopt;
    }
    const std::optional<glm::mat4> target = targetTransform(*cut, scene);
    if (!target) {
        return std::nullopt;
    }
    const std::vector<CutsceneKey>& keys = cut->keys;
    std::vector<float> times;
    times.reserve(keys.size());
    for (const CutsceneKey& key : keys) {
        // Keys are strictly ordered in time: one no later than the key
        // before it is moved just past it, as the parser does (0x8290ff80;
        // the step it adds was not read, a millisecond is used).
        float time = key.time;
        if (!times.empty() && time <= times.back()) {
            time = times.back() + 0.001F;
        }
        times.push_back(time);
    }
    const float first = times.front();
    const float span = times.back() - first;
    float t = std::clamp(m_time - cut->start, first, times.back());
    if (span > 0.0F) {
        t = first + span * ease((t - first) / span, cut->easeIn, cut->easeOut);
    }
    const auto channel = [&](auto value) {
        std::vector<float> values;
        values.reserve(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            values.push_back(value(i));
        }
        return hermite(times, values, t);
    };

    std::vector<glm::vec3> offsets;
    offsets.reserve(keys.size());
    for (const CutsceneKey& key : keys) {
        glm::vec3 offset = key.position;
        if (cut->positionSpace == CutsceneSpace::Part) {
            const int part = cutsceneCarPart(key.carPart);
            if (part < 0 || !scene.parts[static_cast<std::size_t>(part)]) {
                return std::nullopt;
            }
            offset += *scene.parts[static_cast<std::size_t>(part)];
        }
        offsets.push_back(offset);
    }
    const glm::vec3 local(channel([&](std::size_t i) { return offsets[i].x; }),
        channel([&](std::size_t i) { return offsets[i].y; }), channel([&](std::size_t i) { return offsets[i].z; }));
    const std::vector<float> yaws = unwrapped(keys, &CutsceneKey::yaw);
    const std::vector<float> pitches = unwrapped(keys, &CutsceneKey::pitch);
    const std::vector<float> rolls = unwrapped(keys, &CutsceneKey::roll);
    const float yaw = channel([&](std::size_t i) { return yaws[i]; });
    const float pitch = channel([&](std::size_t i) { return pitches[i]; });
    const float roll = channel([&](std::size_t i) { return rolls[i]; });
    const float fov = channel([&](std::size_t i) { return keys[i].fov; });

    CutsceneView view;
    view.position = cut->positionSpace == CutsceneSpace::World ? local : glm::vec3(*target * glm::vec4(local, 1.0F));
    glm::mat4 frame = cut->rotationSpace == CutsceneSpace::World ? glm::mat4(1.0F) : *target;
    frame[3] = glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
    frame = glm::rotate(frame, yaw, glm::vec3(0.0F, 1.0F, 0.0F));
    frame = glm::rotate(frame, pitch, glm::vec3(1.0F, 0.0F, 0.0F));
    frame = glm::rotate(frame, roll, glm::vec3(0.0F, 0.0F, 1.0F));
    view.forward = glm::normalize(glm::vec3(frame * glm::vec4(0.0F, 0.0F, 1.0F, 0.0F)));
    view.up = glm::normalize(glm::vec3(frame * glm::vec4(0.0F, 1.0F, 0.0F, 0.0F)));
    view.fov = glm::radians(fov);
    return view;
}

float CutscenePlayer::overlay(glm::vec3& colour) const
{
    const CutsceneCamera* cut = camera();
    if (cut == nullptr) {
        return 0.0F;
    }
    const float local = m_time - cut->start;
    if (cut->fadeIn && local < cut->fadeIn->duration) {
        colour = cut->fadeIn->colour;
        return fadeInOpacity(*cut->fadeIn, local);
    }
    if (cut->fadeOut) {
        const FadeOptions& fade = *cut->fadeOut;
        const float into = local - (cut->duration - fade.duration);
        if (into > 0.0F) {
            // Opaque once Duration less HoldDuration has passed, held for
            // the rest (0x829035b0 ramps between min(0, D - H) and
            // max(0, D - H)).
            const float end = std::max(0.0F, fade.duration - fade.hold);
            const float x = end > 0.0F ? std::clamp(into / end, 0.0F, 1.0F) : 1.0F;
            colour = fade.colour;
            return fade.sCurve ? (1.0F - std::cos(std::numbers::pi_v<float> * x)) * 0.5F : x;
        }
    }
    return 0.0F;
}

} // namespace openhorizon
