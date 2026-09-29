#include "ParticleShaders.h"

namespace fh::particles::glsl {

const char* const kVertex = R"(#version 450 core
// One particle record, filled by ParticleRenderer as the game fills its
// 36-byte vertices (0x82DC8F88 onwards): centre; size x, size y, normalised
// age, random byte; velocity and spin angle (quaternion for quad type 2);
// the rotated y axis (quad type 2 only).
struct ParticleRecord {
    vec4 position;
    vec4 shape;
    vec4 motion;
    vec4 frame;
};
layout(std430, binding = 0) readonly buffer ParticleRecords
{
    ParticleRecord u_particles[];
};

uniform mat4 u_viewProjection;
uniform vec3 u_cameraPosition;
uniform vec3 u_viewForward;
// 0 camera-facing, 1 aligned with the velocity, 2 oriented by a quaternion
// (CamPos_QuadType.w).
uniform int u_quadType;
// fadeInTo, fadeOutFrom, max_screen_size, forceEdgeOnFadeOff (Params).
uniform vec4 u_params;
uniform vec4 u_beginColour;
uniform vec4 u_midColour;
uniform vec4 u_endColour;
// Towards the light, and the effect's HeadLightPower (LightDir).
uniform vec4 u_lightDirection;
uniform vec4 u_lightInfluence;
uniform vec4 u_specularInfluence;
// Sun colour and shadow intensity (LightColorSun_ShadowIntensity).
uniform vec4 u_sunColour;
uniform vec3 u_ambientColour;
uniform vec3 u_carPosition;
uniform vec3 u_carUp;
uniform vec3 u_carRight;
uniform float u_density;
uniform float u_facingMatchesVelocity;
// 1 animated, 2 gradient, 4 row only.
uniform int u_variant;
// AnimationRate, NumAnimationFrames x and y.
uniform vec3 u_animation;
uniform float u_gradientRows;

out vec2 v_uv;
out vec3 v_uvNext;
out vec2 v_gradient;
out vec4 v_screen;
out vec4 v_colour;

// u, corner y, corner x, v of the four corners of a quad.
const vec4 kCorners[4] = vec4[4](vec4(1.0, -1.0, 1.0, 1.0), vec4(0.0, -1.0, -1.0, 1.0), vec4(0.0, 1.0, -1.0, 0.0),
    vec4(1.0, 1.0, 1.0, 0.0));

// Direct3D 9 multiplication, which the shaders' scalar maths relies on:
// zero times anything, infinity included, is zero.
float mul0(float a, float b)
{
    return (a == 0.0 || b == 0.0) ? 0.0 : a * b;
}

vec3 rotateByQuaternion(vec4 q, vec3 v)
{
    return v * (2.0 * q.w * q.w - 1.0) + 2.0 * dot(q.xyz, v) * q.xyz + 2.0 * q.w * cross(q.xyz, v);
}

void main()
{
    const int index = gl_VertexID >> 2;
    const vec4 corner = kCorners[gl_VertexID & 3];
    float u = corner.x;
    const float cornerY = corner.y;
    const float cornerX = corner.z;
    const float v = corner.w;
    const ParticleRecord p = u_particles[index];
    const vec3 centre = p.position.xyz;
    const float sizeX = p.shape.x;
    const float sizeY = p.shape.y;
    const float age = p.shape.z;
    const float random = p.shape.w;

    const vec3 toParticle = centre - u_cameraPosition;
    const float depth = dot(toParticle, u_viewForward);
    const vec3 n = toParticle * inversesqrt(dot(toParticle, toParticle));
    const float sizeClamp = min(u_params.z * depth * (1.0 / sizeX), 1.0);

    vec3 offset;
    vec3 normal;
    if (u_quadType == 0) {
        const float angle = fract(p.motion.w * 0.15915494 + 0.5) * 6.2831855 - 3.1415927;
        const float s = sin(angle);
        const float c = cos(angle);
        const vec3 right = vec3(n.z, 0.0, -n.x);
        const vec3 up = vec3(-n.x * n.y, n.x * n.x + n.z * n.z, -n.y * n.z);
        const vec3 spunRight = c * right - s * up;
        const vec3 spunUp = c * up + s * right;
        offset = cornerX * sizeX * spunRight + cornerY * sizeY * spunUp;
        normal = cross(spunRight, spunUp);
        if (u_facingMatchesVelocity > 0.5 && dot(spunRight, p.motion.xyz) < 0.0) {
            u = 1.0 - u;
        }
    } else if (u_quadType == 1) {
        const vec3 along = p.motion.xyz * inversesqrt(dot(p.motion.xyz, p.motion.xyz));
        const vec3 across0 = cross(along, n);
        const vec3 across = across0 * inversesqrt(dot(across0, across0));
        offset = cornerX * sizeX * across + cornerY * sizeY * along;
        normal = cross(across, along);
    } else {
        offset = rotateByQuaternion(p.motion, vec3(0.0, sizeY * cornerY, sizeX * cornerX));
        const vec3 normal0 = cross(offset, p.frame.xyz);
        normal = normal0 * inversesqrt(dot(normal0, normal0));
    }

    const vec3 world = centre + sizeClamp * offset;
    const float facing = dot(n, normal);
    const float side = float(facing > 0.0) - float(facing < 0.0);

    vec4 colour = u_midColour;
    if (age > u_params.y) {
        colour = u_midColour + (u_endColour - u_midColour) * clamp((age - u_params.y) * (1.0 / (1.0 - u_params.y)), 0.0, 1.0);
    }
    if (u_params.x > age) {
        colour = u_beginColour + (u_midColour - u_beginColour) * clamp(age * (1.0 / u_params.x), 0.0, 1.0);
    }

    const vec3 ambient = u_ambientColour * u_lightInfluence.y + u_lightInfluence.x;

    const vec3 fromCar = world - u_carPosition;
    const float carDistance2 = dot(fromCar, fromCar);
    const vec3 carDirection = fromCar * inversesqrt(carDistance2);
    const vec3 carForward = cross(u_carRight, u_carUp);
    const float ahead = clamp(1000.0 * dot(carDirection, carForward), 0.0, 1.0);
    const float nearCar = 1.0 - clamp(sqrt(carDistance2) * 0.028571429 - 0.14285715, 0.0, 1.0);
    const float vertical2 = (1.05 - abs(dot(carDirection, u_carUp))) * (1.05 - abs(dot(carDirection, u_carUp)));
    const float vertical4 = vertical2 * vertical2;
    const float lateral2 = (1.5 - abs(dot(carDirection, u_carRight))) * (1.5 - abs(dot(carDirection, u_carRight)));
    const float headLight = vertical4 * vertical4 * (ahead * nearCar) * (lateral2 * lateral2 - 0.1) * u_lightDirection.w;

    const float diffuse = clamp(-side * dot(u_lightDirection.xyz, normal), 0.0, 1.0);
    const vec3 toUp = vec3(-n.x, 1.0 - n.y, -n.z);
    const vec3 halfVector = toUp * inversesqrt(n.z * n.z + n.x * n.x + (1.0 - n.y) * (1.0 - n.y));
    const float specularBase = clamp(-side * dot(normal, halfVector), 0.0, 1.0);
    const float specular = mul0(u_specularInfluence.x, exp2(mul0(u_specularInfluence.y, log2(specularBase))));
    const float sun = max(diffuse, u_specularInfluence.z) * u_lightInfluence.z + specular;
    // No shadow map: the four-tap shadow test of the game's shader reads as
    // fully lit, which makes its shadow term 1.
    const vec3 lit = u_sunColour.rgb * sun + ambient + headLight;
    colour.rgb *= (lit - 1.0) * u_lightInfluence.w + 1.0;
    colour.a *= clamp(4.0 * sizeClamp - 1.2, 0.0, 1.0);
    if (u_params.w > 0.5) {
        colour.a *= clamp(abs(facing) * 10.0 - 4.0, 0.0, 1.0);
    }

    v_uv = vec2(u, v);
    v_uvNext = vec3(0.0);
    v_gradient = vec2(0.0);
    if ((u_variant & 1) != 0) {
        const float across = u_animation.y;
        const float down = u_animation.z;
        const float t = age * u_animation.x;
        const float frame = floor(t);
        const float blend = clamp(fract(t) * 5.0 - 4.0, 0.0, 1.0);
        const float column = fract((1.0 / across) * frame);
        if ((u_variant & 4) != 0) {
            const float row = floor(down * fract((1.0 / down) * random));
            v_uv = vec2((1.0 / across) * u + column, (1.0 / down) * v + row * (1.0 / down));
            v_uvNext = vec3(v_uv.x + 1.0 / across, v_uv.y, blend);
        } else {
            const float row = floor((1.0 / across) * frame);
            v_uv = vec2(column + (1.0 / across) * u, (1.0 / down) * v + row * (1.0 / down));
            const bool lastColumn = column + 0.01 > 1.0 - 1.0 / across;
            v_uvNext = vec3(v_uv.x + 1.0 / across, lastColumn ? v_uv.y + 1.0 / down : v_uv.y, blend);
        }
    }
    if ((u_variant & 2) != 0) {
        v_gradient = vec2(age, floor(u_gradientRows * fract((1.0 / u_gradientRows) * random)));
    }

    const vec4 clip = u_viewProjection * vec4(world, 1.0);
    v_screen = u_density > 0.01 ? vec4(clip.x, clip.y, depth, clip.w) : vec4(0.0);
    v_colour = colour;
    // The game moves vertices of nearly transparent particles to NaN, which
    // drops their triangles.
    gl_Position = colour.a < 0.03 ? vec4(uintBitsToFloat(0x7FC00000u)) : clip;
}
)";

const char* const kFragment = R"(#version 450 core
in vec2 v_uv;
in vec3 v_uvNext;
in vec2 v_gradient;
in vec4 v_screen;
in vec4 v_colour;

layout(location = 0) out vec4 o_colour;

layout(binding = 0) uniform sampler2D u_texture;
layout(binding = 1) uniform sampler2D u_depthTexture;
layout(binding = 2) uniform sampler2D u_gradientTexture;
uniform int u_variant;
uniform int u_soft;
uniform float u_density;
uniform float u_gradientRows;
// The renderer's projection: infinite far plane, reversed depth, so the
// view depth of a depth-buffer value d is near / d.
uniform float u_nearPlane;

void main()
{
    vec4 texel;
    if ((u_variant & 1) != 0) {
        const vec4 a = texture(u_texture, v_uv);
        const vec4 b = texture(u_texture, v_uvNext.xy);
        texel = a + (b - a) * v_uvNext.z;
    } else {
        texel = texture(u_texture, v_uv);
    }
    if (texel.a - 0.06 < 0.0) {
        discard;
    }
    vec4 colour = texel * v_colour;
    if ((u_variant & 2) != 0) {
        const float rows = 1.0 / u_gradientRows;
        colour *= texture(u_gradientTexture, vec2(v_gradient.x, v_gradient.y * rows + rows * 0.5));
    }
    if (u_soft != 0) {
        const vec2 screen = v_screen.xy * (1.0 / v_screen.w) * 0.5 + 0.5;
        const float sceneDepth = u_nearPlane / texture(u_depthTexture, screen).r;
        colour.a *= clamp(u_density * (sceneDepth - v_screen.z), 0.0, 1.0);
    }
    o_colour = vec4(colour.rgb * colour.a, colour.a);
}
)";

} // namespace fh::particles::glsl
