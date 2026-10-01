#include "BillboardShaders.h"

namespace openhorizon::render::billboardglsl {

namespace {

/// Shared by both stages: the scene constants in the track effects'
/// registers (TrackFrameConstants), the draw's own and the cards.
const char* const kBlocks = R"(
layout(std140, binding = 4) uniform XeFrameConstants
{
    // c128-c131 WorldViewProjMatrix (the view-projection: the cards are in
    // world coordinates), c158 CamPos, c213 EmissiveSwitchOnThreshold, c214
    // FogConsts2, c215 FogColor2, c221 ambColor, c241 FogConsts, c242
    // FogColor, c243-c245 ViewInvMatrix (w: the camera), c250 sunDir
    // (towards the sun), c251 sunColor.
    vec4 c[256];
} xe_frame;

// A card as BillboardRenderer stores it (world::ScatterSet::Billboard):
// position (w: fade start), up (w: half the width), X axis (w: height),
// ground normal (w: near end), (u on the side axis's positive side, u on
// its negative side, v of the foot, v of the top), colour, baked light (w:
// far end), (width of the picture in u, 0, 0, 0).
struct Card {
    vec4 position;
    vec4 up;
    vec4 across;
    vec4 groundNormal;
    vec4 uv;
    vec4 colour;
    vec4 baked;
    vec4 extra;
};
layout(std430, binding = 9) readonly buffer Cards
{
    Card xe_cards[];
};

// ZoomCompensation (c163.x); the fade out's width (FadeValues.w negated,
// 0x833A57BC); MinNDotLAndShadowPower (c162) x and y.
uniform float u_zoom;
uniform float u_fadeOutWidth;
uniform float u_minNDotL;
uniform float u_nDotLPower;
)";

const char* const kVertexBody = R"(
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_colour;
layout(location = 2) out vec4 v_fog;
#if defined(NON_FACING)
layout(location = 3) out vec3 v_normal;
layout(location = 4) out vec3 v_baked;
layout(location = 5) out vec4 v_clip;
#else
layout(location = 3) out vec3 v_light;
#endif

// The quad list's corners 0 1 2 3 as two triangles.
const uint kCorners[6] = uint[6](0u, 1u, 2u, 0u, 2u, 3u);

void main()
{
    uint vertex = uint(gl_VertexID);
    Card card = xe_cards[vertex / 6u];
    uint corner = kCorners[vertex % 6u];
    vec3 position = card.position.xyz;
    vec3 up = card.up.xyz;
    vec3 toCamera = xe_frame.c[158].xyz - position;
    float distance = length(toCamera);

    // A zero fade start makes the fade-in a step, as the game's infinite reciprocal does.
    float fadeStart = card.position.w * u_zoom;
    float fromNear = distance - card.groundNormal.w * u_zoom + 2.0 * fadeStart;
    float fadeIn = fadeStart > 0.0 ? clamp(fromNear / fadeStart, 0.0, 1.0) : (fromNear > 0.0 ? 1.0 : 0.0);
    float fadeOut = clamp((card.baked.w * u_zoom - distance) / u_fadeOutWidth, 0.0, 1.0);
    float fade = min(fadeOut, fadeIn);
    float shown = fade > 0.0 ? 1.0 : 0.0;

#if defined(NON_FACING)
    vec3 side = normalize(cross(up, card.across.xyz));
#else
    vec3 side = normalize(cross(up, toCamera / distance));
#endif
    float sideSign = (corner == 1u || corner == 2u) ? -1.0 : 1.0;
    float top = corner >= 2u ? 1.0 : 0.0;
    vec4 world = vec4(position + side * (sideSign * card.up.w * shown) + up * (top * card.across.w * shown), 1.0);
    vec4 clip = vec4(dot(world, xe_frame.c[128]), dot(world, xe_frame.c[129]), dot(world, xe_frame.c[130]),
        dot(world, xe_frame.c[131]));
    gl_Position = clip;

    float u = sideSign > 0.0 ? card.uv.x : card.uv.y;
    float v = top > 0.0 ? card.uv.w : card.uv.z;
#if defined(FOUR_SIDED)
    // The level direction to the camera against the X axis picks one of
    // four pictures side by side.
    vec2 level = toCamera.xz * inversesqrt(dot(toCamera.xz, toCamera.xz));
    float along = level.x * card.across.x + level.y * card.across.z;
    float beside = level.y * card.across.x - level.x * card.across.z;
    float picture = along < -0.707 ? 0.0 : (along > 0.707 ? 2.0 : (beside > 0.0 ? 3.0 : 1.0));
    u += picture * card.extra.x;
#endif
#if defined(TWO_SIDED)
    if (dot(toCamera / distance, card.across.xyz) < 0.0) {
        u += card.extra.x;
    }
#endif
    v_uv = vec2(u, v);
    // Only cards with colour w above 0.5 reach here, which take it as it is.
    v_colour = vec4(2.0 * card.colour.rgb, fade);

    vec3 baked = card.baked.rgb * card.baked.rgb * xe_frame.c[213].y;
#if defined(NON_FACING)
    // The game flips the X axis by facing; with this winding it points away from the camera.
    vec3 across = normalize(card.across.xyz);
    v_normal = dot(across, toCamera) > 0.0 ? -across : across;
    v_baked = baked;
    v_clip = clip;
#else
    // The corner's direction from the card's position, bent by the side
    // axis, lights the card; the ground normal dims it.
    vec3 towards = normalize(side * sideSign + up * top);
    vec3 bent = normalize(vec3(towards.x - side.z, towards.y, towards.z + side.x));
    float groundLight = clamp(dot(card.groundNormal.xyz, xe_frame.c[250].xyz), 0.0, 1.0);
    float lambert = u_minNDotL + (1.0 - u_minNDotL) * clamp(dot(bent, xe_frame.c[250].xyz), 0.0, 1.0);
    v_light = baked + xe_frame.c[221].xyz
        + xe_frame.c[251].xyz * lambert * exp2(log2(groundLight) * u_nDotLPower);
#endif

    // Fog, as in the track effects: FogConsts (c241) x start, w density;
    // FogColor (c242) colour, w scatter start; FogConsts2 (c214) height
    // fade start, end and amount, scatter power; FogColor2 (c215) scatter
    // colour, w scatter ramp.
    vec3 camera = vec3(xe_frame.c[243].w, xe_frame.c[244].w, xe_frame.c[245].w);
    vec3 toPoint = world.xyz - camera;
    float pointDistance = sqrt(dot(toPoint, toPoint));
    vec3 viewDirection = toPoint * inversesqrt(dot(toPoint, toPoint));
    float h = clamp((world.y - xe_frame.c[214].x) / (xe_frame.c[214].y - xe_frame.c[214].x), 0.0, 1.0);
    float heightFade = h * h * (3.0 - 2.0 * h) * xe_frame.c[214].z;
    float density = clamp(1.0 / exp2(max(pointDistance - xe_frame.c[241].x, 0.0) * xe_frame.c[241].w), 0.0, 1.0);
    float amount = (1.0 - density) * (1.0 - heightFade);
    float scatterRamp = clamp((pointDistance - xe_frame.c[242].w) * (1.0 / xe_frame.c[215].w), 0.0, 1.0);
    float sunAlign = clamp(dot(viewDirection, xe_frame.c[250].xyz), 0.0, 1.0);
    float scatter = scatterRamp * exp2(log2(sunAlign) * xe_frame.c[214].w);
    vec3 fogColour = scatter * (xe_frame.c[215].xyz - xe_frame.c[242].xyz) + xe_frame.c[242].xyz;
    v_fog = vec4(amount * fogColour, 1.0 - amount);
}
)";

const char* const kFragmentBody = R"(
layout(binding = 0) uniform sampler2D Diffuse_TextureSampler;
#if defined(NON_FACING)
// Red: how much of the sun reaches the pixel.
layout(binding = 13) uniform sampler2D ShadowMaskSamp;
#endif

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;
layout(location = 2) in vec4 v_fog;
#if defined(NON_FACING)
layout(location = 3) in vec3 v_normal;
layout(location = 4) in vec3 v_baked;
layout(location = 5) in vec4 v_clip;
#else
layout(location = 3) in vec3 v_light;
#endif

layout(location = 0) out vec4 o_colour;

// Pixel c0.z, 1 before the tier draws (0x82440FB8).
const float kColourScale = 1.0;

void main()
{
    vec4 diffuse = texture(Diffuse_TextureSampler, v_uv);
    float alpha = diffuse.a * v_colour.w;
    // Alpha to mask on a single-sample target: keep alpha of one half or more.
    if (alpha < 0.5) {
        discard;
    }
#if defined(NON_FACING)
    float rw = 1.0 / v_clip.w;
    vec2 maskUv = vec2(0.5 * rw * v_clip.x + 0.5, 1.0 - (0.5 * v_clip.y * rw + 0.5));
    maskUv += vec2(0.5) / vec2(textureSize(ShadowMaskSamp, 0));
    float sunVisible = texture(ShadowMaskSamp, maskUv).r;
    vec3 normal = v_normal * inversesqrt(dot(v_normal, v_normal));
    float lambert = u_minNDotL + (1.0 - u_minNDotL) * clamp(dot(normal, xe_frame.c[250].xyz), 0.0, 1.0);
    vec3 light = xe_frame.c[251].xyz * lambert * sunVisible + xe_frame.c[221].xyz + v_baked;
#else
    vec3 light = v_light;
#endif
    vec3 colour = diffuse.rgb * kColourScale * v_colour.rgb * light;
    colour = colour * v_fog.w + v_fog.xyz;
    // The scene target holds the square root of the colour, as the game's.
    o_colour = vec4(sqrt(abs(colour)), alpha);
}
)";

std::string defines(const Variant& variant)
{
    std::string text = "#version 450 core\n";
    if (variant.nonFacing) {
        text += "#define NON_FACING 1\n";
    }
    if (variant.fourSided) {
        text += "#define FOUR_SIDED 1\n";
    }
    if (variant.twoSided) {
        text += "#define TWO_SIDED 1\n";
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

} // namespace openhorizon::render::billboardglsl
