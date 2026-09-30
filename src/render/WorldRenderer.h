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
#include <functional>
#include <memory>
#include <mutex>
#include <span>
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
    /// How dark the sun shadows are (the ShadowIntensity curve): a fully
    /// shadowed pixel keeps 1 - this of the sun, and at 0.01 or less the
    /// shadows are not drawn (see ShadowPass).
    float shadowIntensity = 0.0F;

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
/// programs are all in, the old version stays on screen. Every batch draws
/// with its material's own track effect, translated from the game's
/// microcode (TrackPrograms), reading the converted vertices
/// (world::ShadedMesh) from a storage buffer.
///
/// What is loaded depends on where the camera is, never on where it looks,
/// so turning round or looking back finds everything there: a tile holds
/// every chunk whose band ends beyond the camera's distance to the tile
/// less kPrefetchDistance (an engine choice: the streaming scenario drives
/// with no chunk late, where builds take 25 ms at the median and 0.3 s at
/// worst). What is drawn is picked chunk by chunk every frame, as the game
/// does for each draw (0x824701B8): a chunk is drawn while the camera's
/// zone lists it and the camera's 3D distance to its bounds lies in its
/// band. The game measures to the draw's oriented box, and for a
/// procedural copy to the world box of its transformed mesh bounds (one
/// cell per copy, 0x82ED6318), as the chunk's world bounds are; for the
/// zone-file draws the world bounds make levels switch a little nearer. The
/// scene pass also leaves out the chunks of a tile outside the view. Not
/// applied: the game's scaling of the second level's end by the field of
/// view (0x82470D18), whose input was not traced, and the cross-fades of
/// the procedural copies (see ScatterPass); the zone-file models
/// themselves switch levels without a fade in the game too.
///
/// Where the driver has bindless textures (see TrackPrograms), a draw binds
/// nothing between the batches of one tile: every texture a visible tile
/// uses stays resident, a table in a storage buffer holds their handles,
/// and each batch's material names its entries. Binding textures per batch
/// instead costs about three times the GPU time with the thousands of
/// batches a view holds, so the bound path remains only for drivers without
/// them.
///
/// The screen-space sun shadow mask (ShadowMaskSamp) is ShadowPass's
/// (setShadowMask()), white without one. What else the effects sample that
/// the game supplies itself is bound as follows, the features behind it
/// not being implemented: other samplers a material leaves to the engine
/// (lightmaps it did not bake) black.
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
    /// Draws the visible tiles' opaque batches and procedural copies, and
    /// gathers their blending batches for drawTranslucent().
    void draw(const FrameParams& frame);
    /// Draws the blending batches (road blends, decals, lakes, glows and
    /// light shafts) that the last draw() gathered, over the sky.
    ///
    /// Verified: the scene view's render (0x824744D0) has the track
    /// presentation (slot 300, 0x82469B18) draw its opaque passes (mask 1
    /// pass 0 at 0x824751AC, mask 8 pass 0 at 0x82475368), the sky at the
    /// end of the mask 8 pass (0x8246B474-0x8246B4DC: slot 104, 0x8249C2C0,
    /// calls CRealtimeSky's draw 0x82E37A18), and only then, after turning
    /// on AlphaBlendEnable (render state 60) and state 308 and turning off
    /// state 96, the mask 2 pass 1 (0x824755B0). Inferred: that pass draws
    /// the blending effects, whose Default passes set exactly those three
    /// states (lake_anim_norm_opac_refl_3, the h_vblnd and h_road blends,
    /// light_pollution). Missing: how the game assigns a batch to a pass and
    /// how it orders the batches within one; they keep draw()'s grouping
    /// by program.
    void drawTranslucent();

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
    std::size_t programsPending() const
    {
        return m_programs.pendingCount() + m_depthPrograms.pendingCount() + m_shadowPrograms.pendingCount()
            + m_cubePrograms.pendingCount();
    }
    /// Links translated track effects for up to `budget` on the GL thread,
    /// for a loading screen to call every frame. Returns whether any remain.
    bool precompile(std::chrono::microseconds budget);

    /// Draws the loaded tiles and moving objects with their effects'
    /// DepthOnly technique, for the screen depth of ShadowPass (the caller
    /// binds its target). `viewProjection` maps world positions to clip
    /// space around `camera`; only tiles also inside the orthographic box of
    /// `reach` are drawn, the shadows ending there (the last level's box).
    void drawDepthOnly(const glm::mat4& viewProjection, const glm::vec3& camera, const glm::mat4& reach);
    /// Draws the tiles and moving objects inside a light's box with their
    /// effects' ShadowDepthOnly technique, into the map the caller bound
    /// (see ShadowPass::beginLevel(), which sets the bias). Culling follows
    /// each pass's own cull mode; where the pass sets none it is off (the
    /// state the game's shadow pass inherits there was not traced).
    void drawShadowCasters(const glm::mat4& lightViewProjection, const glm::vec3& camera);
    /// Draws one face of the dynamic environment cube (EnvironmentCube)
    /// into the target the caller bound: the loaded tiles and moving
    /// objects with their effects' RealtimeCubemap technique, from the face
    /// camera `face` (its cameraPosition the cube's centre).
    ///
    /// Verified: the face draw hands the scene mode 1 to every track item
    /// (0x82469B18 takes the view's pass, 1 for cube faces), and the
    /// presentation's opaque and second buckets (masks 1 and 8, 0x824751AC
    /// and 0x82475368) are drawn for it. Inferred: mode 1 selects the
    /// effects' second technique, RealtimeCubemap in all of Colorado's that
    /// have one (the item side of the lookup, slot 164, was not traced).
    /// The tiles drawn are those with any part within `reach` metres of the
    /// centre: the cube's list job takes TrackSettings' Cubemap FarDistance
    /// squared (0x82E91F40); where it compares it was not traced.
    /// Not applied, as their rules were not traced: the list's cap of
    /// MeshLimit meshes ranked by size over squared distance from the camera
    /// with MeshSizeThreshold (0x82470E00, scaled by factors of the list
    /// descriptor, +180 and +184, whose values were not found), the
    /// VisibilityThreshold of the visibility object (0x82CEEAE8), and the
    /// buckets drawn only while the sun shadows are on (mask 2, 0x824755B0).
    /// Procedural copies (ScatterPass) are left out, their part in the
    /// cube's lists not being traced.
    void drawCubeFace(const FrameParams& face, float reach);
    /// Whether drawCubeFaces() can draw several faces in one pass: the
    /// driver lets vertex shaders pick the layer
    /// (GL_ARB_shader_viewport_layer_array). Without it, drawCubeFace()
    /// draws them one at a time.
    bool cubeFacesLayered() const { return m_cubePrograms.layeredCube(); }
    /// As drawCubeFace() for several faces at once, into the layered cube
    /// target the caller bound: every batch is drawn once, as one instance
    /// per face, which puts its clip position (relative to the cube's
    /// centre, `frame.cameraPosition`) through `transforms[i]` into layer
    /// `layers[i]`. The same tiles and batches are drawn as face by face,
    /// without the culling to each face's frustum; the track effects' cube
    /// passes read the camera only through its position and their world-
    /// view-projection matrix, so each face shows what drawCubeFace() would
    /// draw. Needs cubeFacesLayered().
    void drawCubeFaces(
        const FrameParams& frame, std::span<const glm::mat4> transforms, std::span<const int> layers, float reach);
    /// The screen-space sun shadow mask ShadowMaskSamp reads (ShadowPass);
    /// 0 for none (white). Pass 0 before the texture is deleted.
    void setShadowMask(GLuint texture);

    /// Tiles farther than this are not drawn, in metres.
    void setDrawDistance(float metres) { m_drawDistance = metres; }
    float drawDistance() const { return m_drawDistance; }
    const Stats& stats() const { return m_stats; }
    /// True once the tiles around the last update position have geometry
    /// and textures, for holding a loading screen.
    bool nearbyReady() const { return m_nearbyReady; }

    /// The tiles the world is streamed in, indexed like the tile arguments
    /// below.
    const world::WorldTileGrid& grid() const { return m_grid; }
    /// The chunks (indices into the world index's chunks) of the geometry
    /// tile `tile` has on the GPU: empty until its first build is in.
    const std::vector<std::uint32_t>& drawnChunks(std::size_t tile) const { return m_tiles[tile].drawnChunks; }
    /// Whether chunk `chunk`, when its tile has it on the GPU, was drawn as
    /// of the last update(): the camera's zone lists it (WorldChunk::zones)
    /// and the camera's 3D distance to its bounds lies in its band.
    bool chunkDrawn(std::uint32_t chunk) const { return chunk < m_chunkDrawn.size() && m_chunkDrawn[chunk] != 0; }
    /// Chunks are loaded this far in metres before the camera comes within
    /// their band (see update()).
    static constexpr float kPrefetchDistance = 100.0F;
    /// One replacement of a tile's geometry on screen, for measuring how
    /// long streaming takes.
    struct TileSwap {
        std::size_t tile = 0;
        /// Wall-clock seconds from asking for the build to its geometry
        /// being decoded, and from then to its textures being on the GPU.
        float buildSeconds = 0.0F;
        float textureSeconds = 0.0F;
    };
    /// The latest replacements, oldest first, up to kTileSwapsKept.
    const std::deque<TileSwap>& tileSwaps() const { return m_tileSwaps; }
    static constexpr std::size_t kTileSwapsKept = 8192;

private:
    /// What a tile (or moving object) is built into: the track-effect
    /// geometry, and the procedural copies ScatterPass draws.
    struct TileBuild {
        world::ShadedMesh shaded;
        world::TileMesh scattered;
        /// The chunks it was built from.
        std::vector<std::uint32_t> chunks;

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
        /// What is drawn of each batch this frame (see selectRanges() and
        /// selectChunks()): every index for moving objects.
        std::vector<IndexRange> ranges;
        std::vector<std::uint32_t> rangeStart;
        /// The same, less the chunks outside the camera's view, for the
        /// scene pass while `clipped` is set (see draw()).
        std::vector<IndexRange> viewRanges;
        std::vector<std::uint32_t> viewRangeStart;
        bool clipped = false;
        bool drawsNothing(std::uint32_t batch, bool view) const
        {
            const std::vector<std::uint32_t>& start = view && clipped ? viewRangeStart : rangeStart;
            return start[batch] == start[batch + 1];
        }
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
        /// The chunks of `mesh`.
        std::vector<std::uint32_t> drawnChunks;
        /// Whether each of drawnChunks was drawn at the last selectChunks().
        std::vector<std::uint8_t> selection;
        /// When the build in flight was asked for, and when it came back.
        std::chrono::steady_clock::time_point requestedAt;
        std::chrono::steady_clock::time_point builtAt;
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
    /// Picks the chunks of the tiles on the GPU that the camera draws, by
    /// the game's rule (see chunkDrawn()), and the index ranges that draw
    /// them.
    void selectChunks();
    /// Writes the selected ranges of `items`' batches as indirect commands
    /// of `instances` instances each into this frame's command buffer, and
    /// returns where each item's commands start; the last entry is the end.
    std::vector<std::uint32_t> prepareCommands(const std::vector<DrawItem>& items, GLsizei instances);
    /// Draws commands [first, end) of the last prepareCommands(), whose
    /// item's program, mesh and frame block are bound.
    void drawCommands(std::uint32_t first, std::uint32_t end, GLsizei instances);
    void drawItems(const std::vector<DrawItem>& items);
    /// drawCubeFace() and drawCubeFaces(): the batches of `m_cubePrograms`
    /// in the tiles and moving objects `wanted` keeps, drawn `instances`
    /// times with frame constants for `viewProjection` and `inverseView`.
    void drawCubePass(const FrameParams& frame, const glm::mat4& viewProjection, const glm::mat4& inverseView,
        const std::function<bool(const glm::vec3&, const glm::vec3&)>& wanted, GLsizei instances);
    /// drawDepthOnly() and drawShadowCasters(): every drawable batch of
    /// `programs` whose tile or object lies inside `viewProjection`'s clip
    /// box (with `farPlane` its far plane too).
    void drawDepthPass(TrackPrograms& programs, const glm::mat4& viewProjection, const glm::vec3& camera, bool farPlane,
        const glm::mat4* reach);
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
    /// The effects' DepthOnly and ShadowDepthOnly techniques.
    TrackPrograms m_depthPrograms;
    TrackPrograms m_shadowPrograms;
    /// The effects' RealtimeCubemap technique, for drawCubeFace().
    TrackPrograms m_cubePrograms;
    /// Frame blocks of the cube faces, packed one after another over a
    /// frame, and the fixed blocks written for the faces' size.
    GLuint m_cubeFrames = 0;
    std::size_t m_cubeFrameSlots = 0;
    std::size_t m_cubeFrameUsed = 0;
    std::uint64_t m_cubeFrameOf = 0;
    GLuint m_cubeFixed = 0;
    int m_cubeFixedHeight = -1;
    /// The XeCubeFaces block of drawCubeFaces().
    GLuint m_cubeFaceBlock = 0;
    /// Instances drawItems() draws of each batch.
    GLsizei m_drawInstances = 1;
    /// Per chunk of the world index, whether selectChunks() draws it.
    std::vector<std::uint8_t> m_chunkDrawn;
    /// Per chunk, whether it is drawn and inside the scene camera's view,
    /// for tiles the view cuts through.
    std::vector<std::uint8_t> m_chunkInView;
    /// Set while draw() draws the scene pass, whose items use the clipped
    /// ranges.
    bool m_viewPass = false;
    /// The layout of glMultiDrawElementsIndirect's commands.
    struct IndirectCommand {
        std::uint32_t count = 0;
        std::uint32_t instances = 0;
        std::uint32_t firstIndex = 0;
        std::int32_t baseVertex = 0;
        std::uint32_t baseInstance = 0;
    };
    /// The commands of the pass being drawn, and where they start in the
    /// frame's command buffer.
    std::vector<IndirectCommand> m_commands;
    std::size_t m_commandOffset = 0;
    /// Command buffers, one per frame in flight like the frame blocks, each
    /// filled pass after pass from the start of its frame.
    std::array<GLuint, 3> m_commandBuffers{};
    std::array<std::size_t, 3> m_commandCapacity{};
    std::size_t m_commandsUsed = 0;
    std::uint64_t m_commandsFrame = 0;
    /// Frame blocks of the depth passes, packed one after another over a
    /// frame, with the translator's fixed blocks for them.
    GLuint m_depthFrames = 0;
    std::size_t m_depthFrameSlots = 0;
    std::size_t m_depthFrameUsed = 0;
    std::uint64_t m_depthFrameOf = 0;
    GLuint m_depthFixed = 0;
    GLuint m_shadowMask = 0;
    GLuint64 m_shadowMaskHandle = 0;
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
    std::deque<TileSwap> m_tileSwaps;
    Stats m_stats;
};

} // namespace fh::render
