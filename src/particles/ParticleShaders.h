#pragma once

/// GLSL for drawing the game's particles, written from the game's particle
/// shaders (particles_*_vs/ps in the executable, debug paths
/// C:\scratch\p4\horizon\...\Shaders\particles_*.updb). The code follows
/// the microcode's arithmetic step by step and was checked against a
/// translation of it (see ParticleRenderer.cpp); what differs is listed
/// there and on ParticleRenderer.
namespace openhorizon::particles::glsl {

/// Vertex stage. Particles are read from an SSBO of 64-byte records; each
/// particle is four vertices, vertex id = 4 * particle + corner, as the
/// game's quad lists are.
extern const char* const kVertex;
/// Fragment stage of every variant.
extern const char* const kFragment;

} // namespace openhorizon::particles::glsl
