#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

/// The game's particle effects: `media/effects/*.xml`, one emitter each, and
/// the list of effects a track loads (`media/effects/DefaultTrack.xml`).
///
/// Everything here is read the way the game's loader reads it
/// (CParticleSystemNew serialisation at 0x82DCA0C8, the emitter renderer's at
/// 0x82DC7A58 and its material's at 0x82DC3F50): element and attribute
/// names, which file versions carry which elements, the unit conversions and
/// the defaults for missing values all come from that code.
namespace openhorizon::particles {

/// Game path of the folder holding the effect files and their textures.
inline constexpr std::string_view kEffectsFolder = "media/effects";
/// Game path of the list of effects the game loads for a track
/// (`effects\DefaultTrack.xml` in the executable).
inline constexpr std::string_view kDefaultTrackPath = "media/effects/DefaultTrack.xml";

/// A value with a relative variation: the game draws uniformly from
/// value * (1 - variation) to value * (1 + variation) unless a field says
/// otherwise.
struct Varied {
    float value = 0.0F;
    float variation = 0.0F;
};

/// EmitterProperties type. The numbers are those the loader stores.
enum class EmitterShape : std::uint8_t { Box = 0, Plane = 1, SuperSpray = 2 };

/// SpinAxis type.
enum class SpinAxis : std::uint8_t { Random = 0, DirectionOfTravel = 1, PerpToDirOfTravel = 2, UserDefined = 3 };

/// Render/ParticleType value. Modeled particles (car debris) use meshes the
/// renderer here does not draw.
enum class ParticleType : std::uint8_t { Quad = 0, BillboardedQuad = 1, Modeled = 2 };

enum class BlendType : std::uint8_t { Alpha = 0, Additive = 1 };

/// Behavior/ParticleSpawn type.
enum class SpawnType : std::uint8_t { None = 0, Collision = 1, Death = 2, Trails = 3 };

/// ParticleShaderType, as the bit set the loader turns the name into
/// (`_animated` 1, `_gradient` 2, `_rowonly` 4, `_soft` 8). The name selects
/// one of the game's particle shaders (particles_[soft_]animated..._vs/ps).
namespace shader {
inline constexpr std::uint32_t kAnimated = 1;
inline constexpr std::uint32_t kGradient = 2;
inline constexpr std::uint32_t kRowOnly = 4;
inline constexpr std::uint32_t kSoft = 8;
} // namespace shader

/// Particle size over life (EmitterProperties/Size). A particle is born at
/// `value` if it grows (growFor > 0) and at its drawn maximum otherwise,
/// grows to the maximum over [growFrom, growFrom + growFor] and shrinks to
/// its drawn end size over [shrinkTo - shrinkFor, shrinkTo], in units of
/// time over the longest possible life.
struct SizeCurve {
    glm::vec2 value{0.0F};
    glm::vec2 variation{0.0F};
    glm::vec2 growFor{0.0F};
    glm::vec2 shrinkFor{0.0F};
    glm::vec2 maxValue{0.0F};
    glm::vec2 endValue{0.0F};
    float growFrom = 0.0F;
    float shrinkTo = 0.0F;
    /// Box emitters: length, height, width; plane emitters: length and width
    /// (height 0).
    glm::vec3 box{0.0F};
};

struct MotionInheritance {
    /// Percent chance (0-100) that a particle takes on the emitter's
    /// velocity.
    int influence = 0;
    float multiplier = 0.0F;
    /// Distance along the particle's own velocity it is moved at birth, on
    /// top of a random 0-0.015 (see Emitter).
    float offset = 0.0F;
    float variation = 0.0F;
};

/// Constant downward acceleration; negative strengths make particles rise.
struct Gravity {
    float strength = 0.0F;
    /// Read and kept, not used by the update (0x82DBFFC0).
    float decay = 0.0F;
};

/// Speed-proportional drag (0x82DC0098).
struct Friction {
    float strength = 0.0F;
    /// Normalised age from which drag applies.
    float startFrom = 0.0F;
    /// Speed below which drag stops.
    float velocityKept = 0.0F;
};

struct Wind {
    glm::vec4 direction{0.0F};
    float strength = 0.0F;
    float decay = 0.0F;
    float turbulence = 0.0F;
    float frequency = 0.0F;
    float scale = 0.0F;
};

/// Sideways wobble (leaves, litter): three amplitude entries, read in order
/// as amplitude, period and phase (0x82DC11C8).
struct BubbleMotion {
    Varied amplitude;
    Varied period;
    Varied phase;
};

/// The rim smoke's pull towards its wheel.
struct WheelAttractor {
    float sideDriftScale = 0.0F;
    float backDriftScale = 0.0F;
    float downDriftScale = 0.0F;
    float velInhMinDist = 0.0F;
    float velInhDistScale = 0.0F;
    float velInhMin = 0.0F;
};

/// A plane particles bounce off (glass and debris).
struct CollisionPlane {
    glm::vec4 plane{0.0F};
    Varied bounce;
};

/// One behaviour of Behavior, in file order, which is the order the game
/// applies them in.
struct Behaviour {
    enum class Kind : std::uint8_t { Gravity, Friction, Wind, BubbleMotion, WheelAttractor, Collision };
    Kind kind = Kind::Gravity;
    Gravity gravity;
    Friction friction;
    Wind wind;
    BubbleMotion bubble;
    WheelAttractor wheelAttractor;
    CollisionPlane collision;
};

/// Render/ParticleType/Material. Colours are stored the way the game keeps
/// them after loading: red, green and blue squared (the loader raises them
/// to the power 2, 0x82DC4104; saving takes the square root), alpha as in
/// the file.
struct Material {
    BlendType blend = BlendType::Alpha;
    glm::vec4 rgbStart{0.0F};
    glm::vec4 rgbMid{0.0F};
    glm::vec4 rgbEnd{0.0F};
    float nearFade = 0.0F;
    /// Normalised age at which the colour has gone from start to mid.
    float fadeInTo = 0.5F;
    /// Normalised age from which it goes from mid to end.
    float fadeOutFrom = 0.5F;
};

/// Render/ParticleType. Defaults are those of the renderer's constructor
/// (0x82DC9D98, 0x82DC9F80) for elements older files lack.
struct RenderSettings {
    ParticleType type = ParticleType::BillboardedQuad;
    bool dynamicLighting = false;
    bool zSort = false;
    /// Largest size, in units of view depth, before the quad is scaled down
    /// (the vertex shader clamps size to max_screen_size * depth / size).
    float maxScreenSize = 1.0F;
    bool forceEdgeOnFadeOff = false;
    bool facingMatchesVelocity = false;
    /// x: constant light, y: ambient scale, z: sun scale, w: how much the
    /// lighting applies at all (the shader blends from 1 to the lit value).
    glm::vec4 lightInfluence{0.0F, 2.0F, 1.0F, 0.9F};
    float headLightPower = 1.0F;
    /// x: specular scale, y: specular power, z: minimum sun term.
    glm::vec4 specularLightInfluence{0.0F, 1.0F, 0.0F, 0.0F};
    std::string texture;
    std::string texture2;
    Material material;
};

/// One effect file.
struct EffectDefinition {
    /// File name without folder or extension, lower case as on the disc.
    std::string name;
    int version = 0;
    /// Particles alive at once: the size of the system's ring (budget) and
    /// the smaller one the game switches to when it runs low (lowerbudget).
    std::uint32_t budget = 0;
    std::uint32_t lowerBudget = 0;
    /// Hours and minutes of the day the effect shows; files before version
    /// 11 have none (the whole day).
    int timeOfDay[4] = {0, 0, 24, 0};

    EmitterShape shape = EmitterShape::SuperSpray;
    /// SuperSpray spread angles in radians (the file has degrees; the loader
    /// converts value and variation, 0x82DCA4F8).
    Varied offAxis;
    Varied offPlane;
    /// Particles per second. The file's variation attribute is named `var`.
    Varied quantity;
    /// Speed in m/s.
    Varied velocity;
    float emitStart = 0.0F;
    float emitEnd = 0.0F;
    bool emitInfinite = false;
    std::uint32_t displayUntil = 0xFFFFFFFFU;
    /// Seconds; the variation scales both ways like the others.
    Varied life;
    float density = 0.0F;
    std::uint32_t shaderFlags = 0;
    float animationRate = 1.0F;
    int framesX = 1;
    int framesY = 1;
    int gradientRows = 1;
    bool renderInMirror = false;
    SizeCurve size;
    /// Seconds per half turn: the spin rate is pi / spinTime (0 below 0.01).
    Varied spinTime;
    /// Initial angle; value in radians (converted from degrees on load),
    /// variation used as the loader leaves it (an absolute +- range).
    Varied spinPhase;
    SpinAxis spinAxis = SpinAxis::Random;
    glm::vec3 spinAxisUser{0.0F};
    /// Spin in a random direction per particle rather than the sign of
    /// spinPhase's value.
    bool spinAlternate = false;
    bool lockToCamera = false;
    int surfaceEffectId = 0;

    MotionInheritance motionInheritance;
    std::vector<Behaviour> behaviours;
    SpawnType spawnType = SpawnType::None;
    std::string spawnEffect;

    RenderSettings render;

    bool animated() const { return (shaderFlags & shader::kAnimated) != 0; }
    bool gradient() const { return (shaderFlags & shader::kGradient) != 0; }
    bool rowOnly() const { return (shaderFlags & shader::kRowOnly) != 0; }
    bool soft() const { return (shaderFlags & shader::kSoft) != 0; }
    /// The name of the game shader the flags select, e.g.
    /// "particles_soft_animated_gradient" (pixel) without the stage suffix.
    std::string shaderName() const;
    /// The longest life a particle can draw (life * (1 + variation)).
    float maxLife() const { return life.value * (life.variation + 1.0F); }
};

/// Parses an effect file. Throws DataError when the text is not an Effect.
EffectDefinition parseEffect(std::string_view xml, std::string name);

/// One member of a composite effect (`_composite_*.xml`), which starts
/// several effects with offsets in space and time.
struct CompositeMember {
    std::string emitterName;
    std::string fileName;
    glm::vec4 positionOffset{0.0F};
    float startOffset = 0.0F;
    float finishOffset = 0.0F;
};

struct CompositeDefinition {
    std::string name;
    float repeatPeriod = 0.0F;
    std::vector<CompositeMember> members;
};

CompositeDefinition parseComposite(std::string_view xml, std::string name);

/// An entry of DefaultTrack.xml: the effect file and the name game code
/// looks the running system up by.
struct SystemEntry {
    std::string file;
    std::string systemName;
};

std::vector<SystemEntry> parseSystemList(std::string_view xml);

/// The effect file for `file` as DefaultTrack.xml names it (without
/// extension, any case).
std::string effectPath(std::string_view file);

/// Whether a file of the effects folder is a composite rather than a single
/// effect (the game tells them apart by the `_Composite_` prefix).
bool isCompositeName(std::string_view file);

} // namespace openhorizon::particles
