#pragma once

#include "game/TimeOfDay.h"
#include "render/PostProcess.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {

class Vfs;

/// One half of the screen's vignette: the combine draws the top and bottom halves with one each (0x82433360).
/// Verified (0x8256D108, TrackSettings.xml parser): Angle is in degrees (0x82487D4C); alpha becomes minIntensity
/// times the VignetteAmount curve each frame (0x8255E2FC); MaxIntensity is never read.
struct VignetteSettings {
    float angleDegrees = 0.0F;
    glm::vec2 position{0.0F};
    glm::vec2 scale{1.0F};
    float power = 1.0F;
    glm::vec4 colour{0.0F};
    float minIntensity = 0.0F;
};

/// TrackSettings.xml's filmicTone; the curve form is in render/PostMath.h.
struct FilmicCurve {
    float white = 1.0F;
    float shoulderStrength = 0.0F;
    float linearStrength = 0.0F;
    float linearAngle = 0.0F;
    float toeStrength = 0.0F;
    float toeNumerator = 0.0F;
    float toeDenominator = 1.0F;
    float exposure = 0.0F;
};

/// The block the post manager blends each frame, limited to the fields used here (0x2B0 bytes; layout verified
/// from 0x825E02C0 and 0x8255D848). The renderer gets it verbatim at +0x1320 (0x8255D130, 0x8260E0F0).
struct PostSettings {
    float bloomScale = 0.0F;
    float bloomCutoff = 0.0F;
    float bloomPersist = 0.0F;
    float bloomLowerGradient = 0.0F;
    float bloomUpperGradient = 0.0F;
    std::array<float, 5> bloomWeights{};

    /// Saturation kept (1 leaves colour as is), the luminance weights it desaturates towards, and a per-channel
    /// scale applied after.
    float desaturation = 1.0F;
    glm::vec3 desatLuminosity{0.0F};
    glm::vec3 desatPostMod{1.0F};
    float exposureBias = 0.0F;
    float brightness = 0.0F;
    float contrast = 0.0F;

    float tonemapDelay = 0.0F;
    glm::vec2 cameraExposureRange{0.0F};
    /// ToneMapAdaptiveExposure: average luminance keys (extra dark, dark,
    /// light, extra light) and the log2 exposure at each.
    glm::vec4 adaptiveKeys{0.0F};
    glm::vec4 adaptiveExposures{0.0F};
    FilmicCurve filmic;

    VignetteSettings vignetteTop;
    VignetteSettings vignetteBottom;
};

/// The templates of media/dynamicpost/tracks/<track>/ and the zones of PostProcessingZones_safe.xml.
/// Verified (0x82579E78, 0x8257A070, 0x8256D108, 0x8257A368, 0x82569420): bloom gives only Cutoff, Scale and
/// Persist (Weight0..4 are never read); a zone with an unresolved template is dropped; at most NumTriangles kept.
struct PostTemplates {
    struct Bloom {
        std::string name;
        float scale = 0.0F;
        float cutoff = 0.0F;
        float persist = 0.0F;
    };
    struct ColourSettings {
        std::string name;
        float desaturation = 0.0F;
        glm::vec3 luminosity{0.0F};
        glm::vec3 postMod{0.0F};
        float exposureBias = 0.0F;
        float brightness = 0.0F;
        float contrast = 0.0F;
    };
    struct Vignette {
        std::string name;
        VignetteSettings settings;
    };
    struct Fog {
        std::string name;
        TimeOfDayCurves::FogTemplate settings;
    };

    std::vector<Bloom> bloom;
    std::vector<ColourSettings> colourSettings;
    std::vector<Vignette> vignette;
    std::vector<Fog> fog;
};

struct PostZone {
    std::string name;
    std::size_t bloom = 0;
    std::size_t vignetteTop = 0;
    std::size_t vignetteBottom = 0;
    std::size_t colourSettings = 0;
    std::size_t fog = 0;
    /// Seconds the blend into this zone takes.
    float lerpTime = 0.0F;
    /// Colour grading maps by file name in ColourGradingMaps.
    std::string gradingMap;
    std::string gradingMapNight;
    std::vector<std::array<glm::vec2, 3>> triangles;

    /// Strictly inside a triangle on (x, z): a point on an edge is outside (0x82551DF0).
    bool contains(glm::vec2 xz) const;
};

/// Throws DataError when a file is not a Templates document or a bloom, colour or vignette template lacks an
/// attribute (those loaders have no defaults).
PostTemplates parsePostTemplates(std::string_view bloomXml, std::string_view colourSettingsXml,
    std::string_view vignetteXml, std::string_view fogXml);

/// Parses PostProcessingZones_safe.xml against `templates`.
std::vector<PostZone> parsePostZones(std::string_view xml, const PostTemplates& templates);

/// The zone the player is in and the blend into it (0x82558D98, once per frame before the time of day).
/// Verified: first zone in file order wins; turning back into the zone being left reverses the blend (0x82554FF0);
/// entering from outside every zone snaps, leaving blends over its LerpTime. Inferred: the player's car (0x824BB8F8).
class PostZoneTracker {
public:
    static constexpr int kNone = -1;

    void update(const std::vector<PostZone>& zones, std::optional<glm::vec2> position, float dt);

    /// The zone blended towards and the one blended from; kNone for the
    /// settings outside every zone.
    int target() const { return m_target; }
    int source() const { return m_source; }
    float elapsed() const { return m_elapsed; }
    /// Smoothstep of elapsed() over the target's LerpTime (the source's when the target is kNone), 1 when both
    /// are kNone (0x82555058).
    float blend(const std::vector<PostZone>& zones) const;

private:
    int m_previous = kNone;
    int m_target = kNone;
    int m_source = kNone;
    float m_elapsed = 0.0F;
};

/// TrackSettings.xml's default block (0x825E6D08); filmicTone's *Night attributes over a copy of the day values
/// give the night curve (0x82E128B8).
struct TrackPostSettings {
    PostSettings settings;
    FilmicCurve filmicNight;
};

/// Throws DataError when a needed element is missing: what the game falls back to was not traced.
TrackPostSettings parseTrackPostSettings(std::string_view trackSettingsXml);

/// A 16x16x16 colour grading table: RGBA8 texels, red along x, green along
/// y, blue along the slices.
using GradingLut = std::array<std::uint8_t, 16 * 16 * 16 * 4>;

/// A little-endian DDS volume of 16x16x16 A8R8G8B8 texels, as every grading map is. Throws DataError otherwise.
GradingLut parseGradingLut(std::span<const std::uint8_t> dds, std::string_view source);

/// Per texel with identity colour I (index over 15), clamp(I + lerp(d*(fromDay - I) + n*(fromNight - I),
/// d*(toDay - I) + n*(toNight - I), t), 0, 1); d, n are the Day/NightColorGradeAmount curves (0x82552028).
GradingLut blendGradingLuts(const GradingLut& fromDay, const GradingLut& fromNight, const GradingLut& toDay,
    const GradingLut& toNight, float t, float day, float night);

/// A track's post-processing data and its per-frame state.
class PostProcessing {
public:
    /// Throws DataError when a file is missing or malformed.
    static PostProcessing load(const Vfs& vfs, std::string_view track);

    /// `position` is the player's (x, z), empty without one (0x8255D848, curve sampler 0x8265BC6C).
    void update(std::optional<glm::vec2> position, float dt, const TimeOfDayCurves& curves, float minutes);

    const PostSettings& settings() const { return m_settings; }
    /// `reset` makes the adapted luminance jump to the frame's.
    render::PostFrame frame(float aspect, bool reset) const;
    const GradingLut& gradingLut() const { return m_lut; }
    /// The fog template blend, which multiplies the Fog* curves.
    const TimeOfDayCurves::FogTemplate& fog() const { return m_fog; }
    const std::vector<PostZone>& zones() const { return m_zones; }
    const PostZoneTracker& tracker() const { return m_tracker; }
    /// The zone the blend goes towards, empty outside every zone.
    std::string_view zoneName() const;

private:
    struct ZoneLuts {
        GradingLut day{};
        GradingLut night{};
    };

    PostTemplates m_templates;
    std::vector<PostZone> m_zones;
    std::vector<ZoneLuts> m_zoneLuts;
    ZoneLuts m_defaultLuts;
    PostSettings m_default;
    FilmicCurve m_filmicNight;
    PostZoneTracker m_tracker;
    PostSettings m_settings;
    GradingLut m_lut{};
    TimeOfDayCurves::FogTemplate m_fog;
};

} // namespace openhorizon
