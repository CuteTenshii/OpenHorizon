// The track's shading inputs: XPR2 and CAFF cube textures, TrackSettings.xml and
// the scene constants.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "base/Bytes.h"
#include "base/Error.h"
#include "base/Host.h"
#include "game/TimeOfDay.h"
#include "render/TrackPrograms.h"
#include "render/WorldRenderer.h"
#include "vfs/Vfs.h"
#include "world/Archive.h"
#include "world/TrackSettings.h"
#include "world/TrackWorld.h"
#include "world/XboxCube.h"

#include <lzx/zip.h>
#include <xds/xds.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

using namespace openhorizon;

constexpr std::uint32_t kDxt1 = 0x12;
constexpr std::uint32_t kDxt5 = 0x14;
/// Byte order 1 (8 in 16) in the format word, as the disc's cubes have it.
constexpr std::uint32_t kSwap8In16 = 1U << 6;

/// The disc; the case skips when OPENHORIZON_GAME_DIR is not set.
std::unique_ptr<Vfs> disc()
{
    auto vfs = std::make_unique<Vfs>();
    vfs->mountDisc(pathFromUtf8(openhorizon::test::gameDir()));
    return vfs;
}

/// Average of red, green and blue over a level decoded to RGBA.
double brightness(const world::TextureSurface& surface)
{
    const world::RgbaImage image = world::decodeSurface(surface);
    double sum = 0.0;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        sum += image.rgba[i] + image.rgba[i + 1] + image.rgba[i + 2];
    }
    return sum / (3.0 * static_cast<double>(image.rgba.size() / 4));
}

/// Whether every face has `levels` levels halving from `size`.
bool levelsHalve(const world::CubeTexture& cube, int size, int levels)
{
    for (const auto& face : cube.faces) {
        if (static_cast<int>(face.size()) != levels) {
            return false;
        }
        for (int level = 0; level < levels; ++level) {
            const int edge = std::max(1, size >> level);
            if (face[static_cast<std::size_t>(level)].width != edge
                || face[static_cast<std::size_t>(level)].height != edge) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("cubeFacesFollowEachOtherInTheirTail", "[shading]")
{
    // A 4-texel DXT1 cube is all packed tail: one 32x32-block (8 KiB) surface
    // per face, the level 16 texels along X.
    constexpr std::size_t kFaceBytes = 32 * 32 * 8;
    std::vector<std::uint8_t> memory(kFaceBytes * world::CubeTexture::kFaces, 0);
    const std::uint32_t block = xds::tiledOffset2D(4, 0, 32, 3);
    for (std::size_t face = 0; face < world::CubeTexture::kFaces; ++face) {
        for (std::size_t b = 0; b < 8; ++b) {
            memory[face * kFaceBytes + block + b] = static_cast<std::uint8_t>(face * 16 + b);
        }
    }
    world::XboxCubeLayout layout;
    layout.formatWord = kDxt1;
    layout.size = 4;
    const world::CubeTexture cube = world::untileXboxCube(memory, layout);
    CHECK(cube.levelCount() == 1);
    CHECK(cube.format == world::TextureSurface::Format::Dxt1);
    for (std::size_t face = 0; face < world::CubeTexture::kFaces; ++face) {
        const Bytes& data = cube.faces[face][0].data;
        CHECK(data.size() == std::size_t{8});
        if (data.size() == 8) {
            CHECK(static_cast<int>(data[0]) == static_cast<int>(face * 16));
            CHECK(static_cast<int>(data[7]) == static_cast<int>(face * 16 + 7));
        }
    }
}

TEST_CASE("cubeLayoutsTheDataCannotHoldAreRejected", "[shading]")
{
    const std::vector<std::uint8_t> memory(1024, 0);
    world::XboxCubeLayout layout;
    layout.formatWord = kDxt1;
    layout.size = 256;
    bool threw = false;
    try {
        world::untileXboxCube(memory, layout);
    } catch (const DataError&) {
        threw = true;
    }
    CHECK(threw);
    layout.size = 100;
    threw = false;
    try {
        world::untileXboxCube(memory, layout);
    } catch (const DataError&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!world::isCaffCube(memory));
}

TEST_CASE("emissiveSwitchOnFollowsTheSampler", "[shading]")
{
    using render::TrackFrameConstants;
    // By day -1, so no emissive map passes its ModelData.x test (0 by default).
    const glm::vec4 day = TrackFrameConstants::emissiveSwitchOn(0.0F, 1.0F, 12.0F);
    CHECK(day.x == -1.0F);
    CHECK(day.y == 0.0F);
    CHECK(day.z == 1.0F);
    CHECK(day.w == 12.0F);
    // Below the floor the curve counts as 0.
    CHECK(TrackFrameConstants::emissiveSwitchOn(0.049F, 1.0F, 0.0F).x == -1.0F);
    CHECK(TrackFrameConstants::emissiveSwitchOn(0.05F, 1.0F, 0.0F).x == 0.05F);
    // The lightmap ramp: min(max(x - 0.5, 0) * 4, 1) times the lane.
    CHECK_THAT(TrackFrameConstants::emissiveSwitchOn(0.6F, 1.0F, 0.0F).y, WithinAbs(0.4, 1e-6));
    CHECK_THAT(TrackFrameConstants::emissiveSwitchOn(0.6F, 0.5F, 0.0F).y, WithinAbs(0.2, 1e-6));
    CHECK(TrackFrameConstants::emissiveSwitchOn(1.0F, 0.75F, 0.0F).y == 0.75F);
    CHECK(TrackFrameConstants::emissiveSwitchOn(1.0F, 0.0F, 0.0F).y == 0.0F);
    // w is the presentation clock, wrapped as TimeGain's.
    CHECK_THAT(TrackFrameConstants::emissiveSwitchOn(1.0F, 0.0F, 28810.0F).w, WithinAbs(10.0, 1e-3));
}

// vc360_3 reads the first of the vertex group, never the pixel constant c360_3.
TEST_CASE("materialConstantsReadTheirStagesGroup", "[shading]")
{
    using render::materialSlot;
    using xenos::ShaderStage;
    constexpr auto kVertex = static_cast<std::uint32_t>(world::ShadedMesh::kFirstVertexConstant);
    CHECK(materialSlot("c360_0", ShaderStage::Pixel) == std::optional<std::uint32_t>(0));
    CHECK(materialSlot("c360_6", ShaderStage::Pixel) == std::optional<std::uint32_t>(6));
    CHECK(materialSlot("vc360_3", ShaderStage::Vertex) == std::optional<std::uint32_t>(kVertex));
    CHECK(materialSlot("vc360_9", ShaderStage::Vertex) == std::optional<std::uint32_t>(kVertex + 6));
    // The bunting effects' vertex shaders name theirs c360_N.
    CHECK(materialSlot("c360_4", ShaderStage::Vertex) == std::optional<std::uint32_t>(kVertex + 1));
    CHECK(!materialSlot("vc360_2", ShaderStage::Vertex).has_value());
    CHECK(!materialSlot("vc360_10", ShaderStage::Vertex).has_value());
    CHECK(!materialSlot("c360_8", ShaderStage::Pixel).has_value());
    CHECK(!materialSlot("vc360_3", ShaderStage::Pixel).has_value());
    CHECK(!materialSlot("FogColor", ShaderStage::Vertex).has_value());
}

TEST_CASE("sceneConstantsTakeTheTrackSettings", "[shading]")
{
    constexpr std::string_view kXml = R"(<TrackSettings Version="64">
  <DarkColor r="0.1" g="0.2" b="0.3" a="1.0"/>
  <LightColor r="0.9" g="0.8" b="0.7" a="1.0"/>
  <Performance DynamicCubemap="1" NonFocusCarProjectedShadows="1"/>
  <RoadSpecular RoadSpecular="-0.017" RoadSpecular2="0.9" RoadSkid="-0.08" RoadSkid2="0.246" RoadGlossiness="24" RoadGlossiness2="23" RoadLightmapDarkPoint="0.49" RoadLightmapLightPoint="0.93"/>
  <TrackLightMapParams DarkPoint="0.3" LightPoint="1.0"/>
  <skyScale value="1.0"/>
  <cubeSkyScale value="-0.5"/>
  <cubeSkySaturation value="0.6"/>
  <cubeReflGroundScale value="2.0"/>
  <Cubemap Version="4" MeshSizeThreshold="0.06" MeshLimit="300" LimitPerFace="0" FacesPerFrame="6" FarDistance="200.0" VisibilityThreshold="5000"/>
  <LightHemiTop r="0.3" g="0.4" b="0.5" a="1.0"/>
  <LightHemiBottom r="0.1" g="0.2" b="0.3" a="1.0"/>
  <imageBasedLighting Version="3" Enable="0" LogLuvEncode="0" iblAmbientScale="1.6" iblAmbientSaturation="0.65" iblDirectScale="1.3" iblSunColorR="1.0" iblSunColorG="0.98" iblSunColorB="0.94"/>
</TrackSettings>)";
    const world::TrackSettings settings = world::TrackSettings::parse(kXml);
    CHECK(settings.cubemapFarDistance == 200.0F);
    CHECK(settings.lightHemiTop == glm::vec3(0.3F, 0.4F, 0.5F));
    CHECK(settings.lightHemiBottom == glm::vec3(0.1F, 0.2F, 0.3F));
    CHECK_THAT(settings.iblDirectScale, WithinAbs(1.3, 1e-6));
    CHECK(settings.iblSunColour == glm::vec3(1.0F, 0.98F, 0.94F));
    // 0.2 min(sun 1.3 clamp(-y) + 0.7 top + 0.3 bottom, 1).
    const glm::vec4 noon = settings.cubeClearColour(glm::vec3(0.0F, -1.0F, 0.0F));
    CHECK_THAT(noon.x, WithinAbs(0.2, 1e-5));
    CHECK_THAT(noon.y, WithinAbs(0.2, 1e-5));
    CHECK_THAT(noon.z, WithinAbs(0.2, 1e-5));
    CHECK(noon.w == 0.0F);
    // Below the horizon only the hemisphere: 0.7 * 0.3 + 0.3 * 0.1 = 0.24.
    const glm::vec4 night = settings.cubeClearColour(glm::vec3(0.0F, 1.0F, 0.0F));
    CHECK_THAT(night.x, WithinAbs(0.2 * 0.24, 1e-5));
    CHECK_THAT(night.y, WithinAbs(0.2 * (0.7 * 0.4 + 0.3 * 0.2), 1e-5));
    // A low sun: blue gets 1.3 * 0.94 * 0.25 + 0.7 * 0.5 + 0.3 * 0.3.
    const glm::vec4 low = settings.cubeClearColour(glm::vec3(0.968F, -0.25F, 0.0F));
    CHECK_THAT(low.z, WithinAbs(0.2 * (1.3 * 0.94 * 0.25 + 0.7 * 0.5 + 0.3 * 0.3), 1e-5));
    CHECK(settings.skyScale == glm::vec4(1.0F, 0.6F, -0.5F, 2.0F));
    CHECK(settings.roadSpecular2 == glm::vec4(24.0F, 23.0F, 0.49F, 0.93F));
    CHECK(settings.darkColor == glm::vec4(0.1F, 0.2F, 0.3F, 1.0F));
    CHECK(settings.lightColor == glm::vec4(0.9F, 0.8F, 0.7F, 1.0F));
    CHECK(settings.trackLightMap == glm::vec4(0.3F, 1.0F, 0.0F, 0.0F));
    CHECK(settings.dynamicCubemap);

    render::SceneLighting lighting;
    lighting.switchOnLights = 1.0F;
    const auto withSettings = render::TrackFrameConstants::make(
        glm::mat4(1.0F), glm::mat4(1.0F), glm::vec3(0.0F), lighting, 5.0F, glm::mat4(1.0F), &settings);
    CHECK(withSettings.c[222] == settings.skyScale);
    CHECK(withSettings.c[224] == settings.roadSpecular2);
    CHECK(withSettings.c[247] == settings.darkColor);
    CHECK(withSettings.c[248] == settings.lightColor);
    CHECK(withSettings.c[213] == glm::vec4(1.0F, 0.0F, 1.0F, 5.0F));
    const auto without
        = render::TrackFrameConstants::make(glm::mat4(1.0F), glm::mat4(1.0F), glm::vec3(0.0F), lighting, 5.0F);
    CHECK(without.c[222] == glm::vec4(0.0F));
    CHECK(without.c[213] == withSettings.c[213]);

    bool threw = false;
    try {
        world::TrackSettings::parse("<TrackSettings/>");
    } catch (const DataError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("discCubeMaps", "[shading]")
{
    const std::unique_ptr<Vfs> vfs = disc();
    // 256 texels, DXT5, every level down to 1x1 in a packed tail.
    const std::optional<Bytes> track = vfs->read("media/tracks/colorado/staticCarCubemap.xpr");
    CHECK(track.has_value());
    if (track) {
        const world::CubeTexture cube = world::readXprCube(*track);
        CHECK(cube.name == std::string("StaticCarCubemap_HDR_1"));
        CHECK(cube.format == world::TextureSurface::Format::Dxt5);
        CHECK(levelsHalve(cube, 256, 9));
        // The sky lies above the ground: +Y (face 2) is brighter than -Y.
        CHECK(brightness(cube.faces[2][0]) > brightness(cube.faces[3][0]));
    }
    const std::optional<Bytes> shared = vfs->read("media/cars/shared/staticcube.xpr");
    CHECK(shared.has_value());
    if (shared) {
        const world::CubeTexture cube = world::readXprCube(*shared);
        CHECK(cube.format == world::TextureSurface::Format::Dxt1);
        CHECK(levelsHalve(cube, 256, 9));
    }
    const std::optional<Bytes> interior = vfs->read("media/cars/shared/interiorstaticcube.xpr");
    CHECK(interior.has_value());
    if (interior) {
        const world::CubeTexture cube = world::readXprCube(*interior);
        CHECK(cube.format == world::TextureSurface::Format::Dxt5);
        CHECK(levelsHalve(cube, 64, 7));
    }
    // Every XPR2 file on the disc with a cube reads.
    int cubes = 0;
    for (const std::string& path : vfs->list("media")) {
        if (!endsWithNoCase(path, ".xpr")) {
            continue;
        }
        const std::optional<Bytes> file = vfs->read(path);
        if (!file || file->size() < 20 || std::memcmp(file->data() + 16, "TXCM", 4) != 0) {
            continue;
        }
        try {
            const world::CubeTexture cube = world::readXprCube(*file);
            CHECK(cube.levelCount() >= 1);
            ++cubes;
        } catch (const DataError& e) {
            FAIL_CHECK(std::format("{}: {}", path, e.what()));
        }
    }
    CHECK(cubes >= 6);
}

TEST_CASE("discTrackCubeTextures", "[shading]")
{
    const std::unique_ptr<Vfs> vfs = disc();
    const std::shared_ptr<const lzx::ZipArchive> archive = vfs->archiveAt(world::TrackPaths::of("colorado").archive);
    CHECK(archive != nullptr);
    if (!archive) {
        return;
    }
    // The cube most reflective materials name, and a 2D texture.
    const world::CubeTexture cube = world::loadTrackCube(*archive, 0x00000D9D);
    CHECK(cube.format == world::TextureSurface::Format::Dxt1);
    CHECK(levelsHalve(cube, 256, 1));
    CHECK(brightness(cube.faces[2][0]) > brightness(cube.faces[3][0]));
    const lzx::ZipEntry* flat = archive->find("_0x00003751.bin");
    CHECK(flat != nullptr);
    if (flat) {
        CHECK(!world::isCaffCube(world::readEntry(*archive, *flat)));
    }
}

TEST_CASE("discTrackSettingsAndSwitchOnLights", "[shading]")
{
    const std::unique_ptr<Vfs> vfs = disc();
    const world::TrackSettings settings = world::TrackSettings::load(*vfs, "colorado");
    CHECK(settings.skyScale == glm::vec4(1.0F, 0.6F, -0.0F, 1.0F));
    CHECK(settings.roadSpecular2 == glm::vec4(24.0F, 24.0F, 0.49F, 0.93F));
    CHECK(settings.darkColor == glm::vec4(-0.0F, -0.0F, -0.0F, 1.0F));
    CHECK_THAT(settings.lightColor.x, WithinAbs(1.0, 1e-5));
    CHECK(settings.trackLightMap == glm::vec4(0.3F, 1.0F, 0.0F, 0.0F));
    CHECK(settings.dynamicCubemap);

    // SwitchOnLights runs from 0 by day to 1 by night.
    TimeOfDay time(TimeOfDayCurves::load(*vfs, "colorado"));
    time.setSeconds(12.0F * 3600.0F);
    CHECK(render::TrackFrameConstants::emissiveSwitchOn(time.lighting().switchOnLights, 0.0F, 0.0F).x == -1.0F);
    time.setSeconds(0.0F);
    CHECK(render::TrackFrameConstants::emissiveSwitchOn(time.lighting().switchOnLights, 0.0F, 0.0F).x > 0.0F);
}
