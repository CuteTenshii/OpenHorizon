#pragma once

#include "base/Bytes.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace openhorizon::spectators {

/// `media/spectators/models/<model>_<pose>.skinbin` (big-endian). Layout, verified on every file of the disc:
///
///     u32 1                        version
///     u32 L                        levels of detail (4 on the disc)
///     u32 1, u32 L                 array marker and length
///     L x (u32 1, u16 first index, u16 index count)
///     u32 1, u32 1                 markers
///     u32 V                        vertex count
///     V x (u32 1, vertex)          32-byte vertices, each after a marker
///     u32 1, u32 I                 index count
///     I x u16                      triangle list; the levels are ranges of it
///     u32 V, u32 I                 the counts again
///
///     vertex  f32 x, y, z; f32 normal x, y, z; u16 u, v (fractions of 65535); u8 bone 0, 1; u8 weight 0, 1
///             (sum 255)
///
/// Metres in the animations' skeleton frame: right-handed, Y up, facing +Z, left leg at +X. No texture is named;
/// spectators.xml pairs meshes and textures.
struct SkinnedModel {
    struct Vertex {
        glm::vec3 position {0.0F};
        glm::vec3 normal {0.0F, 1.0F, 0.0F};
        glm::vec2 texcoord {0.0F};
        std::array<std::uint8_t, 2> bones {0, 0};
        /// Out of 255.
        std::array<std::uint8_t, 2> weights {255, 0};
    };
    struct Lod {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
    };

    std::vector<Lod> lods;
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;

    /// One more than the highest bone any weighted vertex uses.
    int boneCount() const;
    /// Along Y, 0 for an empty mesh.
    float height() const;
};

/// Throws DataError naming `source` on another layout or an out-of-range index or level.
SkinnedModel parseSkinnedModel(ByteView data, const std::string& source);

} // namespace openhorizon::spectators
