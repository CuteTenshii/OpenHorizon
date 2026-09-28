#pragma once

#include "Gl.h"
#include "Shader.h"
#include "Texture.h"
#include "WorldRenderer.h"

#include "carmodel/CarModel.h"

#include <glm/glm.hpp>

#include <array>
#include <vector>

namespace fh::render {

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
    /// Tail and brake lamps glow while braking.
    float brakeLights = 0.0F;
    /// Headlamps, by time of day.
    float headLights = 0.0F;
    /// Body colour (linear RGB) that tints the paint, and whether it is a
    /// metallic finish.
    glm::vec3 paint{0.8F};
    bool metallic = false;
};

/// One car model on the GPU, drawn node by node: the body and cockpit with
/// the car, each wheel and brake rotor turned by its steering and spin, and
/// each caliper by its steering only.
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
    glm::mat4 nodeTransform(const carmodel::CarMesh::Node& node, const CarPose& pose) const;

    std::vector<carmodel::CarMesh::Node> m_nodes;
    std::vector<carmodel::CarMesh::Batch> m_batches;
    std::vector<Texture> m_textures;
    Texture m_white;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_ibo = 0;
    Shader m_shader;
};

} // namespace fh::render
