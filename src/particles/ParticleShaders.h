#pragma once

/// GLSL ports of the executable's particles_*_vs/ps, following the microcode's arithmetic; differences are
/// listed on ParticleRenderer.
namespace openhorizon::particles::glsl {

/// Vertex stage: an SSBO of 64-byte records, vertex id = 4 * particle + corner as in the game's quad lists.
extern const char* const kVertex;
/// Fragment stage of every variant.
extern const char* const kFragment;

} // namespace openhorizon::particles::glsl
