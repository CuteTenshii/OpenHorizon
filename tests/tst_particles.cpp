// Particle effects: the effect file reader, the tyre effect table, the
// particle simulation and the tyre effect rules on synthetic data, and with
// FH1_GAME_DIR set, every effect of the disc.

#include "TestHarness.h"

#include "particles/Effect.h"
#include "particles/GameRandom.h"
#include "particles/ParticleManager.h"
#include "particles/ParticleSystem.h"
#include "particles/SurfaceEffects.h"
#include "particles/TyreEffects.h"

#include "vfs/Vfs.h"
#include "world/XboxTexture.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdlib>
#include <numbers>
#include <string>

namespace particles = fh::particles;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

/// A soft smoke effect in the disc's format, version 13.
constexpr const char* kSmokeXml = R"(
<Effect Version="13" budget="10" lowerbudget="6">
  <TimeOfDayData startH="6" startM="30" endH="20" endM="0"/>
  <EmitterProperties type="SuperSpray">
    <OffAxis value="0.000000" variation="0.000000"/>
    <OffPlane value="0.000000" variation="0.000000"/>
    <Quantity value="30.000000" var="0.000000"/>
    <Velocity value="2.000000" variation="0.000000"/>
    <EmitTime start="0.000000" end="0.000000" infinite="1"/>
    <DisplayUntil value="4294967295"/>
    <Life value="1.000000" variation="0.000000"/>
    <Density Version="3" value="0.500000"/>
    <ParticleShaderType Version="3" value="_soft"/>
    <RenderInMirror value="1"/>
    <Size valueX="0.100000" valueY="0.200000" variationX="0.000000" variationY="0.000000" growforX="0.500000" growforY="0.000000" shrinkforX="0.000000" shrinkforY="0.000000" maxValueX="1.000000" maxValueY="2.000000" endValueX="0.000000" endValueY="0.000000" growFrom="0.000000" shrinkTo="0.000000"/>
    <SpinTime value="2.000000" variation="0.000000"/>
    <SpinPhase value="90.000000" variation="0.000000"/>
    <SpinAxis type="DirectionOfTravel" alternate="0"/>
    <LockTo camera="0"/>
    <SurfaceEffectId type="0"/>
  </EmitterProperties>
  <Behavior>
    <MotionInheritance influence="100" multiplier="0.500000" offset="0.000000" variation="0.000000"/>
    <Gravity strength="-0.500000" decay="0.000000"/>
    <Friction strength="0.650000" startFrom="0.000000" velocityKept="3.200000"/>
    <ParticleSpawn type="None"/>
  </Behavior>
  <Render>
    <ParticleType value="BillboardedQuad" dynamic_lighting="1" zsort="1" max_screen_size="0.080000" forceEdgeOnFadeOff="1">
      <light_influence v.x="0.100000" v.y="2.000000" v.z="2.700000" v.w="1.000000"/>
      <HeadLightPower value="1.250000"/>
      <specular_light_influence v.x="0.000000" v.y="1.000000" v.z="0.300000" v.w="0.000000"/>
      <Texture value="GR_Puffy_Smoke_DIFF.xds"/>
      <Texture2 value=""/>
      <Material>
        <BlendType value="alpha"/>
        <RGBStart r="0.500000" g="1.000000" b="0.200000" a="0.600000"/>
        <RGBEnd r="0.100000" g="0.100000" b="0.100000" a="0.000000"/>
        <NearFade nearFade="0.000000"/>
        <FadeInTo fadeInTo="0.150000"/>
        <FadeOutFrom fadeOutFrom="0.560000"/>
        <RGBMid r="1.000000" g="0.500000" b="1.000000" a="0.400000"/>
      </Material>
    </ParticleType>
  </Render>
</Effect>)";

particles::ParticleManager smokeManager(std::string_view xml = kSmokeXml)
{
    std::vector<particles::EffectDefinition> definitions;
    definitions.push_back(particles::parseEffect(xml, "smoke"));
    return {std::move(definitions), {{"Smoke", "Smoke"}}};
}

} // namespace

TEST(randomMatchesTheGamesGenerator)
{
    // seed * 0x41C64E6D + 12345, bits 16-30.
    particles::GameRandom r(1);
    CHECK_EQ(r.next15(), 16838U);
    CHECK_EQ(r.next15(), 5758U);
    CHECK_EQ(r.next15(), 10113U);
    for (int i = 0; i < 1000; ++i) {
        const float u = r.uniform();
        CHECK(u >= 0.0F && u <= 1.0F);
        const float v = r.range(-2.0F, 3.0F);
        CHECK(v >= -2.0F && v <= 3.0F);
    }
}

TEST(effectFileIsReadLikeTheGame)
{
    const particles::EffectDefinition e = particles::parseEffect(kSmokeXml, "smoke");
    CHECK_EQ(e.version, 13);
    CHECK_EQ(e.budget, 10U);
    CHECK_EQ(e.lowerBudget, 6U);
    CHECK_EQ(e.timeOfDay[0], 6);
    CHECK_EQ(e.timeOfDay[1], 30);
    CHECK(e.shape == particles::EmitterShape::SuperSpray);
    CHECK_NEAR(e.quantity.value, 30.0, 1e-6);
    CHECK_NEAR(e.velocity.value, 2.0, 1e-6);
    CHECK(e.soft());
    CHECK(!e.animated());
    CHECK_NEAR(e.density, 0.5, 1e-6);
    // Degrees become radians for the phase value only.
    CHECK_NEAR(e.spinPhase.value, kPi / 2.0, 1e-6);
    CHECK(e.spinAxis == particles::SpinAxis::DirectionOfTravel);
    CHECK_EQ(e.behaviours.size(), std::size_t{2});
    CHECK(e.behaviours[0].kind == particles::Behaviour::Kind::Gravity);
    CHECK(e.behaviours[1].kind == particles::Behaviour::Kind::Friction);
    CHECK_NEAR(e.behaviours[1].friction.velocityKept, 3.2, 1e-6);
    CHECK_EQ(e.motionInheritance.influence, 100);
    CHECK(e.render.type == particles::ParticleType::BillboardedQuad);
    CHECK(e.render.zSort);
    CHECK(e.render.forceEdgeOnFadeOff);
    CHECK_NEAR(e.render.lightInfluence.x, 0.1, 1e-6);
    CHECK_NEAR(e.render.headLightPower, 1.25, 1e-6);
    // Colours are squared, alpha kept.
    const particles::Material& m = e.render.material;
    CHECK_NEAR(m.rgbStart.r, 0.25, 1e-6);
    CHECK_NEAR(m.rgbStart.b, 0.04, 1e-6);
    CHECK_NEAR(m.rgbStart.a, 0.6, 1e-6);
    CHECK_NEAR(m.rgbMid.g, 0.25, 1e-6);
    CHECK_NEAR(m.rgbEnd.r, 0.01, 1e-6);
    CHECK_NEAR(m.fadeInTo, 0.15, 1e-6);
    CHECK_NEAR(m.fadeOutFrom, 0.56, 1e-6);
    CHECK_EQ(e.render.texture, std::string("GR_Puffy_Smoke_DIFF.xds"));
    CHECK_NEAR(e.maxLife(), 1.0, 1e-6);
}

TEST(olderVersionsFollowTheLoadersRules)
{
    // Version 6: no fades (the RGBMid
    // element is not read; mid is the mean of start and end before
    // squaring), and a non-soft shader clears the density.
    const std::string xml = R"(<Effect Version="6" budget="11">
      <EmitterProperties type="Plane">
        <Size length="2.0" width="3.0" valueX="1" valueY="1" maxValueX="1" maxValueY="1"/>
        <Density value="4.0"/>
        <ParticleShaderType value="_animated_rowonly"/>
        <Animation rate="2.0" numFramesX="4" numFramesY="2"/>
        <OffAxis value="45" variation="10"/>
      </EmitterProperties>
      <Behavior><MotionInheritance influence="0"/></Behavior>
      <Render><ParticleType value="Quad" dynamic_lighting="0">
        <Material><BlendType value="additive"/>
          <RGBStart r="0.2" g="0.4" b="0.6" a="1"/><RGBEnd r="0.6" g="0.8" b="1.0" a="0"/>
          <RGBMid r="1" g="1" b="1" a="1"/><FadeInTo fadeInTo="0.9"/>
        </Material></ParticleType></Render></Effect>)";
    const particles::EffectDefinition e = particles::parseEffect(xml, "old");
    // Version 1 files have no lower budget: half the budget.
    CHECK_EQ(particles::parseEffect(R"(<Effect Version="1" budget="11"/>)", "v1").lowerBudget, 5U);
    CHECK(e.shape == particles::EmitterShape::Plane);
    CHECK_NEAR(e.size.box.x, 2.0, 1e-6);
    CHECK_NEAR(e.size.box.z, 3.0, 1e-6);
    CHECK_NEAR(e.offAxis.value, 0.0, 1e-6);
    CHECK(e.animated() && e.rowOnly() && !e.soft());
    CHECK_NEAR(e.density, 0.0, 1e-6);
    CHECK_EQ(e.framesX, 4);
    CHECK_EQ(e.framesY, 2);
    const particles::Material& m = e.render.material;
    CHECK(m.blend == particles::BlendType::Additive);
    CHECK_NEAR(m.rgbMid.r, 0.16, 1e-6);
    CHECK_NEAR(m.rgbMid.g, 0.36, 1e-6);
    CHECK_NEAR(m.rgbMid.a, 0.5, 1e-6);
    CHECK_NEAR(m.fadeInTo, 0.5, 1e-6);
    // Unlit particles do not blend in any lighting.
    CHECK_NEAR(e.render.lightInfluence.w, 0.0, 1e-6);
    CHECK_THROWS(particles::parseEffect("<Track/>", "bad"));
}

TEST(effectListAndComposites)
{
    const auto list = particles::parseSystemList(
        R"(<Track><SystemEffect file="Dirt1" system_name="Dirt"/><SystemEffect file="_Composite_Fountain" system_name="_Composite_Fountain"/></Track>)");
    CHECK_EQ(list.size(), std::size_t{2});
    CHECK_EQ(list[0].systemName, std::string("Dirt"));
    CHECK_EQ(particles::effectPath(list[0].file), std::string("media/effects/dirt1.xml"));
    CHECK(particles::isCompositeName(list[1].file));
    const particles::CompositeDefinition c = particles::parseComposite(
        R"(<Composite Version="2"><RepeatPeriod time="4"/><Effect><EmitterName name="a"/><FileName name="b"/><PosOffset v.x="1" v.y="2" v.z="3" v.w="0"/><StartOffset time="0.5"/><FinishOffset time="1.5"/></Effect></Composite>)",
        "c");
    CHECK_NEAR(c.repeatPeriod, 4.0, 1e-6);
    CHECK_EQ(c.members.size(), std::size_t{1});
    CHECK_NEAR(c.members[0].positionOffset.y, 2.0, 1e-6);
    CHECK_NEAR(c.members[0].finishOffset, 1.5, 1e-6);
}

TEST(surfaceWeightsComeFromSkidData)
{
    const auto table = particles::SurfaceEffectTable::parse(R"(<Root>
      <SurfaceTypesList><SurfaceType value="Asphalt"/><SurfaceType value="Gravel"/><SurfaceType value="Kerb"/><SurfaceType value="SmashableWood"/></SurfaceTypesList>
      <SurfaceTypes>
        <Asphalt><SkidData><MinIntensity value="0.0"/><SmokeType Smoke="1" Dust="0" Grass="0" Dirt="0"/></SkidData></Asphalt>
        <Gravel><SkidData><MinIntensity value="0.4"/><SmokeType Smoke="0" Dust="0" Grass="0" Dirt="0" GravelBits="1"/></SkidData></Gravel>
        <SmashableWood><SmokeType Smoke="0" Dust="1"/></SmashableWood>
      </SurfaceTypes></Root>)");
    CHECK_EQ(table.surfaces().size(), std::size_t{4});
    CHECK_NEAR(table.find(0)->weight(particles::TyreChannel::Smoke), 1.0, 1e-6);
    CHECK_NEAR(table.find(1)->weight(particles::TyreChannel::GravelBits), 1.0, 1e-6);
    CHECK_NEAR(table.find(1)->minIntensity, 0.4, 1e-6);
    CHECK(!table.find(2)->hasSkidData);
    // SmokeType outside SkidData is not the tyre table's.
    CHECK_NEAR(table.find(3)->weight(particles::TyreChannel::Dust), 0.0, 1e-6);
    CHECK(table.find(4) == nullptr);
    CHECK_EQ(table.idOf("Gravel").value_or(99), std::size_t{1});
    CHECK_EQ(particles::tyreChannelName(particles::TyreChannel::DustBumper3), std::string_view("DustBumper3"));
}

TEST(rotationsFollowXnaConventions)
{
    const glm::mat3 x = particles::rotationX(0.3F);
    CHECK_NEAR(x[1].y, std::cos(0.3), 1e-6);
    CHECK_NEAR(x[1].z, std::sin(0.3), 1e-6);
    // A roll of 90 degrees takes x to y; a yaw of 90 degrees takes x to -z.
    const glm::vec3 rolled = particles::transformRow(glm::vec3(1, 0, 0), particles::rollPitchYaw(0, 0, kPi / 2));
    CHECK_NEAR(rolled.y, 1.0, 1e-6);
    const glm::vec3 yawed = particles::transformRow(glm::vec3(1, 0, 0), particles::rollPitchYaw(0, kPi / 2, 0));
    CHECK_NEAR(yawed.z, -1.0, 1e-6);
    const glm::mat3 n = particles::rotationNormal(glm::vec3(1, 0, 0), 0.7F);
    const glm::mat3 r = particles::rotationX(0.7F);
    for (int i = 0; i < 3; ++i) {
        CHECK_NEAR(glm::length(n[i] - r[i]), 0.0, 1e-6);
    }
}

TEST(emissionRateAndBudget)
{
    particles::ParticleManager manager = smokeManager();
    particles::ParticleSystem& system = *manager.find("Smoke");
    particles::EmitRequest request;
    request.direction = glm::vec3(0.0F, 0.0F, 1.0F);
    // No emission before the first update: the rate is per the last step.
    manager.emit(system, request);
    CHECK_EQ(system.size(), std::size_t{0});
    manager.update(0.1F);
    manager.emit(system, request);
    CHECK_EQ(system.size(), std::size_t{3});
    for (int i = 0; i < 5; ++i) {
        manager.emit(system, request);
    }
    // The ring holds at most the budget.
    CHECK_EQ(system.size(), std::size_t{10});
    // Particles free their slots after the longest life.
    for (int i = 0; i < 11; ++i) {
        manager.update(0.1F);
    }
    CHECK_EQ(system.size(), std::size_t{0});
}

TEST(sprayDirectionAndSpawnedState)
{
    particles::ParticleManager manager = smokeManager();
    particles::ParticleSystem& system = *manager.find("Smoke");
    manager.update(1.0F / 30.0F);
    // Headings are kept for horizontal directions, as the game's emitter
    // angles work out.
    const glm::vec3 directions[]
        = {glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1), glm::vec3(-0.6F, 0, 0.8F)};
    for (const glm::vec3& d : directions) {
        system.clear();
        particles::EmitRequest request;
        request.direction = d;
        manager.emit(system, request);
        CHECK_EQ(system.size(), std::size_t{1});
        const particles::Particle& p = system.at(0);
        // Velocity 2 m/s along the direction; motion inheritance adds
        // nothing without an inherited velocity. The asin polynomial is
        // approximate.
        CHECK(glm::length(p.velocity / 2.0F - d) < 2e-3F);
        CHECK(p.alive);
        CHECK_NEAR(p.age, 0.0, 1e-6);
        CHECK_NEAR(p.life, 1.0, 1e-6);
        CHECK_NEAR(p.maxSize.x, 1.0, 1e-6);
        // Grows, so it starts at valueX; does not grow in y.
        CHECK_NEAR(p.size.x, 0.1, 1e-6);
        CHECK_NEAR(p.size.y, 2.0, 1e-6);
        CHECK_NEAR(p.spinRate, kPi / 2.0, 1e-5);
        CHECK(p.random >= 0.0F && p.random <= 255.0F);
    }
}

TEST(behavioursAndSizeCurve)
{
    particles::ParticleManager manager = smokeManager();
    particles::ParticleSystem& system = *manager.find("Smoke");
    const float dt = 1.0F / 30.0F;
    manager.update(dt);
    particles::EmitRequest request;
    request.direction = glm::vec3(0.0F, 0.0F, 1.0F);
    request.inheritVelocity = glm::vec3(10.0F, 0.0F, 0.0F);
    manager.emit(system, request);
    CHECK_EQ(system.size(), std::size_t{1});
    const glm::vec3 v0 = system.at(0).velocity;
    // Full inheritance chance at half the emitter velocity.
    CHECK_NEAR(v0.x, 5.0, 1e-4);
    manager.update(dt);
    const particles::Particle& p = system.at(0);
    // Gravity -0.5 adds 0.5 * dt^2 upwards, then friction scales by
    // 1 - 0.65 * speed * dt (speed above 3.2 m/s).
    glm::vec3 expected = v0;
    expected.y += 0.5F * dt * dt;
    expected *= 1.0F - 0.65F * glm::length(expected) * dt;
    CHECK(glm::length(p.velocity - expected) < 1e-5F);
    CHECK_NEAR(p.age, dt, 1e-6);
    // Half way through the growth window: t = life/maxLife * age.
    for (int i = 0; i < 7; ++i) {
        manager.update(dt);
    }
    const float t = system.at(0).age - dt;
    CHECK_NEAR(system.at(0).size.x, 0.1 + 0.9 * (t / 0.5), 1e-4);
}

TEST(skidIntensityFollowsTheFrictionCircle)
{
    particles::SurfaceEffects surface;
    surface.minIntensity = 0.4F;
    particles::TyreWheelInput wheel;
    wheel.normalisedLoad = 1.3F;
    wheel.frictionCircle = 0.5F;
    CHECK_NEAR(particles::skidIntensity(wheel, surface), 0.0, 1e-6);
    wheel.frictionCircle = 2.5F;
    CHECK_NEAR(particles::skidIntensity(wheel, surface), 1.0, 1e-6);
    wheel.frictionCircle = (0.95F + 2.0F) * 0.5F;
    CHECK_NEAR(particles::skidIntensity(wheel, surface), 0.5, 1e-5);
    // Lighter wheels lower the thresholds and the weight.
    wheel.normalisedLoad = 0.5F;
    wheel.frictionCircle = 1.5F * 2.0F;
    CHECK_NEAR(particles::skidIntensity(wheel, surface), 0.8 * 0.45, 1e-5);
    wheel.normalisedLoad = 0.0F;
    CHECK_NEAR(particles::skidIntensity(wheel, surface), 0.0, 1e-6);
    // The surface floor applies while the wheel moves and turns.
    wheel.wheelVelocity = glm::vec3(1.0F, 0.0F, 0.0F);
    wheel.wheelRpm = 10.0F;
    CHECK_NEAR(particles::skidIntensity(wheel, surface), 0.4, 1e-6);
}

TEST(tyreSmokeRules)
{
    particles::ParticleManager manager = smokeManager();
    const auto surfaces = particles::SurfaceEffectTable::parse(R"(<Root>
      <SurfaceTypesList><SurfaceType value="Asphalt"/></SurfaceTypesList>
      <SurfaceTypes><Asphalt><SkidData><MinIntensity value="0.0"/><SmokeType Smoke="1"/></SkidData></Asphalt></SurfaceTypes></Root>)");
    particles::TyreEffects tyres(manager, surfaces);
    particles::ParticleSystem& smoke = *manager.find("Smoke");
    manager.update(0.1F);

    particles::TyreCarInput car;
    for (particles::TyreWheelInput& w : car.wheels) {
        w.onGround = true;
        w.surface = 0;
    }
    // The smoke needs slide above 0.3.
    car.wheels[2].slide = 0.3F;
    tyres.emit(car);
    CHECK_EQ(smoke.size(), std::size_t{0});
    car.wheels[2].slide = 1.0F;
    tyres.emit(car);
    CHECK_EQ(smoke.size(), std::size_t{3});
    // It fades out between 8 and 10 m/s of car speed.
    smoke.clear();
    car.velocity = glm::vec3(0.0F, 0.0F, 10.0F);
    tyres.emit(car);
    CHECK_EQ(smoke.size(), std::size_t{0});
    // Wheels far from the car are ignored.
    car.velocity = glm::vec3(0.0F);
    car.wheels[2].frame.position = glm::vec3(0.0F, 0.0F, 7.0F);
    tyres.emit(car);
    CHECK_EQ(smoke.size(), std::size_t{0});
}

TEST(discEffects)
{
    const char* gameDir = std::getenv("FH1_GAME_DIR");
    if (gameDir == nullptr || *gameDir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(gameDir);

    const particles::ParticleManager manager = particles::ParticleManager::load(vfs);
    CHECK_EQ(manager.systems().size(), std::size_t{208});
    CHECK_EQ(manager.skipped().size(), std::size_t{17});

    // Every single effect file of the folder reads.
    std::size_t effects = 0;
    for (const std::string& path : vfs.list(particles::kEffectsFolder)) {
        if (!path.ends_with(".xml") || path.ends_with("defaulttrack.xml")) {
            continue;
        }
        const std::string file = path.substr(particles::kEffectsFolder.size() + 1);
        const std::string text = vfs.readText(path).value_or("");
        if (particles::isCompositeName(file)) {
            CHECK(!particles::parseComposite(text, file).members.empty());
        } else {
            particles::parseEffect(text, file);
            ++effects;
        }
    }
    CHECK_EQ(effects, std::size_t{241});

    const particles::ParticleSystem* smoke = manager.find("Smoke");
    CHECK(smoke != nullptr);
    if (smoke != nullptr) {
        const particles::EffectDefinition& d = smoke->definition();
        CHECK_EQ(d.budget, 180U);
        CHECK_NEAR(d.quantity.value, 20.0, 1e-5);
        CHECK_NEAR(d.life.value, 1.600001, 1e-5);
        CHECK(d.soft());
        CHECK_NEAR(d.size.maxValue.x, 1.0, 1e-6);
        CHECK_EQ(d.render.texture, std::string("GR_Puffy_Smoke_DIFF.xds"));
    }
    const particles::ParticleSystem* dirt = manager.find("Dirt");
    CHECK(dirt != nullptr && dirt->definition().animated() && dirt->definition().rowOnly()
        && dirt->definition().gradient());

    const auto table = particles::SurfaceEffectTable::load(vfs);
    CHECK_EQ(table.surfaces().size(), std::size_t{68});
    const particles::SurfaceEffects* asphalt = table.find(table.idOf("Asphalt").value_or(999));
    CHECK(asphalt != nullptr && asphalt->weight(particles::TyreChannel::Smoke) == 1.0F);
    const particles::SurfaceEffects* dirtSurface = table.find(table.idOf("Dirt").value_or(999));
    CHECK(dirtSurface != nullptr && dirtSurface->weight(particles::TyreChannel::Dust) == 0.5F
        && dirtSurface->weight(particles::TyreChannel::Dirt) == 1.0F && dirtSurface->minIntensity == 0.4F);

    // The smoke texture decodes.
    const auto texture = vfs.read("media/effects/gr_puffy_smoke_diff.xds");
    CHECK(texture.has_value());
    if (texture) {
        const fh::world::RgbaImage image = fh::world::decodeXboxTexture(*texture);
        CHECK(image.width > 0 && image.height > 0);
    }
}

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
