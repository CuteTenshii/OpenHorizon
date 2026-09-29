#pragma once

#include "CubeMap.h"
#include "Gl.h"
#include "ScatterPass.h"
#include "Shader.h"
#include "Texture.h"
#include "TrackPrograms.h"

#include "base/JobPool.h"
#include "world/ShadedTiles.h"
#include "world/TrackWorld.h"
#include "world/WorldTiles.h"
#include "world/XboxCube.h"

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fh::render {

/// The light, sky and fog of one moment, in the terms of the game's own
/// shaders (all colours linear RGB, in the scene's HDR units):
///
/// - Track surfaces are drawn with the game's own effects (TrackPrograms),
///   which take the sun (`sunDir`, `sunColor`), a flat ambient (`ambColor`)
///   and a hemisphere ambient from above and below (`skyColor`,
///   `groundColor`) from this block.
/// - The sky dome (the game's `Sky_PS` and `Sky_Simple_VS`, embedded in the
///   executable) is `gain * clamp(mix(base, haze, h), 0, 1)` with
///   `base = mix(bottom, top, t ^ colourPower)`,
///   `h = hazeIntensity * (ellipse * max(dot(view, hazeDirection), 0)) ^ hazePower`
///   and `ellipse = clamp(1 + (hazeEllipticalScale - 1) * dot(view, T), 0, 1)`;
///   `t = clamp((w - clampBottom) / (clampTop - clampBottom), 0, 1)` for the
///   dome vertex's w. The executable builds the dome itself (0x82E34768):
///   rows every 6 degrees of elevation from -6 to 84 with w = (row + 1) / 15,
///   so w = elevation / 90 degrees + 1 / 15, and it raises the colour power
///   to at least 0.01 (0x82E34728). Inferred rather than read: that T is the
///   up vector, and which curve feeds each constant (matched by name).
/// - Fog (the track effects' vertex shaders) blends towards
///   `mix(fogColour, fogScatterColour, scatter)` by
///   `(1 - 2 ^ (-density * max(distance - start, 0))) * (1 - amount * smoothstep(heightStart, heightEnd, y))`,
///   where `scatter = max(dot(view, toSun), 0) ^ scatterPower` times a ramp
///   from `scatterStart` over `scatterRamp` metres. The executable fills the
///   constants at 0x82429780: FogConsts = (start, 0, 0, FogDensity * 0.01),
///   FogConsts2 = (heightStart, heightEnd, amount, scatterPower), and the
///   scatter start and ramp in the w of the two fog colours.
/// - Over the sky lies a band of the same fog colour times the sky gain
///   (`SkyFog_PS` with `SkyFog_VS`, which places the band on a sphere and
///   gives it the height `15000 * sin(elevation)`), faded out by
///   `1 - smoothstep(skyHeightFadeStart, skyHeightFadeEnd, height)`. Its
///   opacity is `min(FogDensity * 0.01 * 15000, 1)` (0x8242B618). Inferred:
///   that the band's `Gain` is the sky gain, and the band's elevation range,
///   which comes from constants whose source was not found.
struct SceneLighting {
    /// Direction light travels from the sun (or moon), normalised.
    glm::vec3 sunDirection{};
    glm::vec3 sunColour{};
    /// Flat ambient of the world (the track effects' `ambColor`, sampled
    /// from the AmbientColour curve into the same lighting block as the
    /// hemisphere pair below; 0x8265A7CC).
    glm::vec3 ambientColour{};
    /// Hemisphere ambient of the world: from straight above and below.
    glm::vec3 ambientTop{};
    glm::vec3 ambientBottom{};
    /// The same for cars, which the game lights with their own pair.
    glm::vec3 carAmbientTop{};
    glm::vec3 carAmbientBottom{};

    glm::vec3 skyTop{};
    glm::vec3 skyBottom{};
    float skyColourPower = 0.0F;
    /// The dome's w (elevation / 90 degrees + 1 / 15) where the sky has
    /// fully turned to the top colour, and where it starts to leave the
    /// bottom colour.
    float skyClampTop = 0.0F;
    float skyClampBottom = 0.0F;
    float skyGain = 0.0F;
    glm::vec3 hazeColour{};
    /// Normalised, towards the centre of the glow around the sun.
    glm::vec3 hazeDirection{};
    float hazeIntensity = 0.0F;
    float hazePower = 0.0F;
    float hazeEllipticalScale = 0.0F;
    /// The sun or moon sprite SkyObjects draws, from the SunObject curves
    /// (see SkyObjects): the normalised direction from SunObjectPos to
    /// SunObjectTargetPos, so from the sprite towards the camera.
    glm::vec3 sunObjectDirection{0.0F, -1.0F, 0.0F};
    glm::vec3 sunObjectColour{};
    float sunObjectAlpha = 0.0F;
    float sunObjectSize = 0.0F;
    /// Above one half the sprite is the moon.
    float sunObjectMoon = 0.0F;
    /// The stars' opacity (the StarFade curve).
    float starFade = 0.0F;
    /// The SwitchOnLights curve as sampled (0 by day, 1 by night), from
    /// which the scene takes when lit models and lightmaps switch on (see
    /// TrackFrameConstants::emissiveSwitchOn).
    float switchOnLights = 0.0F;

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
    /// Seconds the world has run unpaused since the track was loaded: the
    /// clock the track effects animate with (TrackFrameConstants::timeGain).
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
/// and uploaded once ready. Until a tile's new geometry, textures and
/// programs are all in, the old version stays on screen, so moving never
/// shows holes. Every batch draws with its material's own track effect,
/// translated from the game's microcode (TrackPrograms), reading the
/// converted vertices (world::ShadedMesh) from a storage buffer.
///
/// Where the driver has bindless textures (see TrackPrograms), a draw binds
/// nothing between the batches of one tile: every texture a visible tile
/// uses stays resident, a table in a storage buffer holds their handles,
/// and each batch's material names its entries. Binding textures per batch
/// instead costs about three times the GPU time with the thousands of
/// batches a view holds, so the bound path remains only for drivers without
/// them.
///
/// What the effects sample that the game supplies itself is bound as
/// follows, the features behind it not being implemented: the screen-space
/// shadow mask (ShadowMaskSamp, which the game fills from its cascaded
/// shadow maps) white, so every surface counts as sunlit; other samplers a
/// material leaves to the engine (lightmaps it did not bake) black; cube
/// maps other than the track's own (below) black.
///
/// Cube samplers take the cube map their material names: the reflective
/// materials' Cube_MapSampler names one of the track's own cube textures
/// (world::loadTrackCube; in Colorado three, used by 964, 20 and 3
/// materials). Where the material names none, or its cube has no data on
/// the disc (one texture, used by 41 materials, exists only as a bundle
/// copy, which holds no cube), the sampler reads black.
///
/// A few ambient occlusion and lightmap textures (56 of Colorado's, all in
/// AO_Sampler, AOMapSampler or Light_MapSampler slots, none of them diffuse)
/// have no data on the disc: the PVS file and the zones' streaming tables
/// name them, but bin.zip has no `_0x<ID>` file for them and no bundle pack
/// holds their record. Their units get the black texture, like the
/// lightmaps a material leaves to the engine.
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
    /// Shows the event props (world::EventPropFilter::route) of route
    /// `routeId` in place of the current route's, rebuilding the tiles that
    /// change as setHiddenChunks() does. The renderer starts on route 0,
    /// free roam's.
    void setEventRoute(int routeId);
    /// The route free roam loads (world::Placement::eventRoutes).
    static constexpr int kFreeRoamRoute = 0;
    int eventRoute() const { return m_eventRoute; }
    /// Chunks of event props the tiles include under the current route and
    /// hidden chunks, all levels of detail.
    std::size_t eventChunksIncluded() const { return m_eventChunksIncluded; }
    /// TimeGain (TrackFrameConstants::timeGain) as the last draw() set it.
    glm::vec4 timeGain() const { return m_timeGain; }
    /// EmissiveSwitchOnThreshold (TrackFrameConstants::emissiveSwitchOn) as
    /// the last draw() set it.
    glm::vec4 emissiveSwitchOn() const { return m_emissiveSwitchOn; }
    /// Cube maps the materials name that are on the GPU.
    std::size_t cubeMapsLoaded() const;
    /// Starts drawing `chunks` as one movable object, built in the
    /// background; returns its handle.
    int addDynamic(std::vector<std::uint32_t> chunks);
    /// Moves a dynamic object: `transform` maps its chunks from where the
    /// world index places them to where they are now.
    void setDynamicTransform(int handle, const glm::mat4& transform);
    void removeDynamic(int handle);

    /// Track effects not linked yet. Their translation starts on the job
    /// pool when the renderer is made; precompile() links them.
    std::size_t programsPending() const { return m_programs.pendingCount(); }
    /// Links translated track effects for up to `budget` on the GL thread,
    /// for a loading screen to call every frame. Returns whether any remain.
    bool precompile(std::chrono::microseconds budget) { return m_programs.compileSome(budget); }

    /// Tiles farther than this are not drawn, in metres.
    void setDrawDistance(float metres) { m_drawDistance = metres; }
    const Stats& stats() const { return m_stats; }
    /// True once the tiles around the last update position have geometry
    /// and textures, for holding a loading screen.
    bool nearbyReady() const { return m_nearbyReady; }

private:
    /// What a tile (or moving object) is built into: the track-effect
    /// geometry, and the procedural copies ScatterPass draws.
    struct TileBuild {
        world::ShadedMesh shaded;
        world::TileMesh scattered;

        bool empty() const { return shaded.indices.empty() && scattered.indices.empty(); }
        std::vector<std::uint32_t> textureIds() const;
    };
    struct GpuMesh {
        GLuint vao = 0;
        GLuint vertices = 0;
        GLuint indices = 0;
        /// One TrackMaterial per batch, 256 bytes apart without bindless
        /// textures (a uniform block each), packed with them.
        GLuint materials = 0;
        std::vector<world::ShadedMesh::Batch> batches;
        /// Every texture the batches name, to keep them from eviction while
        /// the mesh is drawn.
        std::vector<std::uint32_t> textureIds;
        /// The program of each batch, nullptr where it cannot be built.
        std::vector<const TrackPrograms::Program*> programs;
        std::size_t triangles = 0;
        /// The procedural copies, nullptr when there are none.
        std::shared_ptr<ScatterPass::Mesh> scattered;
    };
    struct TileState {
        int builtState = -1;
        int requestedState = -1;
        bool building = false;
        std::shared_ptr<GpuMesh> mesh;
        /// A finished build waiting for its textures before it replaces the
        /// mesh on screen.
        std::unique_ptr<TileBuild> pending;
        int pendingState = -1;
    };
    struct BuiltTile {
        std::size_t tile = 0;
        int state = 0;
        std::unique_ptr<TileBuild> mesh;
    };
    struct LoadedTexture {
        std::uint32_t id = 0;
        std::unique_ptr<world::TextureMipChain> chain;
    };
    struct LoadedCube {
        std::uint32_t id = 0;
        std::unique_ptr<world::CubeTexture> cube;
    };
    /// A cube map a material samples through a cube sampler. The few there
    /// are stay loaded for the renderer's life.
    struct CubeSlot {
        CubeMap map;
        bool ready = false;
        /// The resident bindless handle, 0 without one.
        GLuint64 handle = 0;
    };
    struct TextureSlot {
        Texture texture;
        bool requested = false;
        bool ready = false;
        bool failed = false;
        std::uint64_t lastUsed = 0;
        /// The resident bindless handle, 0 without one.
        GLuint64 handle = 0;
    };

    struct Dynamic {
        std::shared_ptr<GpuMesh> mesh;
        glm::mat4 transform{1.0F};
    };
    struct BuiltDynamic {
        int handle = 0;
        std::unique_ptr<TileBuild> mesh;
    };
    struct DrawItem {
        const TrackPrograms::Program* program = nullptr;
        const GpuMesh* mesh = nullptr;
        std::uint32_t batch = 0;
        /// Block of the frame constant buffer: 0 for the static world, one
        /// per moving object after it.
        std::uint32_t frameSlot = 0;
    };

    std::unique_ptr<TileBuild> build(const std::vector<std::uint32_t>& chunks);
    std::shared_ptr<GpuMesh> upload(const TileBuild& build);
    static void destroy(GpuMesh& mesh);
    void requestTexture(std::uint32_t id, float priority);
    /// Loads cube texture `id` in the background, once.
    void requestCube(std::uint32_t id);
    /// The handle table entry of cube texture `id`, black until it loads.
    std::uint32_t cubeEntry(std::uint32_t id);
    /// The cube map of `id` if it is loaded, else the black one.
    GLuint cubeTexture(std::uint32_t id) const;
    bool texturesReady(const TileBuild& build) const;
    const Texture* texture(std::uint32_t id);
    /// The texture handle table entry of texture `id`, assigned on first
    /// use and kept for the renderer's life.
    std::uint32_t textureEntry(std::uint32_t id);
    void setHandle(std::uint32_t entry, GLuint64 handle);
    /// Uploads the handle table entries changed since the last frame.
    void flushHandles();
    /// Whether every texture the batch names is on the GPU or known to be
    /// missing from the disc.
    bool texturesLoaded(const world::ShadedMesh::Batch& batch) const;
    /// Marks the textures of a mesh about to be drawn as used this frame.
    void touchTextures(const GpuMesh& mesh);
    void evictTextures();
    void collect(const GpuMesh& mesh, std::uint32_t frameSlot);
    void drawItems(const std::vector<DrawItem>& items);
    /// Sets the pass's render states and the matching system block.
    void applyState(const world::TrackShaders::RenderState& state);
    /// glBindTextureUnit, skipped when the unit already holds `texture`.
    void bindTexture(GLuint unit, GLuint texture);
    void bindSamplers(const TrackPrograms::Program& program, const world::ShadedMesh::Batch& batch);

    /// Applies the event route and the hidden chunks to the grid.
    void applyFilters();

    const world::TrackWorld& m_world;
    JobPool& m_jobs;
    int m_eventRoute = kFreeRoamRoute;
    std::vector<bool> m_hiddenChunks;
    std::size_t m_eventChunksIncluded = 0;
    glm::vec4 m_timeGain{0.0F};
    glm::vec4 m_emissiveSwitchOn{0.0F};
    world::WorldTileGrid m_grid;
    std::vector<TileState> m_tiles;
    std::unordered_map<std::uint32_t, TextureSlot> m_textures;

    std::mutex m_resultMutex;
    std::deque<BuiltTile> m_builtTiles;
    std::deque<BuiltDynamic> m_builtDynamics;
    std::deque<LoadedTexture> m_loadedTextures;
    std::vector<std::uint32_t> m_failedTextures;
    std::deque<LoadedCube> m_loadedCubes;
    std::unordered_map<std::uint32_t, CubeSlot> m_cubes;
    std::unordered_map<std::uint32_t, std::uint32_t> m_cubeEntries;
    std::shared_ptr<std::atomic<bool>> m_alive;

    std::unordered_map<int, Dynamic> m_dynamics;
    int m_nextDynamic = 1;
    TrackPrograms m_programs;
    /// Whether the programs take their textures from the handle table.
    bool m_bindless = false;
    /// The engine-supplied textures (see the class comment).
    Texture m_white;
    Texture m_black;
    GLuint m_blackCube = 0;
    /// For volume samplers, which no track texture feeds.
    GLuint m_black3D = 0;
    /// The texture handle table (see texturehandle) on the CPU and the GPU,
    /// with the range of entries changed since it was last uploaded.
    std::vector<GLuint64> m_handles;
    GLuint m_handleBuffer = 0;
    std::size_t m_handleCapacity = 0;
    std::size_t m_dirtyFirst = 0;
    std::size_t m_dirtyEnd = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> m_textureEntries;
    /// Frame constant blocks, the system and control blocks.
    /// Frames in flight: each frame writes its constants into the next
    /// buffer, so the driver never waits for the GPU to finish reading one.
    static constexpr std::size_t kFrameBuffers = 3;
    std::array<GLuint, kFrameBuffers> m_frameBuffers{};
    std::array<std::size_t, kFrameBuffers> m_frameSlots{};
    /// The frame buffer of the frame being drawn.
    GLuint m_frameBuffer = 0;
    GLuint m_fixedBlocks = 0;
    /// Framebuffer height the fixed blocks were last written for.
    int m_fixedHeight = -1;
    /// Texture on each unit as this renderer last bound it, this frame.
    std::array<GLuint, 32> m_boundTextures{};
    ScatterPass m_scatter;
    /// Meshes with procedural copies to draw this frame, with the matrix
    /// that places them (identity for the static world).
    std::vector<std::pair<const GpuMesh*, glm::mat4>> m_scatterDraws;
    std::vector<DrawItem> m_opaque;
    /// Batches whose pass blends, drawn after all opaque ones.
    std::vector<DrawItem> m_blended;
    float m_drawDistance = 9000.0F;
    std::uint64_t m_frame = 0;
    glm::vec3 m_camera{0.0F};
    int m_cameraZone = -1;
    bool m_nearbyReady = false;
    Stats m_stats;
};

} // namespace fh::render
