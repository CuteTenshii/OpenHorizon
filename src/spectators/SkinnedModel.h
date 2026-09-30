#pragma once

#include "base/Bytes.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace openhorizon::spectators {

/// A spectator mesh, `media/spectators/models/<model>_<pose>.skinbin`
/// (big-endian). Layout, verified on all 40 files of the disc:
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
///     vertex  f32 x, y, z; f32 normal x, y, z; u16 u, u16 v (fractions of
///             65535); u8 bone 0, u8 bone 1; u8 weight 0, u8 weight 1 (sum
///             255)
///
/// Every level indexes the one vertex array. Positions are metres in the
/// coordinates of the skeleton the animations carry (see CrowdAnimation.h):
/// Y up, the person facing +Z with the left leg at +X, a right-handed frame.
/// The file names no texture; spectators.xml pairs meshes and textures.
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
    /// Distance from the lowest to the highest vertex along Y, 0 for an
    /// empty mesh.
    float height() const;
};

/// Parses a `.skinbin` file. Throws DataError naming `source` when the data
/// does not have the layout above or an index or level lies out of range.
SkinnedModel parseSkinnedModel(ByteView data, const std::string& source);

} // namespace openhorizon::spectators
