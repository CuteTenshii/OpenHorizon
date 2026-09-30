#include "AnimatedObjectShaders.h"

#include <format>

namespace openhorizon::render::animatedglsl {

namespace {

/// Shared by both stages: the scene constants in the track effects'
/// registers (TrackFrameConstants), and the draw's own.
const char* const kBlocks = R"(
layout(std140, binding = 4) uniform XeFrameConstants
{
    // c128-c131 WorldViewProjMatrix (the world matrix is the identity, so
    // the view-projection), c136-c138 WorldInvTransMatrix, c213
    // EmissiveSwitchOnThreshold, c214 FogConsts2, c215 FogColor2, c221
    // ambColor, c241 FogConsts, c242 FogColor, c243-c245 ViewInvMatrix (w:
    // the camera), c250 sunDir (towards the sun), c251 sunColor.
    vec4 c[256];
} xe_frame;

layout(std140, binding = 7) uniform ProcAnimDraw
{
    // c152-c155 InstanceMatrix, for row vectors: world = x c152 + y c153 +
    // z c154 + c155. The draw loop (0x824449B0) sets the part's matrix
    // times the object's world matrix for rigid batches (0x82445180), the
    // world matrix alone for skinned ones (0x824451F8).
    vec4 instanceMatrix[4];
    // c156 UOffset: (offset, 0, 0, 0), added to u.
    vec4 uOffset;
    // Pixel c0 = (1, 1, 1, specular intensity) (0x82445958): z scales the
    // diffuse and emissive colours.
    vec4 c0;
    // Pixel c1: yz scale the normal map's coordinates, w blends the mapped
    // normal in (0x824458E4).
    vec4 c1;
    // c148 ModelData: x is the switch-on value from which the emissive map
    // lights (0x82444D4C).
    vec4 modelData;
    // c213 as the draw loop leaves it for the model (0x82444B40).
    vec4 emissiveSwitchOn;
} xe_draw;
)";

const char* const kVertexBody = R"(
// The stream as AnimatedObjectRenderer converts it from the files'
// big-endian vertices, whose declarations the executable builds at
// 0x82EC5380: POSITION0 four halves (w: u), NORMAL0 four signed bytes (w:
// v), TANGENT0 10:10:10:2 signed, BLENDINDICES0 and BLENDWEIGHT0 four
// bytes. The byte order the Xenos fetch gives (x from the element's last
// byte, from the low bits of a 10:10:10:2 word) is applied on the CPU.
layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
#if defined(NORM)
layout(location = 2) in vec4 in_tangent;
#endif
#if defined(SKINNED) || defined(SKINNED_1BONE)
layout(location = 3) in uvec4 in_boneIndices;
#endif
#if defined(SKINNED)
layout(location = 4) in vec4 in_boneWeights;
#endif

#if defined(SKINNED) || defined(SKINNED_1BONE)
// The second stream of the skinned declarations: per bone a translation and
// a 3x3 matrix row by row (row i the image of axis i). The indices address
// it as they are.
struct Bone {
    vec4 translation;
    vec4 row0;
    vec4 row1;
    vec4 row2;
};
layout(std430, binding = 8) readonly buffer ProcAnimBones
{
    Bone xe_bones[];
};

vec3 boneTransform(uint index, vec3 p)
{
    Bone b = xe_bones[index];
    return p.x * b.row0.xyz + p.y * b.row1.xyz + p.z * b.row2.xyz + b.translation.xyz;
}

vec3 boneRotate(uint index, vec3 n)
{
    Bone b = xe_bones[index];
    return n.x * b.row0.xyz + n.y * b.row1.xyz + n.z * b.row2.xyz;
}
#endif

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec4 v_texcoord;
layout(location = 2) out vec4 v_fog;
layout(location = 3) out vec4 v_clip;
layout(location = 4) out vec4 v_world;
#if defined(NORM)
layout(location = 6) out vec3 v_tangent;
layout(location = 7) out vec3 v_binormal;
#endif

vec3 instanceRotate(vec3 v)
{
    return v.x * xe_draw.instanceMatrix[0].xyz + v.y * xe_draw.instanceMatrix[1].xyz
        + v.z * xe_draw.instanceMatrix[2].xyz;
}

void main()
{
    vec3 position = in_position.xyz;
    vec3 normal = in_normal.xyz;
#if defined(SKINNED)
    // A linear blend of four bones, weight k with index k, not
    // renormalised.
    vec3 skinned = vec3(0.0);
    vec3 skinnedNormal = vec3(0.0);
    for (int k = 0; k < 4; ++k) {
        skinned += in_boneWeights[k] * boneTransform(in_boneIndices[k], position);
        skinnedNormal += in_boneWeights[k] * boneRotate(in_boneIndices[k], normal);
    }
    position = skinned;
    normal = skinnedNormal;
#elif defined(SKINNED_1BONE)
    position = boneTransform(in_boneIndices.x, position);
    normal = boneRotate(in_boneIndices.x, normal);
#endif

    vec4 world = position.x * xe_draw.instanceMatrix[0] + position.y * xe_draw.instanceMatrix[1]
        + position.z * xe_draw.instanceMatrix[2] + xe_draw.instanceMatrix[3];
    vec4 clip = vec4(dot(world, xe_frame.c[128]), dot(world, xe_frame.c[129]), dot(world, xe_frame.c[130]),
        dot(world, xe_frame.c[131]));
    gl_Position = clip;

    v_texcoord = vec4(in_position.w + xe_draw.uOffset.x, in_normal.w * 0.5 + 0.5, 0.0, 0.0);
    v_normal = instanceRotate(normal);
    v_clip = clip;
    v_world = vec4(world.xyz, 1.0);

#if defined(NORM)
    // The tangent ends in WorldInvTransMatrix (c136-c138), which the draw
    // loop never sets, so it keeps the scene's. Rigid batches take the
    // tangent as stored, without InstanceMatrix, and the binormal N x T;
    // skinned ones turn it with their bones and InstanceMatrix, flip it
    // where the stored handedness is not positive, and take T x N.
    vec3 tangent = in_tangent.xyz;
#if defined(SKINNED)
    vec3 skinnedTangent = vec3(0.0);
    for (int k = 0; k < 4; ++k) {
        skinnedTangent += in_boneWeights[k] * boneRotate(in_boneIndices[k], tangent);
    }
    tangent = instanceRotate(skinnedTangent);
#elif defined(SKINNED_1BONE)
    tangent = instanceRotate(boneRotate(in_boneIndices.x, tangent));
#endif
    vec3 worldTangent = vec3(dot(tangent, xe_frame.c[136].xyz), dot(tangent, xe_frame.c[137].xyz),
        dot(tangent, xe_frame.c[138].xyz));
#if defined(SKINNED) || defined(SKINNED_1BONE)
    v_tangent = in_tangent.w > 0.0 ? worldTangent : -worldTangent;
    v_binormal = cross(worldTangent, v_normal);
#else
    v_tangent = worldTangent;
    v_binormal = cross(v_normal, worldTangent);
#endif
#endif

    // Fog, as in every lit variant and the track effects: FogConsts (c241)
    // x start, w density; FogColor (c242) colour, w scatter start;
    // FogConsts2 (c214) height fade start, end and amount, scatter power;
    // FogColor2 (c215) scatter colour, w scatter ramp.
    vec3 camera = vec3(xe_frame.c[243].w, xe_frame.c[244].w, xe_frame.c[245].w);
    vec3 toPoint = world.xyz - camera;
    float distance = sqrt(dot(toPoint, toPoint));
    vec3 viewDirection = toPoint * inversesqrt(dot(toPoint, toPoint));
    float h = clamp((world.y - xe_frame.c[214].x) / (xe_frame.c[214].y - xe_frame.c[214].x), 0.0, 1.0);
    float heightFade = h * h * (3.0 - 2.0 * h) * xe_frame.c[214].z;
    float density = clamp(1.0 / exp2(max(distance - xe_frame.c[241].x, 0.0) * xe_frame.c[241].w), 0.0, 1.0);
    float amount = (1.0 - density) * (1.0 - heightFade);
    float scatterRamp = clamp((distance - xe_frame.c[242].w) * (1.0 / xe_frame.c[215].w), 0.0, 1.0);
    float sunAlign = clamp(dot(viewDirection, xe_frame.c[250].xyz), 0.0, 1.0);
    float scatter = scatterRamp * exp2(log2(sunAlign) * xe_frame.c[214].w);
    vec3 fogColour = scatter * (xe_frame.c[215].xyz - xe_frame.c[242].xyz) + xe_frame.c[242].xyz;
    v_fog = vec4(amount * fogColour, 1.0 - amount);
}
)";

const char* const kFragmentBody = R"(
layout(binding = 0) uniform sampler2D Diffuse_TextureSampler;
#if defined(NORM)
layout(binding = 1) uniform sampler2D NormalMapSampler;
#endif
#if defined(EMISSIVE)
layout(binding = 3) uniform sampler2D EmissiveMapSampler;
#endif
// Red: how much of the sun reaches the pixel.
layout(binding = 13) uniform sampler2D ShadowMaskSamp;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_texcoord;
layout(location = 2) in vec4 v_fog;
layout(location = 3) in vec4 v_clip;
layout(location = 4) in vec4 v_world;
#if defined(NORM)
layout(location = 6) in vec3 v_tangent;
layout(location = 7) in vec3 v_binormal;
#endif

layout(location = 0) out vec4 o_colour;

void main()
{
    vec2 uv = v_texcoord.xy;
    // The mask at this pixel, half a texel in as the fetch's offset says.
    float rw = 1.0 / v_clip.w;
    vec2 maskUv = vec2(0.5 * rw * v_clip.x + 0.5, 1.0 - (0.5 * v_clip.y * rw + 0.5));
    maskUv += vec2(0.5) / vec2(textureSize(ShadowMaskSamp, 0));
    float sunVisible = texture(ShadowMaskSamp, maskUv).r;

    vec3 normal = v_normal * inversesqrt(dot(v_normal, v_normal));
#if defined(NORM)
    // A DXT5-style normal map: x from red times alpha along the binormal,
    // y from green along the tangent, z rebuilt; blended with the
    // interpolated normal by c1.w.
    vec4 bump = texture(NormalMapSampler, uv * xe_draw.c1.yz);
    float nx = 2.0 * bump.r * bump.a - 1.0;
    float ny = 2.0 * bump.g - 1.0;
    float nz = sqrt(abs(1.0 - (nx * nx + ny * ny)));
    vec3 binormal = v_binormal * inversesqrt(dot(v_binormal, v_binormal));
    vec3 tangent = v_tangent * inversesqrt(dot(v_tangent, v_tangent));
    vec3 mapped = nx * binormal + ny * tangent + nz * v_normal;
    vec3 blended = (mapped - v_normal) * xe_draw.c1.w + v_normal;
    normal = blended * inversesqrt(dot(blended, blended));
#endif

    vec4 diffuse = texture(Diffuse_TextureSampler, uv);
#if defined(ALPHA_TO_MASK)
    if (diffuse.a < 0.5) {
        discard;
    }
#endif
    float nDotL = clamp(dot(normal, xe_frame.c[250].xyz), 0.0, 1.0);
    vec3 light = nDotL * xe_frame.c[251].xyz * sunVisible + xe_frame.c[221].xyz;

    vec3 colour = vec3(0.0);
#if defined(EMISSIVE)
    // Lit once the switch-on value reaches ModelData.x: off by day, on by
    // night.
    vec3 emissive = texture(EmissiveMapSampler, uv).rgb;
    float on = xe_draw.emissiveSwitchOn.x >= xe_draw.modelData.x ? 1.0 : 0.0;
    colour += emissive * xe_draw.emissiveSwitchOn.z * xe_draw.c0.z * on;
#endif
    colour += diffuse.rgb * xe_draw.c0.z * light;
    colour = colour * v_fog.w + v_fog.xyz;
    // The scene target holds the square root of the colour, as the game's.
    o_colour = vec4(sqrt(abs(colour)), diffuse.a);
}
)";

std::string defines(const Variant& variant)
{
    std::string text = "#version 450 core\n";
    if (variant.geometry == 1) {
        text += "#define SKINNED 1\n";
    } else if (variant.geometry == 2) {
        text += "#define SKINNED_1BONE 1\n";
    }
    if (variant.normalMap) {
        text += "#define NORM 1\n";
    }
    if (variant.emissive) {
        text += "#define EMISSIVE 1\n";
    }
    if (variant.alphaToMask) {
        text += "#define ALPHA_TO_MASK 1\n";
    }
    return text;
}

} // namespace

std::string vertexSource(const Variant& variant)
{
    return defines(variant) + kBlocks + kVertexBody;
}

std::string fragmentSource(const Variant& variant)
{
    return defines(variant) + kBlocks + kFragmentBody;
}

} // namespace openhorizon::render::animatedglsl
