#include "AnimatedObjects.h"

#include "world/Archive.h"

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include <lzx/zip.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>

namespace fh::animated {

namespace {

/// Bytes of a set that setKind() needs.
constexpr std::size_t kKindBytes = 0x34;
/// The time-of-day gate takes a SwitchOnLights of 0 as this (0x8265AFB8).
constexpr float kLightsOffValue = -1.0F;
/// A day threshold at or above this does not gate (0x8241FE60).
constexpr float kDayGateLimit = 0.95F;

/// A small, well-mixed hash (from MurmurHash3's finaliser) for the clip
/// choices, which the game makes with its random generator.
std::uint32_t mix(std::uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85EBCA6BU;
    h ^= h >> 13;
    h *= 0xC2B2AE35U;
    h ^= h >> 16;
    return h;
}

/// Whether a world box can be seen through `viewProjection`: not wholly
/// beyond one side of the view or behind the camera.
bool inView(const glm::mat4& viewProjection, const glm::vec3& lo, const glm::vec3& hi)
{
    std::array<int, 5> outside{};
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 p(
            (corner & 1) != 0 ? hi.x : lo.x, (corner & 2) != 0 ? hi.y : lo.y, (corner & 4) != 0 ? hi.z : lo.z);
        const glm::vec4 clip = viewProjection * glm::vec4(p, 1.0F);
        outside[0] += clip.x < -clip.w ? 1 : 0;
        outside[1] += clip.x > clip.w ? 1 : 0;
        outside[2] += clip.y < -clip.w ? 1 : 0;
        outside[3] += clip.y > clip.w ? 1 : 0;
        outside[4] += clip.w <= 0.0F ? 1 : 0;
    }
    return std::ranges::none_of(outside, [](int n) { return n == 8; });
}

/// Whether the route gate shows a placement of a set with these routes
/// while `route` is loaded (as world::Placement::onRoute).
bool onRoute(const AnimatedObjects::Instance& instance, int route)
{
    if (!instance.eventProp || instance.eventRoutes.empty() || route < 0) {
        return true;
    }
    return std::ranges::find(instance.eventRoutes, static_cast<std::uint8_t>(route)) != instance.eventRoutes.end();
}

} // namespace

AnimatedObject AnimatedObject::make(std::uint32_t setNumber, AnimatedObjectSet set, const std::string& source)
{
    AnimatedObject object;
    object.setNumber = setNumber;
    object.granny = granny::readFileInfo(set.granny(), source);
    object.set = std::move(set);
    const std::vector<granny::Model>& models = object.granny.models;
    if (object.set.entries.size() > models.size()) {
        throw DataError("{}: {} entries but {} Granny models", source, object.set.entries.size(), models.size());
    }
    for (std::size_t e = 0; e < object.set.entries.size(); ++e) {
        const granny::Model& model = models[e];
        for (const auto& lod : object.set.entries[e].lods) {
            for (const AnimatedObjectBatch& batch : lod) {
                if (batch.bone >= static_cast<int>(model.bones.size())) {
                    throw DataError("{}: a batch names bone {} of {}", source, batch.bone, model.bones.size());
                }
            }
        }
    }
    for (const granny::Animation& animation : object.granny.animations) {
        std::vector<int> groups(object.set.entries.size(), -1);
        std::vector<std::vector<int>> tracks(object.set.entries.size());
        for (std::size_t e = 0; e < object.set.entries.size(); ++e) {
            const granny::Model& model = models[e];
            for (std::size_t g = 0; g < animation.groups.size(); ++g) {
                if (animation.groups[g].name == model.name) {
                    groups[e] = static_cast<int>(g);
                    break;
                }
            }
            tracks[e].assign(model.bones.size(), -1);
            if (groups[e] < 0) {
                continue;
            }
            const granny::TrackGroup& group = animation.groups[static_cast<std::size_t>(groups[e])];
            for (std::size_t t = 0; t < group.tracks.size(); ++t) {
                const int bone = model.findBone(group.tracks[t].name);
                if (bone >= 0) {
                    tracks[e][static_cast<std::size_t>(bone)] = static_cast<int>(t);
                }
            }
        }
        object.groups.push_back(std::move(groups));
        object.tracks.push_back(std::move(tracks));
    }
    return object;
}

std::optional<AnimatedObjects> AnimatedObjects::load(const lzx::ZipArchive& archive, const std::atomic<bool>* cancel)
{
    const auto cancelled = [cancel] { return cancel != nullptr && cancel->load(std::memory_order_relaxed); };
    const std::vector<std::size_t> sets = world::distinctEntries(archive, ".pgeo");
    struct Read {
        std::uint32_t kind = 0;
        std::optional<std::uint32_t> number;
        std::optional<AnimatedObject> object;
        std::optional<AnimatedPlacementSet> placements;
        bool failed = false;
    };
    std::vector<Read> reads(sets.size());
    const auto& entries = archive.entries();
    world::parallelFor(sets.size(), [&](std::size_t s) {
        if (cancelled()) {
            return;
        }
        Read& read = reads[s];
        const lzx::ZipEntry& entry = entries[sets[s]];
        try {
            read.kind = setKind(world::readEntryPrefix(archive, entry, kKindBytes));
            if (read.kind != kObjectSetKind && read.kind != kPlacementSetKind) {
                return;
            }
            read.number = world::numberBefore(entry.name, 'G', ".pgeo");
            if (!read.number) {
                read.failed = true;
                return;
            }
            Bytes data = world::readEntry(archive, entry);
            if (read.kind == kObjectSetKind) {
                read.object
                    = AnimatedObject::make(*read.number, readObjectSet(std::move(data), entry.name), entry.name);
            } else {
                read.placements = readPlacementSet(data, entry.name);
            }
        } catch (const DataError&) {
            read.failed = true;
        }
    });
    if (cancelled()) {
        return std::nullopt;
    }
    std::vector<AnimatedObject> objects;
    std::vector<AnimatedPlacementSet> placements;
    int failed = 0;
    for (Read& read : reads) {
        failed += read.failed ? 1 : 0;
        if (read.object) {
            objects.push_back(std::move(*read.object));
        }
        if (read.placements) {
            placements.push_back(std::move(*read.placements));
        }
    }
    AnimatedObjects result(std::move(objects), placements);
    result.m_failedSets = failed;
    return result;
}

AnimatedObjects::AnimatedObjects(
    std::vector<AnimatedObject> objects, const std::vector<AnimatedPlacementSet>& placements)
    : m_objects(std::move(objects))
{
    std::map<std::uint32_t, std::uint32_t> bySet;
    m_entryOffsets.resize(m_objects.size());
    m_matrixCounts.resize(m_objects.size());
    for (std::size_t o = 0; o < m_objects.size(); ++o) {
        bySet[m_objects[o].setNumber] = static_cast<std::uint32_t>(o);
        std::uint32_t offset = 0;
        for (std::size_t e = 0; e < m_objects[o].set.entries.size(); ++e) {
            m_entryOffsets[o].push_back(offset);
            offset += static_cast<std::uint32_t>(m_objects[o].granny.models[e].bones.size());
        }
        m_matrixCounts[o] = offset;
    }
    // The play state each set's shared instance uses.
    std::vector<std::optional<std::uint32_t>> shared(m_objects.size());
    for (const AnimatedPlacementSet& set : placements) {
        for (const AnimatedPlacement& placement : set.placements) {
            const auto found = bySet.find(placement.objectSet);
            if (found == bySet.end()) {
                ++m_missingObjects;
                continue;
            }
            Instance instance;
            instance.object = found->second;
            instance.placement = placement;
            const glm::vec3& x = placement.axes[0];
            const glm::vec3& y = placement.axes[1];
            const glm::vec3& z = placement.axes[2];
            instance.world = glm::mat4(
                glm::vec4(x, 0.0F), glm::vec4(y, 0.0F), glm::vec4(z, 0.0F), glm::vec4(placement.position, 1.0F));
            const AnimatedObjectSet& objectSet = m_objects[instance.object].set;
            instance.boundsMin = glm::vec3(std::numeric_limits<float>::max());
            instance.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 p((corner & 1) != 0 ? objectSet.boundsMax.x : objectSet.boundsMin.x,
                    (corner & 2) != 0 ? objectSet.boundsMax.y : objectSet.boundsMin.y,
                    (corner & 4) != 0 ? objectSet.boundsMax.z : objectSet.boundsMin.z);
                const glm::vec3 w(instance.world * glm::vec4(p, 1.0F));
                instance.boundsMin = glm::min(instance.boundsMin, w);
                instance.boundsMax = glm::max(instance.boundsMax, w);
            }
            instance.eventProp = set.eventProp;
            instance.eventRoutes = set.eventRoutes;
            if (placement.ownInstance) {
                instance.state = static_cast<std::uint32_t>(m_states.size());
                m_states.emplace_back();
            } else {
                std::optional<std::uint32_t>& state = shared[instance.object];
                if (!state) {
                    state = static_cast<std::uint32_t>(m_states.size());
                    m_states.emplace_back();
                }
                instance.state = *state;
            }
            m_states[instance.state].seed
                = mix(static_cast<std::uint32_t>(m_instances.size()) * 0x9E3779B9U ^ placement.objectSet);
            m_instances.push_back(std::move(instance));
        }
    }
    m_advanced.assign(m_states.size(), 0);
}

float AnimatedObjects::clipTime(std::size_t instance) const
{
    const PlayState& state = m_states[m_instances[instance].state];
    return state.clip < 0 ? 0.0F : static_cast<float>(state.phase + state.elapsed);
}

int AnimatedObjects::clip(std::size_t instance) const
{
    return m_states[m_instances[instance].state].clip;
}

void AnimatedObjects::sampleLocals(
    const AnimatedObject& object, int animation, std::size_t entry, float time, std::vector<granny::Transform>& out)
{
    const granny::Model& model = object.granny.models[entry];
    out.resize(model.bones.size());
    for (std::size_t b = 0; b < model.bones.size(); ++b) {
        out[b] = model.bones[b].local;
    }
    if (animation < 0 || static_cast<std::size_t>(animation) >= object.granny.animations.size()) {
        return;
    }
    const auto a = static_cast<std::size_t>(animation);
    const int group = object.groups[a][entry];
    if (group < 0) {
        return;
    }
    const granny::Animation& clip = object.granny.animations[a];
    const granny::TrackGroup& tracks = clip.groups[static_cast<std::size_t>(group)];
    const std::vector<int>& boneTracks = object.tracks[a][entry];
    for (std::size_t b = 0; b < model.bones.size(); ++b) {
        if (boneTracks[b] < 0) {
            continue;
        }
        const granny::TransformTrack& track = tracks.tracks[static_cast<std::size_t>(boneTracks[b])];
        // A tracked bone takes its whole local transform from the track;
        // what an identity curve gives is the identity.
        granny::Transform& local = out[b];
        local = granny::Transform{};
        std::array<float, 9> value{};
        if (track.position.sample(time, clip.duration, true, value.data())) {
            local.position = {value[0], value[1], value[2]};
            local.flags |= granny::Transform::kHasPosition;
        }
        if (track.orientation.sample(time, clip.duration, true, value.data())) {
            const glm::quat q(value[3], value[0], value[1], value[2]);
            const float length = std::sqrt(glm::dot(q, q));
            local.orientation = length > 0.0F ? q / length : q;
            local.flags |= granny::Transform::kHasOrientation;
        }
        if (track.scaleShear.sample(time, clip.duration, true, value.data())) {
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 3; ++column) {
                    local.scaleShear[static_cast<int>(column)][static_cast<int>(row)] = value[row * 3 + column];
                }
            }
            local.flags |= granny::Transform::kHasScaleShear;
        }
    }
}

void AnimatedObjects::update(float dt, const AnimatedView& view)
{
    m_visible.clear();
    m_pose.clear();
    std::ranges::fill(m_advanced, std::uint8_t{0});
    const float lights = view.switchOnLights > 0.0F ? view.switchOnLights : kLightsOffValue;
    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        const Instance& instance = m_instances[i];
        if (!onRoute(instance, view.route)) {
            continue;
        }
        const AnimatedPlacement& placement = instance.placement;
        const float distance = glm::length(placement.position - view.camera);
        if (placement.cullDistance < 0.0F || distance > placement.cullDistance
            || !inView(view.viewProjection, instance.boundsMin, instance.boundsMax)) {
            continue;
        }
        const int lod = distance <= placement.lodDistances[0] ? 0 : distance <= placement.lodDistances[1] ? 1 : 2;
        const AnimatedObject& object = m_objects[instance.object];

        PlayState& state = m_states[instance.state];
        const bool ready = !(placement.nightThreshold > 0.0F && lights < placement.nightThreshold)
            && !(placement.dayThreshold < kDayGateLimit && lights > placement.dayThreshold);
        if (m_advanced[instance.state] == 0) {
            m_advanced[instance.state] = 1;
            if (!ready) {
                state.clip = -1;
            } else if (state.clip >= 0) {
                state.elapsed += static_cast<double>(dt);
            } else if (!object.granny.animations.empty()
                && (placement.triggerDistance < 0.0F || distance <= placement.triggerDistance)) {
                const std::uint32_t pick = mix(state.seed + state.picks++);
                const auto count = static_cast<std::uint32_t>(object.granny.animations.size());
                state.clip = static_cast<int>(pick % count);
                const float duration = object.granny.animations[static_cast<std::size_t>(state.clip)].duration;
                state.phase = duration * static_cast<float>(mix(pick) >> 8) / static_cast<float>(1U << 24);
                state.elapsed = 0.0;
            }
        }

        Visible visible;
        visible.instance = static_cast<std::uint32_t>(i);
        visible.lod = lod;
        visible.firstMatrix = static_cast<std::uint32_t>(m_pose.size());
        m_pose.resize(m_pose.size() + m_matrixCounts[instance.object], glm::mat4(1.0F));
        float time = 0.0F;
        if (state.clip >= 0) {
            const float duration = object.granny.animations[static_cast<std::size_t>(state.clip)].duration;
            const double played = static_cast<double>(state.phase) + state.elapsed;
            time = duration > 0.0F ? static_cast<float>(std::fmod(played, static_cast<double>(duration))) : 0.0F;
        }
        for (std::size_t e = 0; e < object.set.entries.size(); ++e) {
            if (object.set.entries[e].lods[static_cast<std::size_t>(lod)].empty()) {
                continue;
            }
            const granny::Model& model = object.granny.models[e];
            sampleLocals(object, state.clip, e, time, m_locals);
            granny::worldPose(model, m_locals, m_world);
            glm::mat4* out = m_pose.data() + visible.firstMatrix + m_entryOffsets[instance.object][e];
            for (std::size_t b = 0; b < model.bones.size(); ++b) {
                out[b] = m_world[b] * model.bones[b].inverseWorld;
            }
        }
        m_visible.push_back(visible);
    }
}

} // namespace fh::animated
