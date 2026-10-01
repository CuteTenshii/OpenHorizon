#pragma once

#include "Gl.h"
#include "Texture.h"
#include "WorldRenderer.h"
#include "XenosProgram.h"

#include "carmodel/CarModel.h"

#include <glm/glm.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openhorizon::render {

class CarProgramCache;

/// Where a car and its wheels are this frame.
struct CarPose {
    /// Car space to world.
    glm::mat4 body{1.0F};
    /// Per corner (front left, front right, rear left, rear right): the hub
    /// centre in car space, the steering angle and the wheel's spin angle,
    /// in radians.
    std::array<glm::vec3, 4> hubs{};
    std::array<float, 4> steer{};
    std::array<float, 4> spin{};
    /// Per corner: the wheel's angular speed, rad/s.
    std::array<float, 4> spinRate{};
    /// Tail and brake lamps glow while braking.
    float brakeLights = 0.0F;
    /// Headlamps, by time of day.
    float headLights = 0.0F;
    /// Body colour (linear RGB) that tints the paint, and whether it is a
    /// metallic finish.
    glm::vec3 paint{0.8F};
    bool metallic = false;
    /// The car effect's useStaticCubeMap (pixel b2): reflect the track's
    /// static cube (envStaticSampler) instead of the dynamic one
    /// (envSampler). Verified in the car item's setup (0x8246D448): the
    /// focus car of the view takes the static cube only while the dynamic
    /// one is not updated (0x8241A428: EnvMapFrequencyScale of 0, or the
    /// renderer's flag +8316, set through its vtable slot 236 by callers not
    /// traced, or the camera's vtable slot 120, not identified); every other
    /// car takes the dynamic one. Not applied: cars whose item field +632
    /// is 2 take the static cube, and what that field holds was not traced.
    bool useStaticCubeMap = false;
};

/// One car model on the GPU, drawn with the game's own car effect: every
/// batch runs the passes of the technique its material names, translated
/// from the effect's Xenos microcode, over the part's vertices as the carbin
/// stores them. The body and cockpit move with the car, each wheel and
/// brake rotor with its steering and spin, and each caliper with its
/// steering only.
class CarRenderer {
public:
    explicit CarRenderer(const carmodel::CarModel& model);
    ~CarRenderer();
    CarRenderer(const CarRenderer&) = delete;
    CarRenderer& operator=(const CarRenderer&) = delete;

    /// Draws the opaque batches, or with `translucent` the glass, which
    /// must come after every opaque surface of the scene.
    void draw(const FrameParams& frame, const CarPose& pose, bool translucent);
    /// Draws the car's depth with its batches' shadow map techniques (see
    /// casterTechnique()) through `viewProjection` into a shadow map, or
    /// with `screen` into a screen depth, where the glass is left out as the
    /// scene draws it after everything opaque. The cockpit is left out: the
    /// body and glass hide it from any light. The caller binds the target;
    /// culling is off, the car effect's shadow map passes not naming a cull
    /// mode.
    void drawDepth(const glm::mat4& viewProjection, const CarPose& pose, bool screen);
    /// The screen-space sun shadow mask every car samples as ShadowMaskSamp
    /// (ShadowPass); 0 for none (white).
    static void setShadowMask(GLuint texture);
    /// The cube maps every car reflects, as the renderer hands them to the
    /// car draw (SetEnvCubemaps, 0x8247B268): `dynamicCube` for envSampler
    /// (the renderer's vtable slot 268, its dynamic cube at +7448, see
    /// EnvironmentCube) and `staticCube` for envStaticSampler (slot 276, the
    /// track's staticCarCubemap.xpr at +8164). 0 leaves a sampler black.
    static void setEnvironmentCubes(GLuint dynamicCube, GLuint staticCube);
    /// The value the draws give pixel constant `name`, from the first pass
    /// declaring it; nothing when none does.
    std::optional<glm::vec4> pixelConstant(std::string_view name) const;
    /// The texture the draws bind to sampler `sampler`, from the first pass
    /// sampling it; 0 when none does or it is left empty.
    GLuint boundTexture(std::string_view sampler) const;
    /// The shadow map technique of a batch, as the car's render item
    /// builder (0x82E637A0) picks it: none for the motion blur meshes and
    /// the materials that cast no shadow (carmodel::castsNoShadow()),
    /// shadowMap_badge for badges, tireShadowMap for tyre materials
    /// (0x82E54180), rimShadowMap for rim materials, brakeShadowMap for the
    /// rotors and calipers, shadowMap otherwise. Not applied: the
    /// shadowMap_FocusCar variant, chosen by flag bits whose source was not
    /// traced.
    static std::string casterTechnique(const carmodel::CarMesh::Batch& batch, carmodel::CarMesh::NodeKind node);
    /// Programs linked for the car passes, shared by every live car renderer.
    static std::size_t programsLinked();

private:
    /// The constants fillConstants() sets on every draw.
    enum class Constant : std::size_t {
        WorldViewProjection,
        WorldView,
        World,
        WorldInverseTranspose,
        EyePosition,
        VertexEyePosition,
        PartPosition,
        Uv1Compression,
        Uv2Compression,
        ShCompression,
        UseMorph,
        ModelToWorld,
        TireRotation,
        WheelScale,
        TireDeformation,
        WheelMorph,
        ShDirectionalLight,
        ShHemisphereRed,
        ShHemisphereGreen,
        ShHemisphereBlue,
        ShDirectionalColour,
        PaintColour,
        LampAmounts,
        HeadLampAmounts,
        Count,
    };
    using ConstantSlots = std::array<XenosProgram::FloatSlot, static_cast<std::size_t>(Constant::Count)>;
    /// A material parameter of the settings files, resolved to its program's
    /// registers.
    using ParameterSlots = std::vector<std::pair<XenosProgram::FloatSlot, glm::vec4>>;

    struct Pass {
        std::shared_ptr<XenosProgram> program;
        /// The program's slots for the per-draw constants.
        std::shared_ptr<const ConstantSlots> slots;
        std::optional<carmodel::CarPass::Blend> blend;
        /// The states the car draw sets for the batch's material
        /// (carmodel::materialDrawState()).
        bool cull = true;
        bool alphaToMask = false;
        /// The program's constants with its defaults and literals loaded,
        /// and reflectionScaler as the car draw sets it.
        std::shared_ptr<const XenosConstants> defaults;
        /// The boolean register of useStaticCubeMap, when the pass has it.
        std::optional<std::uint32_t> useStaticCubeMap;
        /// The texture on each of the program's sampler units for the
        /// batch the pass draws; 0 leaves a unit empty.
        std::vector<std::pair<GLuint, GLuint>> textures;
        /// The material parameters for the batch the pass draws, for the
        /// normal finish and the metallic one.
        std::array<ParameterSlots, 2> parameters;
    };

    struct Draw {
        std::uint32_t batch = 0;
        const Pass* pass = nullptr;
        std::size_t constants = 0;
        bool mirrored = false;
    };

    glm::mat4 nodeTransform(const carmodel::CarMesh::Node& node, const CarPose& pose) const;
    void fillConstants(XenosConstants& c, const Pass& pass, const FrameParams& frame, const CarPose& pose,
        const carmodel::CarMesh::Batch& batch, const glm::mat4& world) const;
    /// The (unit, texture) pairs `program` samples for `batch`.
    std::vector<std::pair<GLuint, GLuint>> texturesFor(
        const XenosProgram& program, const carmodel::CarMesh::Batch& batch) const;
    const carmodel::ShaderSettings::Parameters* settingsFor(const carmodel::CarMesh::Batch& batch, bool metallic) const;

    /// Whether any of the car can be in the view: its bounding spheres
    /// against the sides of the view frustum and the plane of the eye.
    bool inView(const FrameParams& frame, const CarPose& pose) const;
    /// Whether the car lies inside the clip box of `viewProjection`: its
    /// sides, and with `orthographic` its near and far planes too.
    bool inBox(const glm::mat4& viewProjection, const CarPose& pose, bool orthographic) const;
    /// The shaderless-colour passes of `technique` for drawDepth(); empty
    /// when the effect lacks it.
    std::vector<Pass> casterPasses(const carmodel::CarModel& model, const std::string& technique);

    /// Whether batch `batch` of a corner node shows at the wheel's speed.
    bool wheelBatchVisible(const carmodel::CarMesh::Batch& batch, const CarPose& pose) const;

    std::shared_ptr<CarProgramCache> m_cache;
    std::optional<carmodel::CarModel::BlurRim> m_blurRim;
    std::vector<carmodel::CarMesh::Node> m_nodes;
    std::vector<carmodel::CarMesh::Batch> m_batches;
    std::vector<carmodel::CarMesh::VertexRange> m_ranges;
    std::map<std::string, carmodel::ShaderSettings, std::less<>> m_settings;
    carmodel::ShaderSettings m_tyreSettings;
    /// Passes of each batch's technique, by batch.
    std::vector<std::vector<Pass>> m_passes;
    /// Passes of each batch's shadow map technique, by batch.
    std::vector<std::vector<Pass>> m_casterPasses;
    std::vector<Texture> m_textures;
    /// A sphere round the model at rest (car space), and the radius round
    /// each hub that holds everything turning or steering with the wheel.
    glm::vec3 m_boundsCentre{0.0F};
    float m_boundsRadius = 0.0F;
    float m_wheelRadius = 0.0F;
    Texture m_black;
    Texture m_white;
    GLuint m_vao = 0;
    GLuint m_vertices = 0;
    GLuint m_indices = 0;
    /// Zeros standing in for the damage and morph streams of an undamaged
    /// car (see draw()).
    GLuint m_zeros = 0;
    XenosConstantStream m_stream;
    std::vector<Draw> m_draws;
};

} // namespace openhorizon::render
