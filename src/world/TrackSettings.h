#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <optional>
#include <string_view>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::world {

/// The `TrackSettings.xml` values the track effects read as scene constants. Verified: loader 0x82E128B8 (vtable
/// 0x822569C4 slot 134) at the offsets below; the settings block (slot 16, 0x82553180) starts at +192; uploaded by
/// 0x82443D50 from the scene block 0x82469B18 fills.
struct TrackSettings {
    /// SkyScale (c222): skyScale, cubeSkySaturation, cubeSkyScale, cubeReflGroundScale (loader 892-904, settings
    /// 700-712; copied at 0x82418B10, 0x82469F7C-0x82469FB0).
    glm::vec4 skyScale{0.0F};
    /// RoadSpecularParams2 (c224): RoadGlossiness, RoadGlossiness2, RoadLightmapDarkPoint, RoadLightmapLightPoint
    /// (1024-1036; settings 832-844, 0x8246A0CC-0x8246A138).
    glm::vec4 roadSpecular2{0.0F};
    /// V2LightmapColor1 and 2 (c247, c248): DarkColor, LightColor (800, 816; settings 608, 624; 0x82418AF8,
    /// 0x82418B04).
    glm::vec4 darkColor{0.0F};
    glm::vec4 lightColor{0.0F};
    /// TrackLightMapParams DarkPoint, LightPoint (1200, 1204; settings 1008, 0x82418BC4), uploaded as pixel c212.
    /// Lane z scales EmissiveSwitchOnThreshold.y's night ramp; z and w are taken as 0 (no writer found).
    glm::vec4 trackLightMap{0.0F};
    /// LightHemiTop, LightHemiBottom (336, 352; settings 144, 160), iblDirectScale (984), iblSunColorR, G, B (992):
    /// inputs of cubeClearColour() (0x82418A84-0x82418AC0).
    glm::vec3 lightHemiTop{0.0F};
    glm::vec3 lightHemiBottom{0.0F};
    float iblDirectScale = 0.0F;
    glm::vec3 iblSunColour{0.0F};
    /// Cubemap FarDistance (632, 0x82E15968), squared for the environment cube's list job (0x82E91F40).
    /// min(3 FarDistance, 5000) under renderer flags +9519/+9520, untraced and taken as clear.
    float cubemapFarDistance = 0.0F;
    /// Performance/DynamicCubemap (byte 1378; settings 1186): render the car's environment cube each frame
    /// (0x82424244) instead of using staticCarCubemap.xpr.
    bool dynamicCubemap = false;
    /// ShadowLightDirection (400, 0x82E15ACC) when OverrideShadowLightDirection (368, 0x82E15A68) is set: a fixed
    /// sun shadow direction (0x82420940 slot 0x13C; 0x8247EBAC slot 0x140, manager +0x230 at 0x82412E50).
    std::optional<glm::vec3> shadowLightDirection;

    /// Environment cube clear colour, square-root encoded: 0.2 min(iblSunColour iblDirectScale clamp(-y, 0, 1)
    /// + 0.7 LightHemiTop + 0.3 LightHemiBottom, 1), alpha 0 (0x82E792C0; 0.2 at 0x833A1410, set from 0x82253E90;
    /// clamp 0x824A7D60). Cleared first at 0x82431998, then after each face's resolve at 0x8243A120.
    /// Inferred: y (renderer +8516, set by 0x824A8620) is the sun direction's.
    glm::vec4 cubeClearColour(const glm::vec3& sunDirection) const;

    /// Throws DataError when it is not a TrackSettings document or lacks one of the elements above.
    static TrackSettings parse(std::string_view xml);
    /// Reads `media/tracks/<track>/TrackSettings.xml`. Throws DataError if missing or malformed.
    static TrackSettings load(const Vfs& vfs, std::string_view track);
};

} // namespace openhorizon::world
