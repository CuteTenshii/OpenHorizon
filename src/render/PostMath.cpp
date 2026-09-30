#include "PostMath.h"

#include <algorithm>
#include <cmath>

namespace fh::render::post {

float filmic(const Filmic& k, float x)
{
    return (x * (k.a * x + k.c * k.b) + k.d * k.e) / (x * (k.a * x + k.b) + k.d * k.f) - k.e / k.f;
}

float exposureLog2(float luminance, const glm::vec4& keys, const glm::vec4& exposures, float bias)
{
    int segment = 1;
    if (!(luminance >= keys.y)) {
        segment = 0;
    } else if (luminance >= keys.z) {
        segment = 2;
    }
    const float from = keys[segment];
    const float to = keys[segment + 1];
    const float t = std::clamp((luminance - from) / (to - from), 0.0F, 1.0F);
    return bias + exposures[segment] + t * (exposures[segment + 1] - exposures[segment]);
}

glm::mat4 colourTransform(
    float contrast, float brightness, float saturation, const glm::vec3& weights, const glm::vec3& postMod)
{
    // glm matrices are column-major and act on column vectors: m[column][row].
    glm::mat4 contrastMatrix(1.0F + contrast);
    contrastMatrix[3] = glm::vec4(glm::vec3(-0.5F * contrast), 1.0F);

    glm::mat4 brightnessMatrix(1.0F);
    brightnessMatrix[3] = glm::vec4(glm::vec3(brightness), 1.0F);

    glm::mat4 saturationMatrix(1.0F);
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            saturationMatrix[column][row] = (1.0F - saturation) * weights[column] + (row == column ? saturation : 0.0F);
        }
    }

    glm::mat4 postModMatrix(1.0F);
    postModMatrix[0][0] = postMod.x;
    postModMatrix[1][1] = postMod.y;
    postModMatrix[2][2] = postMod.z;

    return postModMatrix * saturationMatrix * brightnessMatrix * contrastMatrix;
}

glm::vec4 vignetteTransform(float angleRadians, const glm::vec2& scale, float aspect)
{
    const float c = std::cos(angleRadians);
    const float s = std::sin(angleRadians);
    return {c * scale.x * aspect, s * scale.x, -s * scale.y * aspect, c * scale.y};
}

} // namespace fh::render::post
