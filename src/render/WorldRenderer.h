#pragma once

#include "Gl.h"
#include "Shader.h"
#include "Texture.h"

#include "base/JobPool.h"
#include "world/TrackWorld.h"
#include "world/WorldTiles.h"

#include <glm/glm.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fh::render {

/// The light, sky and fog of one moment, in the terms of the game's own
/// shaders (all colours linear RGB, in the scene's HDR units):
///
/// - Track surfaces are lit by `sunColor` times the cosine towards the sun
///   plus a hemisphere ambient, `mix(groundColor, skyColor, 0.5 + 0.5 * n.y)`
///   (the track effects' `sunColor`, `sunDir`, `skyColor` and `groundColor`
///   constants).
/// - The sky dome (the game's `Sky_PS` and `Sky_Simple_VS`, embedded in the
///   executable) is `gain * clamp(mix(base, haze, h), 0, 1)` with
///   `base = mix(bottom, top, t ^ colourPower)`,
///   `h = hazeIntensity * (ellipse * max(dot(view, hazeDirection), 0)) ^ hazePower`
///   and `ellipse = clamp(1 + (hazeEllipticalScale - 1) * dot(view, T), 0, 1)`;
///   `t = clamp((w - clampBottom) / (clampTop - clampBottom), 0, 1)` for the
///   dome vertex's w. Inferred rather than read: that w is the sine of the
///   elevation and T the up vector, and which curve feeds each constant
///   (matched by name).
/// - Fog (the track effects' vertex shaders) blends towards
///   `mix(fogColour, fogScatterColour, scatter)` by
///   `(1 - 2 ^ (-density * max(distance - start, 0))) * (1 - amount * smoothstep(heightStart, heightEnd, y))`,
///   where `scatter = max(dot(view, toSun), 0) ^ scatterPower` times a ramp
///   from `scatterStart` over `scatterRamp` metres.
/// - Over the sky lies a band of the same fog colour times the sky gain
///   (`SkyFog_PS` with `SkyFog_VS`, which places the band on a sphere and
///   gives it the height `15000 * sin(elevation)`), faded out by
///   `1 - smoothstep(skyHeightFadeStart, skyHeightFadeEnd, height)`. Its
///   opacity comes from a constant the executable derives from the fog
///   density in a way not decoded; the fog of the terrain at 15 km stands in.
struct SceneLighting {
    /// Direction light travels from the sun (or moon), normalised.
    glm::vec3 sunDirection{};
    glm::vec3 sunColour{};
    /// Hemisphere ambient of the world: from straight above and below.
    glm::vec3 ambientTop{};
    glm::vec3 ambientBottom{};
    /// The same for cars, which the game lights with their own pair.
    glm::vec3 carAmbientTop{};
    glm::vec3 carAmbientBottom{};

    glm::vec3 skyTop{};
    glm::vec3 skyBottom{};
    float skyColourPower = 0.0F;
    /// Sine of the elevation where the sky has fully turned to the top
    /// colour, and where it starts to leave the bottom colour.
    float skyClampTop = 0.0F;
    float skyClampBottom = 0.0F;
    float skyGain = 0.0F;
    glm::vec3 hazeColour{};
    /// Normalised, towards the centre of the glow around the sun.
    glm::vec3 hazeDirection{};
    float hazeIntensity = 0.0F;
    float hazePower = 0.0F;
    float hazeEllipticalScale = 0.0F;
    /// The visible sun or moon: normalised direction towards it, and its
    /// colour with its opacity applied.
    glm::vec3 sunDiscDirection{};
    glm::vec3 sunDiscColour{};

    glm::vec3 fogColour{};
    glm::vec3 fogScatterColour{};
    float fogScatterPower = 0.0F;
    /// Base-2 extinction per metre beyond `fogStart` metres (the
    /// `2 ^ (-density * distance)` of the game's shaders).
    float fogDensity = 0.0F;
    float fogStart = 0.0F;
    /// Fog thins by up to `fogHeightFadeAmount` between these world heights.
    float fogHeightFadeAmount = 0.0F;
    float fogHeightFadeStart = 0.0F;
    float fogHeightFadeEnd = 0.0F;
    /// The sun's colour enters the fog from this distance, fully after
    /// another `fogScatterRamp` metres.
    float fogScatterStart = 0.0F;
    float fogScatterRamp = 0.0F;
    /// The band of fog the game draws over the bottom of the sky
    /// (`SkyFog_PS`) thins out between these heights on its 15 km ring.
    float fogSkyHeightFadeStart = 0.0F;
    float fogSkyHeightFadeEnd = 0.0F;
};

/// Per-frame values every world shader needs.
struct FrameParams {
    glm::mat4 view{1.0F};
    glm::mat4 projection{1.0F};
    glm::vec3 cameraPosition{0.0F};
    SceneLighting lighting;
    float time = 0.0F;
};

/// GLSL shared by the scene's shaders, to paste after the `#version` line:
/// uniforms for every SceneLighting value and the functions `skyColour`
/// (the sky seen along a direction), `hemisphereAmbient` and `applyFog`,
/// written after the game's shaders as SceneLighting describes.
extern const char* const kSceneLightingGlsl;

/// Sets the uniforms kSceneLightingGlsl declares; the shader must be bound.
void setSceneLighting(Shader& shader, const SceneLighting& lighting);

/// Streams and draws a track's world: 500 m tiles are rebuilt on worker
/// threads whenever the camera crosses one of a tile's level-of-detail
/// edges, and the textures their batches use are decoded in the background
/// and uploaded once ready. Until a tile's new geometry and textures are all
/// in, the old version stays on screen, so moving never shows holes.
class WorldRenderer {
public:
    struct Stats {
        std::size_t tilesDrawn = 0;
        std::size_t triangles = 0;
        std::size_t batches = 0;
        std::size_t texturesLoaded = 0;
        std::size_t textureBytes = 0;
        std::size_t pendingJobs = 0;
    };

    WorldRenderer(const world::TrackWorld& world, JobPool& jobs);
    ~WorldRenderer();
    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;

    /// Schedules tile builds and texture loads for a camera at `camera`, and
    /// uploads whatever the workers finished. Call once per frame on the GL
    /// thread before draw().
    void update(const glm::vec3& camera);
    void draw(const FrameParams& frame);

    /// Leaves out the chunks `hidden` marks (indexed like the world index's
    /// chunks); tiles holding changed chunks are rebuilt, keeping their old
    /// version on screen until the new one is ready.
    void setHiddenChunks(const std::vector<bool>& hidden);
    /// Starts drawing `chunks` as one movable object, built in the
    /// background; returns its handle.
    int addDynamic(std::vector<std::uint32_t> chunks);
    /// Moves a dynamic object: `transform` maps its chunks from where the
    /// world index places them to where they are now.
    void setDynamicTransform(int handle, const glm::mat4& transform);
    void removeDynamic(int handle);

    /// Tiles farther than this are not drawn, in metres.
    void setDrawDistance(float metres) { m_drawDistance = metres; }
    const Stats& stats() const { return m_stats; }
    /// True once the tiles around the last update position have geometry
    /// and textures, for holding a loading screen.
    bool nearbyReady() const { return m_nearbyReady; }

private:
    struct GpuMesh {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ibo = 0;
        std::vector<world::TileMesh::Batch> batches;
        std::size_t triangles = 0;
    };
    struct TileState {
        int builtState = -1;
        int requestedState = -1;
        bool building = false;
        std::shared_ptr<GpuMesh> mesh;
        /// A finished build waiting for its textures before it replaces the
        /// mesh on screen.
        std::unique_ptr<world::TileMesh> pending;
        int pendingState = -1;
    };
    struct BuiltTile {
        std::size_t tile = 0;
        int state = 0;
        std::unique_ptr<world::TileMesh> mesh;
    };
    struct LoadedTexture {
        std::uint32_t id = 0;
        std::unique_ptr<world::TextureMipChain> chain;
    };
    struct TextureSlot {
        Texture texture;
        bool requested = false;
        bool ready = false;
        bool failed = false;
        std::uint64_t lastUsed = 0;
    };

    struct Dynamic {
        std::shared_ptr<GpuMesh> mesh;
        glm::mat4 transform{1.0F};
    };
    struct BuiltDynamic {
        int handle = 0;
        std::unique_ptr<world::TileMesh> mesh;
    };

    std::shared_ptr<GpuMesh> upload(const world::TileMesh& mesh);
    void drawBatches(const GpuMesh& mesh, bool waterPass);
    static void destroy(GpuMesh& mesh);
    void requestTexture(std::uint32_t id, float priority);
    bool texturesReady(const world::TileMesh& mesh) const;
    const Texture& textureOr(std::uint32_t id, const Texture& fallback);
    void evictTextures();

    const world::TrackWorld& m_world;
    JobPool& m_jobs;
    world::WorldTileGrid m_grid;
    std::vector<TileState> m_tiles;
    std::unordered_map<std::uint32_t, TextureSlot> m_textures;

    std::mutex m_resultMutex;
    std::deque<BuiltTile> m_builtTiles;
    std::deque<BuiltDynamic> m_builtDynamics;
    std::deque<LoadedTexture> m_loadedTextures;
    std::vector<std::uint32_t> m_failedTextures;
    std::shared_ptr<std::atomic<bool>> m_alive;

    std::unordered_map<int, Dynamic> m_dynamics;
    int m_nextDynamic = 1;
    Shader m_shader;
    Texture m_white;
    Texture m_flatNormal;
    float m_drawDistance = 9000.0F;
    std::uint64_t m_frame = 0;
    glm::vec3 m_camera{0.0F};
    int m_cameraZone = -1;
    bool m_nearbyReady = false;
    Stats m_stats;
};

} // namespace fh::render
