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

/// One half of the screen's vignette: the final combine draws the top half
/// of the picture with one and the bottom half with another (two passes
/// over rows 0..h/2 and h/2..h, 0x82433360).
///
/// Verified against the vignette template loader (0x8256D108) and the
/// TrackSettings.xml parser: Angle is in degrees (the caller of the combine
/// multiplies it by 0.0174533 at 0x82487D4C), the colour's alpha is
/// replaced every frame by minIntensity times the VignetteAmount curve
/// (0x8255E2FC), and the templates' MaxIntensity is never read (no such
/// string exists in the executable).
struct VignetteSettings {
    float angleDegrees = 0.0F;
    glm::vec2 position{0.0F};
    glm::vec2 scale{1.0F};
    float power = 1.0F;
    glm::vec4 colour{0.0F};
    float minIntensity = 0.0F;
};

/// The filmic curve's parameters as TrackSettings.xml's filmicTone gives
/// them (the curve form is in render/PostMath.h).
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

/// The post-processing settings block the game's post manager blends each
/// frame (0x2B0 bytes; layout verified from the TimeOfDay.xml posteffects
/// parser 0x825E02C0 and the blend 0x8255D848), limited to the fields this
/// engine uses. The blended block is copied verbatim to the renderer
/// (0x8255D130, 0x8260E0F0: renderer + 0x1320), from which the final
/// combine and the bloom read their constants.
struct PostSettings {
    float bloomScale = 0.0F;
    float bloomCutoff = 0.0F;
    float bloomPersist = 0.0F;
    float bloomLowerGradient = 0.0F;
    float bloomUpperGradient = 0.0F;
    std::array<float, 5> bloomWeights{};

    /// The saturation the desaturation matrix keeps (1 leaves colour as it
    /// is), the luminance weights it desaturates towards, and a per-channel
    /// scale applied after it.
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

/// The template files of media/dynamicpost/tracks/<track>/ and the zones of
/// the track's PostProcessingZones_safe.xml.
///
/// Verified (loaders 0x82579E78, 0x8257A070, 0x8256D108, 0x8257A368, zone
/// loader 0x82569420): attribute names match exactly; the bloom templates
/// give only Cutoff, Scale and Persist (their Weight0..4 are never read: no
/// such string exists in the executable); a zone whose template names do
/// not all resolve is dropped; a zone keeps at most NumTriangles triangles
/// of three (posX, posZ) points each.
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

    /// Strictly inside one of the triangles on the ground plane (x, z):
    /// the three edge cross products all positive or all negative, so a
    /// point on an edge is outside (0x82551DF0).
    bool contains(glm::vec2 xz) const;
};

/// Parses the four template files. Throws DataError when one is not a
/// Templates document or a bloom, colour or vignette template lacks an
/// attribute (those loaders have no defaults).
PostTemplates parsePostTemplates(std::string_view bloomXml, std::string_view colourSettingsXml,
    std::string_view vignetteXml, std::string_view fogXml);

/// Parses PostProcessingZones_safe.xml against `templates`.
std::vector<PostZone> parsePostZones(std::string_view xml, const PostTemplates& templates);

/// Which zone the player is in and how far the blend into it has gone
/// (0x82558D98, run once per frame before the time of day moves on).
///
/// Verified: the zone is the first in file order containing the player's
/// position; when it changes the blend restarts from zero, unless the
/// player turns back into the zone being left, in which case the time is
/// mirrored so the blend reverses from where it was (0x82554FF0); entering
/// a zone from outside every zone snaps (the elapsed time becomes FLT_MAX),
/// while leaving one for the outside blends over that zone's LerpTime.
/// Inferred: the position is the player's car's (the first active entry of
/// the local player's vehicle list, 0x824BB8F8).
class PostZoneTracker {
public:
    static constexpr int kNone = -1;

    void update(const std::vector<PostZone>& zones, std::optional<glm::vec2> position, float dt);

    /// The zone blended towards and the one blended from; kNone for the
    /// settings outside every zone.
    int target() const { return m_target; }
    int source() const { return m_source; }
    float elapsed() const { return m_elapsed; }
    /// How far from source() to target() the blend is: smoothstep of the
    /// elapsed time over the target's LerpTime (the source's when the
    /// target is kNone), 1 when both are kNone (0x82555058).
    float blend(const std::vector<PostZone>& zones) const;

private:
    int m_previous = kNone;
    int m_target = kNone;
    int m_source = kNone;
    float m_elapsed = 0.0F;
};

/// The track's default settings block from TrackSettings.xml (0x825E6D08)
/// with its filmicTone read as the day curve, and the night curve of the
/// same element: its *Night attributes over a copy of the day values (the
/// parser 0x82E128B8 copies the day block before reading them).
struct TrackPostSettings {
    PostSettings settings;
    FilmicCurve filmicNight;
};

/// Throws DataError when an element the block needs is missing: the values
/// a missing element leaves were not traced.
TrackPostSettings parseTrackPostSettings(std::string_view trackSettingsXml);

/// A 16x16x16 colour grading table: RGBA8 texels, red along x, green along
/// y, blue along the slices.
using GradingLut = std::array<std::uint8_t, 16 * 16 * 16 * 4>;

/// Reads one of the game's grading maps: a little-endian DDS volume of
/// 16x16x16 A8R8G8B8 texels (every map in ColourGradingMaps and the track's
/// colorgradinglookup*.dds is one). Throws DataError otherwise.
GradingLut parseGradingLut(std::span<const std::uint8_t> dds, std::string_view source);

/// The table the combine samples, rebuilt each frame from the four maps of
/// the zone blend (0x82552028): for every texel with identity colour I,
/// clamp(I + lerp(d*(fromDay - I) + n*(fromNight - I), d*(toDay - I) +
/// n*(toNight - I), t), 0, 1), with d and n the DayColorGradeAmount and
/// NightColorGradeAmount curves. I is the texel index over 15.
GradingLut blendGradingLuts(const GradingLut& fromDay, const GradingLut& fromNight, const GradingLut& toDay,
    const GradingLut& toNight, float t, float day, float night);

/// Everything the post-processing takes from a track, and its per-frame
/// state: the zone blend, the time-of-day multipliers and the grading
/// table.
class PostProcessing {
public:
    /// Loads the templates, zones, grading maps and TrackSettings.xml's
    /// block. Throws DataError when a file is missing or malformed.
    static PostProcessing load(const Vfs& vfs, std::string_view track);

    /// Advances the zone blend by `dt` with the player at `position` (x, z;
    /// nothing when there is no player) and blends the settings for the
    /// time of day `minutes` (0x8255D848 and the curve sampler 0x8265BC6C).
    void update(std::optional<glm::vec2> position, float dt, const TimeOfDayCurves& curves, float minutes);

    const PostSettings& settings() const { return m_settings; }
    /// The renderer's constants for the current settings, for a picture of
    /// width over height `aspect`; `reset` makes the adapted luminance jump
    /// to the frame's.
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
