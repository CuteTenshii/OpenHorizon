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
    struct Pass {
        std::shared_ptr<XenosProgram> program;
        std::optional<carmodel::CarPass::Blend> blend;
        /// The program's constants with its defaults and literals loaded.
        std::shared_ptr<const XenosConstants> defaults;
    };

    struct Draw {
        std::uint32_t batch = 0;
        const Pass* pass = nullptr;
        std::size_t constants = 0;
        bool mirrored = false;
    };

    glm::mat4 nodeTransform(const carmodel::CarMesh::Node& node, const CarPose& pose) const;
    void fillConstants(XenosConstants& c, const XenosProgram& program, const FrameParams& frame, const CarPose& pose,
        const carmodel::CarMesh::Batch& batch, const glm::mat4& world) const;
    void bindTextures(const XenosProgram& program, const carmodel::CarMesh::Batch& batch) const;
    const carmodel::ShaderSettings::Parameters* settingsFor(
        const carmodel::CarMesh::Batch& batch, const CarPose& pose) const;

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
