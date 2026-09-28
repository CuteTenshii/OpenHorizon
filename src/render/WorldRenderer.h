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

/// Per-frame values every world shader needs.
struct FrameParams {
    glm::mat4 view{1.0F};
    glm::mat4 projection{1.0F};
    glm::vec3 cameraPosition{0.0F};
    /// Direction light travels from the sun, normalised.
    glm::vec3 sunDirection{0.3F, -0.8F, 0.4F};
    glm::vec3 sunColour{1.0F};
    /// Sky overhead and at the horizon, from the time-of-day curves.
    glm::vec3 skyColour{0.05F, 0.15F, 0.4F};
    glm::vec3 horizonColour{0.7F, 0.85F, 0.95F};
    glm::vec3 groundColour{0.35F, 0.3F, 0.25F};
    glm::vec3 fogColour{0.7F, 0.78F, 0.88F};
    float fogDensity = 0.00012F;
    float time = 0.0F;
};

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

    std::shared_ptr<GpuMesh> upload(const world::TileMesh& mesh);
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
    std::deque<LoadedTexture> m_loadedTextures;
    std::vector<std::uint32_t> m_failedTextures;
    std::shared_ptr<std::atomic<bool>> m_alive;

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
