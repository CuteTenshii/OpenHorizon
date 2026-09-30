#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <optional>
#include <string_view>

namespace fh {
class Vfs;
}

namespace fh::world {

/// The values of a track's `TrackSettings.xml` that its effects read as
/// scene constants.
///
/// Verified in the executable: CTrackPresentation's settings loader
/// (0x82E128B8, slot 134 of its vtable at 0x822569C4) reads each element
/// into the presentation at the offsets below, and the presentation's
/// settings block that the renderer asks for (vtable slot 16, 0x82553180)
/// starts at +192, so a field at loader offset N is read by the scene
/// constant writer's callers at N - 192. The writer (0x82443D50) uploads
/// them from the scene block its caller (0x82469B18) fills.
struct TrackSettings {
    /// SkyScale (c222): `skyScale`, `cubeSkySaturation`, `cubeSkyScale`,
    /// `cubeReflGroundScale` (loader offsets 892, 896, 900, 904; settings
    /// 700-712), copied to the render thread's block at 0x82418B10 and from
    /// there into the scene block at 0x82469F7C-0x82469FB0.
    glm::vec4 skyScale{0.0F};
    /// RoadSpecularParams2 (c224): RoadGlossiness, RoadGlossiness2,
    /// RoadLightmapDarkPoint, RoadLightmapLightPoint (1024-1036; settings
    /// 832-844, 0x8246A0CC-0x8246A138).
    glm::vec4 roadSpecular2{0.0F};
    /// V2LightmapColor1 and 2 (c247, c248): DarkColor and LightColor (800,
    /// 816; settings 608 and 624, copied at 0x82418AF8 and 0x82418B04).
    glm::vec4 darkColor{0.0F};
    glm::vec4 lightColor{0.0F};
    /// TrackLightMapParams DarkPoint and LightPoint (1200, 1204). The scene
    /// block takes the vector at 1200 (settings 1008, 0x82418BC4) and
    /// uploads it as the pixel constant c212, which no track effect
    /// declares; its third lane (1208) scales the night lightmap ramp of
    /// EmissiveSwitchOnThreshold.y (see TrackFrameConstants). No code was
    /// found that writes 1208 or 1212 (neither the loader, which reads only
    /// the two points, nor the presentation's constructor), so they are
    /// taken as the zero the object starts with.
    glm::vec4 trackLightMap{0.0F};
    /// Performance/DynamicCubemap (byte 1378; settings 1186): with it set,
    /// the presentation renders the environment cube around the car each
    /// frame (0x82424244) instead of using staticCarCubemap.xpr.
    bool dynamicCubemap = false;
    /// ShadowLightDirection (loader offset 400, parsed at 0x82E15ACC) when
    /// OverrideShadowLightDirection (368, 0x82E15A68) is set: the scene
    /// then hands it, as written, to the object the shadow manager takes its
    /// light direction from (0x82420940 sets it through vtable slot 0x13C,
    /// 0x8247EBAC reads it back through slot 0x140 into the manager's +0x230
    /// at 0x82412E50), so the sun shadows keep one direction all day.
    /// Nothing when the track does not override it; where the direction
    /// comes from then was not traced. Optional in the file. The scene reads
    /// both at those same offsets of the presentation (0x82420940).
    std::optional<glm::vec3> shadowLightDirection;

    /// Parses the XML text of a TrackSettings.xml. Throws DataError when it
    /// is not a TrackSettings document or lacks one of the elements above.
    static TrackSettings parse(std::string_view xml);
    /// Reads `media/tracks/<track>/TrackSettings.xml`. Throws DataError if it
    /// is missing or malformed.
    static TrackSettings load(const Vfs& vfs, std::string_view track);
};

} // namespace fh::world
