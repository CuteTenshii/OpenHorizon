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
#include <utility>
#include <vector>

namespace fh::render {

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
        /// The program's constants with its defaults and literals loaded.
        std::shared_ptr<const XenosConstants> defaults;
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

} // namespace fh::render
