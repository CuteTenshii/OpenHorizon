// Tests of the animated objects: the kind-4 and kind-5 procedural sets and
// Granny curve sampling on data built here in the layouts the headers
// document, and, when FH1_GAME_DIR names an extracted disc, Colorado's
// windmills, wind turbines and ferris wheel and the media/animatedobjects
// pack.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "animated/AnimatedObjects.h"
#include "animated/AnimatedSets.h"
#include "animated/Granny.h"
#include "vfs/Vfs.h"
#include "world/Archive.h"
#include "world/TrackWorld.h"

#include <lzx/zip.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

using fh::test::gameDir;

namespace {

using fh::Bytes;
namespace animated = fh::animated;
namespace granny = fh::animated::granny;

/// Builds big-endian test data.
struct Writer {
    Bytes data;

    void u8(std::uint8_t v) { data.push_back(v); }
    void u16(std::uint16_t v)
    {
        u8(static_cast<std::uint8_t>(v >> 8));
        u8(static_cast<std::uint8_t>(v));
    }
    void u32(std::uint32_t v)
    {
        u16(static_cast<std::uint16_t>(v >> 16));
        u16(static_cast<std::uint16_t>(v));
    }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void text(std::string_view s) { data.insert(data.end(), s.begin(), s.end()); }
    void zeros(std::size_t n) { data.insert(data.end(), n, 0); }
    void align(std::size_t n)
    {
        while (data.size() % n != 0) {
            data.push_back(0);
        }
    }
    void put32(std::size_t at, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            data[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
        }
    }
};

/// The shared procedural set header up to its name, with the name written.
Writer setHeader(std::uint32_t kind, std::string_view name, std::uint32_t entries, std::uint32_t headerEntries)
{
    Writer w;
    w.text("OEGP");
    w.zeros(0x60 - 4);
    w.put32(0x30, kind);
    w.put32(0x3C, static_cast<std::uint32_t>(name.size() + 1));
    w.put32(0x40, entries);
    w.put32(0x44, headerEntries);
    w.text(name);
    w.u8(0);
    return w;
}

void finish(Writer& w)
{
    w.put32(0x48, static_cast<std::uint32_t>(w.data.size()));
}

void placement(Writer& w, const glm::vec3& position, std::uint32_t entry, float cull, float night)
{
    // Axes: a quarter turn about Y.
    const std::array<glm::vec3, 3> axes{glm::vec3(0, 0, -1), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)};
    for (const glm::vec3& axis : axes) {
        w.f32(axis.x);
        w.f32(axis.y);
        w.f32(axis.z);
        w.f32(0.0F);
    }
    w.f32(position.x);
    w.f32(position.y);
    w.f32(position.z);
    w.f32(1.0F);
    w.u32(entry);
    w.f32(500.0F);
    w.f32(1800.0F);
    w.f32(cull);
    w.u32(1);
    w.u32(0x01000000);
    w.f32(-1.0F);
    w.f32(-1.0F);
    w.u32(0);
    w.f32(-1.0F);
    w.f32(night);
    w.f32(1.0F);
    w.u32(0);
    w.u32(0);
    w.zeros(8);
}

Bytes placementSet()
{
    Writer w = setHeader(animated::kPlacementSetKind, "anim_proc_test", 0, 2);
    w.align(4);
    w.u32(7);
    w.u32(0);
    w.u32(32);
    w.u32(0);
    w.align(4);
    w.u32(2);
    w.u32(0);
    w.u32(9620 * 2);
    w.u32(0);
    w.u32(0xDEADBEEF);
    w.u32(0xDEADBEEF);
    w.f32(3000.0F);
    w.align(16);
    placement(w, {10.0F, 20.0F, 30.0F}, 0, 3000.0F, 0.0F);
    placement(w, {-5.0F, 1.0F, 2.0F}, 1, 1500.0F, 0.2F);
    finish(w);
    return w.data;
}

TEST_CASE("readsPlacementSets", "[animated]")
{
    const animated::AnimatedPlacementSet set = animated::readPlacementSet(placementSet(), "test");
    CHECK(set.name == std::string("anim_proc_test"));
    CHECK(!set.eventProp);
    CHECK(set.placements.size() == std::size_t{2});
    if (set.placements.size() != 2) {
        return;
    }
    const animated::AnimatedPlacement& first = set.placements[0];
    CHECK(first.objectSet == 7U);
    CHECK_THAT(first.position.z, WithinAbs(30.0F, 1e-6F));
    CHECK_THAT(first.axes[0].z, WithinAbs(-1.0F, 1e-6F));
    CHECK_THAT(first.axes[2].x, WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(first.lodDistances[1], WithinAbs(1800.0F, 1e-3F));
    CHECK_THAT(first.cullDistance, WithinAbs(3000.0F, 1e-3F));
    CHECK(first.ownInstance);
    CHECK(first.triggerDistance < 0.0F);
    const animated::AnimatedPlacement& second = set.placements[1];
    CHECK(second.objectSet == 32U);
    CHECK_THAT(second.nightThreshold, WithinAbs(0.2F, 1e-6F));

    Bytes truncated = placementSet();
    truncated.resize(truncated.size() - 4);
    CHECK_THROWS_AS(animated::readPlacementSet(truncated, "test"), std::exception);
}

/// A kind-4 set of one entry with one rigid batch of one triangle, with a
/// stand-in for the Granny file.
Bytes objectSet(std::uint32_t grannyBytes)
{
    Writer w = setHeader(animated::kObjectSetKind, "Anim_Test", 1, 0);
    w.align(4);
    w.u32(11101);
    w.u32(0);
    w.align(16);
    const std::size_t h = w.data.size();
    for (float v : {-1.0F, 0.0F, -2.0F, 0.0F, 1.0F, 3.0F, 2.0F, 0.0F}) {
        w.f32(v);
    }
    w.zeros(96 - 32);
    w.put32(h + 36, grannyBytes);
    w.put32(h + 60, 1);
    w.put32(h + 68, 1);
    w.align(4);
    w.u32(8);
    w.text("tex_d");
    w.zeros(3);
    w.align(4);
    // The entry: one opaque draw, bone count 4, one batch at level 0.
    w.u32(1);
    w.u32(0);
    w.u32(4);
    w.u32(1);
    w.u32(0);
    w.u32(1);
    w.u32(0xABCD);
    w.zeros(32);
    w.align(4);
    // The batch.
    const std::size_t batch = w.data.size();
    w.u32(2);
    w.u32(3);
    w.u32(3);
    w.u32(1);
    w.u32(0);
    w.u32(1);
    w.u32(0);
    w.u32(0);
    w.zeros(68 - 32);
    w.put32(batch + 44, 0x1234);
    w.put32(batch + 52, 0x5678);
    w.put32(batch + 64, 5);
    w.align(4);
    // The draw.
    w.u32(0);
    w.u32(0xFFFFFFFF);
    w.u32(0xFFFFFFFF);
    w.u32(0xFFFFFFFF);
    w.u32(0);
    w.u32(1);
    w.u32(0);
    w.u32(1);
    w.u32(0);
    w.f32(75.0F);
    w.f32(0.01F);
    w.align(4);
    const std::size_t granny = w.data.size();
    w.zeros(grannyBytes);
    w.align(4);
    const std::size_t gpu = w.data.size();
    w.u16(0);
    w.u16(1);
    w.u16(2);
    w.align(4);
    w.zeros(3 * 12);
    w.put32(h + 40, static_cast<std::uint32_t>(w.data.size() - gpu));
    (void)granny;
    finish(w);
    return w.data;
}

TEST_CASE("readsObjectSets", "[animated]")
{
    const animated::AnimatedObjectSet set = animated::readObjectSet(objectSet(8), "test");
    CHECK(set.name == std::string("Anim_Test"));
    CHECK(set.textureIds.size() == std::size_t{1});
    CHECK(set.textureNames.size() == std::size_t{1});
    if (set.textureNames.size() == 1) {
        CHECK(set.textureNames[0] == std::string("tex_d"));
    }
    CHECK_THAT(set.boundsMax.y, WithinAbs(3.0F, 1e-6F));
    CHECK(set.entries.size() == std::size_t{1});
    if (set.entries.size() != 1 || set.entries[0].lods[0].size() != 1) {
        CHECK(false);
        return;
    }
    CHECK(set.entries[0].boneCount == 4U);
    CHECK(set.entries[0].lods[1].empty());
    const animated::AnimatedObjectBatch& batch = set.entries[0].lods[0][0];
    CHECK(batch.bone == 2);
    CHECK(batch.vertexType == 5U);
    CHECK(animated::AnimatedObjectSet::vertexStride(batch.vertexType) == 12U);
    CHECK(batch.opaque.size() == std::size_t{1});
    if (batch.opaque.size() == 1) {
        CHECK(batch.opaque[0].textures[0] == 0);
        CHECK(batch.opaque[0].textures[1] == -1);
        CHECK(batch.opaque[0].triangleCount == 1U);
        CHECK(batch.opaque[0].flags[1] == 1U);
        CHECK_THAT(batch.opaque[0].specularPower, WithinAbs(75.0F, 1e-6F));
    }
    CHECK(set.grannySize == std::size_t{8});
    // Indices first in the GPU block, the vertices 4-byte aligned after.
    CHECK(batch.indexOffset == set.grannyOffset + 8);
    CHECK(batch.vertexOffset == batch.indexOffset + 8);
    CHECK(fh::loadBe16(set.data.data() + batch.indexOffset + 4) == 2);

    // A Granny block that does not leave the GPU block its size.
    Bytes wrong = objectSet(8);
    wrong[0x80 + 36 + 3] = 12;
    CHECK_THROWS_AS(animated::readObjectSet(wrong, "test"), std::exception);
}

granny::Curve spline(int degree, std::vector<float> knots, std::vector<float> controls, int dimension)
{
    granny::Curve curve;
    curve.kind = granny::Curve::Kind::Spline;
    curve.degree = degree;
    curve.dimension = dimension;
    curve.knots = std::move(knots);
    curve.controls = std::move(controls);
    return curve;
}

TEST_CASE("samplesCurves", "[animated]")
{
    float out[4] = {};
    // Degree 1: straight lines between the controls.
    const granny::Curve line = spline(1, {0.0F, 1.0F, 3.0F}, {0.0F, 10.0F, 30.0F}, 1);
    CHECK(line.sample(0.5F, 3.0F, false, out));
    CHECK_THAT(out[0], WithinAbs(5.0F, 1e-5F));
    CHECK(line.sample(2.0F, 3.0F, false, out));
    CHECK_THAT(out[0], WithinAbs(20.0F, 1e-5F));
    // At the last knot, its control.
    CHECK(line.sample(3.0F, 3.0F, false, out));
    CHECK_THAT(out[0], WithinAbs(30.0F, 1e-5F));

    // Degree 2: the weights sum to one, so equal controls give their value
    // wherever the curve is sampled, looping or not.
    const granny::Curve flat = spline(2, {0.0F, 0.5F, 1.0F, 2.0F}, {3.0F, 3.0F, 3.0F, 3.0F}, 1);
    for (float t : {0.0F, 0.25F, 0.75F, 1.5F, 1.999F}) {
        CHECK(flat.sample(t, 2.0F, true, out));
        CHECK_THAT(out[0], WithinAbs(3.0F, 1e-5F));
    }
    // Between knots 1 and 2 the quadratic weights of 0x82A84018: with
    // knots 0, 1, 2, 3 at t = 1.5, a = 0.75, b = 0.5, c = 0.25.
    const granny::Curve ramp = spline(2, {0.0F, 1.0F, 2.0F, 3.0F}, {0.0F, 1.0F, 2.0F, 3.0F}, 1);
    CHECK(ramp.sample(1.5F, 3.0F, false, out));
    const float a = 0.75F;
    const float b = 0.5F;
    const float c = 0.25F;
    CHECK_THAT(
        out[0], WithinAbs((1.0F - a) * (1.0F - b) * 0.0F + (a + b - a * b - b * c) * 1.0F + b * c * 2.0F, 1e-5F));

    // A looping curve's last knot is its first a duration later: just
    // before the end it approaches the start's value.
    const granny::Curve cycle = spline(1, {0.0F, 1.0F, 2.0F}, {0.0F, 10.0F, 0.0F}, 1);
    CHECK(cycle.sample(1.999F, 2.0F, true, out));
    CHECK_THAT(out[0], WithinAbs(0.01F, 1e-3F));

    // Identity and keyframe curves leave the value to the caller.
    granny::Curve identity;
    out[0] = 42.0F;
    CHECK(!identity.sample(0.5F, 1.0F, true, out));
    CHECK_THAT(out[0], WithinAbs(42.0F, 0.0F));
}

TEST_CASE("composesGrannyTransforms", "[animated]")
{
    granny::Transform t;
    t.flags = granny::Transform::kHasPosition | granny::Transform::kHasOrientation;
    t.position = {1.0F, 2.0F, 3.0F};
    t.orientation = glm::angleAxis(std::numbers::pi_v<float> / 2.0F, glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::vec4 p = t.matrix() * glm::vec4(1.0F, 0.0F, 0.0F, 1.0F);
    CHECK_THAT(p.x, WithinAbs(1.0F, 1e-5F));
    CHECK_THAT(p.z, WithinAbs(2.0F, 1e-5F));
    // Without the flags the stored parts are ignored.
    t.flags = 0;
    const glm::vec4 q = t.matrix() * glm::vec4(1.0F, 0.0F, 0.0F, 1.0F);
    CHECK_THAT(q.x, WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(q.z, WithinAbs(0.0F, 1e-6F));

    granny::Model model;
    model.initialPlacement.flags = granny::Transform::kHasPosition;
    model.initialPlacement.position = {0.0F, 10.0F, 0.0F};
    granny::Bone root;
    granny::Bone child;
    child.parent = 0;
    child.local.flags = granny::Transform::kHasPosition;
    child.local.position = {0.0F, 0.0F, 1.0F};
    model.bones = {root, child};
    std::vector<glm::mat4> world;
    granny::worldPose(model, {}, world);
    CHECK_THAT(world[1][3].y, WithinAbs(10.0F, 1e-6F));
    CHECK_THAT(world[1][3].z, WithinAbs(1.0F, 1e-6F));
}

/// Colorado's animated objects, loaded once for the disc tests.
const animated::AnimatedObjects* colorado()
{
    static std::unique_ptr<animated::AnimatedObjects> loaded;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const char* dir = gameDir();
        if (dir == nullptr) {
            return nullptr;
        }
        fh::Vfs vfs;
        vfs.mountDisc(dir);
        const auto archive = vfs.archiveAt(fh::world::TrackPaths::of("colorado").archive);
        if (!archive) {
            CHECK(false);
            return nullptr;
        }
        if (auto objects = animated::AnimatedObjects::load(*archive)) {
            loaded = std::make_unique<animated::AnimatedObjects>(std::move(*objects));
        }
    }
    return loaded.get();
}

const animated::AnimatedObject* objectOfSet(const animated::AnimatedObjects& objects, std::uint32_t set)
{
    for (const animated::AnimatedObject& object : objects.objects()) {
        if (object.setNumber == set) {
            return &object;
        }
    }
    return nullptr;
}

/// The angle in degrees between two orientations.
float degreesBetween(const glm::quat& a, const glm::quat& b)
{
    const float d = std::min(1.0F, std::abs(glm::dot(a, b)));
    return 2.0F * std::acos(d) * 180.0F / std::numbers::pi_v<float>;
}

TEST_CASE("loadsColoradosAnimatedObjects", "[animated]")
{
    const animated::AnimatedObjects* objects = colorado();
    if (objects == nullptr) {
        return;
    }
    // 33 kind-4 sets, placed 455 times by 202 kind-5 sets.
    CHECK(objects->objects().size() == std::size_t{33});
    CHECK(objects->failedSets() == 0);
    CHECK(objects->instances().size() == std::size_t{455});
    CHECK(objects->missingObjects() == 0);
    std::map<std::uint32_t, int> perSet;
    for (const animated::AnimatedObjects::Instance& instance : objects->instances()) {
        ++perSet[objects->objects()[instance.object].setNumber];
    }
    // The metal and wooden windmills, the wind turbines and the ferris wheel.
    CHECK(perSet[7] == 8);
    CHECK(perSet[8] == 11);
    CHECK(perSet[32] == 8);
    CHECK(perSet[14] == 1);

    const animated::AnimatedObject* windmill = objectOfSet(*objects, 7);
    CHECK(windmill != nullptr);
    if (windmill == nullptr) {
        return;
    }
    CHECK(windmill->set.name == std::string("Anim_ANIM_FARM_WindmillMetal31"));
    CHECK(windmill->set.textureIds == std::vector<std::uint32_t>{11101});
    CHECK(windmill->set.entries.size() == std::size_t{3});
    CHECK(windmill->granny.animations.size() == std::size_t{1});
    // The head: three rigid batches on the flap, the body and the fan.
    const auto& head = windmill->set.entries[0].lods[0];
    CHECK(head.size() == std::size_t{3});
    if (head.size() == 3) {
        CHECK(windmill->granny.models[0].bones[static_cast<std::size_t>(head[2].bone)].name == std::string("Fan"));
        CHECK(head[2].vertexCount == 350U);
    }
    // The fan turns about its hub at 225 degrees a second.
    const int fan = windmill->granny.models[0].findBone("Fan");
    CHECK(fan >= 0);
    if (fan >= 0) {
        std::vector<granny::Transform> early;
        std::vector<granny::Transform> late;
        animated::AnimatedObjects::sampleLocals(*windmill, 0, 0, 2.0F, early);
        animated::AnimatedObjects::sampleLocals(*windmill, 0, 0, 2.4F, late);
        const auto bone = static_cast<std::size_t>(fan);
        CHECK_THAT(degreesBetween(early[bone].orientation, late[bone].orientation), WithinAbs(90.0F, 1.5F));
        // The hub stays where it is.
        CHECK_THAT(glm::length(early[bone].position - late[bone].position), WithinAbs(0.0F, 1e-5F));
    }

    // A windmill of the plains, placed where the set says.
    bool found = false;
    for (const animated::AnimatedObjects::Instance& instance : objects->instances()) {
        if (instance.object == static_cast<std::uint32_t>(windmill - objects->objects().data())
            && glm::length(instance.placement.position - glm::vec3(-670.54F, 87.36F, -2972.59F)) < 0.05F) {
            found = true;
            CHECK_THAT(instance.placement.cullDistance, WithinAbs(3000.0F, 1e-3F));
            CHECK_THAT(instance.placement.lodDistances[0], WithinAbs(500.0F, 1e-3F));
        }
    }
    CHECK(found);

    // The wind turbine's rotor turns once in ten seconds.
    const animated::AnimatedObject* turbine = objectOfSet(*objects, 32);
    CHECK(turbine != nullptr);
    if (turbine != nullptr) {
        const int hub = turbine->granny.models[0].findBone("Dummy001");
        CHECK(hub >= 0);
        if (hub >= 0) {
            std::vector<granny::Transform> a;
            std::vector<granny::Transform> b;
            animated::AnimatedObjects::sampleLocals(*turbine, 0, 0, 3.0F, a);
            animated::AnimatedObjects::sampleLocals(*turbine, 0, 0, 4.0F, b);
            const auto bone = static_cast<std::size_t>(hub);
            CHECK_THAT(degreesBetween(a[bone].orientation, b[bone].orientation), WithinAbs(36.0F, 1.0F));
        }
    }

    // The ferris wheel: a skinned wheel of 36 bones.
    const animated::AnimatedObject* ferris = objectOfSet(*objects, 14);
    CHECK(ferris != nullptr);
    if (ferris != nullptr) {
        CHECK(ferris->set.entries.size() == std::size_t{2});
        if (ferris->set.entries.size() == 2) {
            CHECK(ferris->set.entries[1].boneCount == 36U);
            CHECK(!ferris->set.entries[1].lods[0].empty());
            if (!ferris->set.entries[1].lods[0].empty()) {
                CHECK(ferris->set.entries[1].lods[0][0].bone == -1);
                CHECK(ferris->set.entries[1].lods[0][0].vertexCount == 13156U);
            }
        }
    }
}

TEST_CASE("playsOnThePresentationClock", "[animated]")
{
    const animated::AnimatedObjects* loaded = colorado();
    if (loaded == nullptr) {
        return;
    }
    animated::AnimatedObjects objects = *loaded;
    // Looking at the ferris wheel from 200 m south of it.
    const glm::vec3 wheel(-1084.47F, -1.51F, -614.58F);
    animated::AnimatedView view;
    view.camera = wheel + glm::vec3(0.0F, 20.0F, -200.0F);
    view.viewProjection = glm::perspective(1.0F, 16.0F / 9.0F, 0.5F, 5000.0F)
        * glm::lookAt(view.camera, wheel + glm::vec3(0.0F, 20.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    objects.update(0.0F, view);
    std::size_t ferris = objects.instances().size();
    for (const animated::AnimatedObjects::Visible& visible : objects.visible()) {
        if (glm::length(objects.instances()[visible.instance].placement.position - wheel) < 0.05F) {
            ferris = visible.instance;
            CHECK(visible.lod == 1);
        }
    }
    CHECK(ferris < objects.instances().size());
    if (ferris >= objects.instances().size()) {
        return;
    }
    CHECK(objects.clip(ferris) >= 0);
    const float start = objects.clipTime(ferris);
    objects.update(0.5F, view);
    CHECK_THAT(objects.clipTime(ferris) - start, WithinAbs(0.5F, 1e-4F));
    // Paused: the presentation's step is 0 and nothing moves.
    const std::vector<glm::mat4> before = objects.pose();
    objects.update(0.0F, view);
    CHECK_THAT(objects.clipTime(ferris) - start, WithinAbs(0.5F, 1e-4F));
    CHECK(objects.pose() == before);
    // Out of view it keeps its clock.
    animated::AnimatedView away = view;
    away.viewProjection = glm::perspective(1.0F, 16.0F / 9.0F, 0.5F, 5000.0F)
        * glm::lookAt(view.camera, view.camera + glm::vec3(0.0F, 0.0F, -1.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    objects.update(3.0F, away);
    CHECK_THAT(objects.clipTime(ferris) - start, WithinAbs(0.5F, 1e-4F));
    objects.update(0.25F, view);
    CHECK_THAT(objects.clipTime(ferris) - start, WithinAbs(0.75F, 1e-4F));
    // And the wheel's bones move as it plays.
    CHECK(objects.pose() != before);
}

/// Reads one file of media/animatedobjects through the engine's file system.
std::optional<animated::AnimatedObject> packObject(const fh::Vfs& vfs, const std::string& path)
{
    Bytes data = vfs.readOrThrow(path);
    return animated::AnimatedObject::make(0, animated::readObjectSet(std::move(data), path), path);
}

TEST_CASE("readsTheAnimatedObjectPack", "[animated]")
{
    const char* dir = gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    // A rigid vehicle, a skinned animal and a gameplay laser with its own
    // clips: one of each kind the pack holds.
    for (const char* name : {"anim_helicopter_001", "anim_cows_001", "anim_gply_laser_checkpoint"}) {
        const std::optional<animated::AnimatedObject> object
            = packObject(vfs, std::string("media/animatedobjects/") + name + ".pgeo");
        CHECK(object.has_value());
        if (object) {
            CHECK(!object->set.entries.empty());
            CHECK(!object->granny.animations.empty());
            CHECK(object->set.textureIds.empty());
            CHECK(!object->set.textureNames.empty());
        }
    }
}

TEST_CASE("readsEveryAnimatedObject", "[animated][slow][.]")
{
    const char* dir = gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    int files = 0;
    for (const std::string& path : vfs.list("media/animatedobjects")) {
        if (!fh::endsWithNoCase(path, ".pgeo")) {
            continue;
        }
        ++files;
        try {
            (void)packObject(vfs, path);
        } catch (const std::exception& e) {
            FAIL_CHECK(e.what());
        }
    }
    CHECK(files == 290);
}

} // namespace
