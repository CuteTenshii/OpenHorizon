#pragma once

#include "Microcode.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
}

/// Validation of generated GLSL with glslang (through shaderc), without a GL context. Kept out of
/// openhorizon_xenos so the engine does not link shaderc.
namespace openhorizon::xenos {

/// Compiles `glsl` as a GLSL 4.50 shader of `stage` for OpenGL. Returns the
/// compiler's messages on failure, nothing on success.
std::optional<std::string> compileGlsl(std::string_view glsl, ShaderStage stage, std::string_view name);

/// Every `.fxobj` the game can load, loose or inside mounted archives, as normalized paths.
std::vector<std::string> listEffects(const Vfs& vfs);

struct CheckReport {
    std::size_t effects = 0;
    std::size_t effectParseFailures = 0;
    std::size_t shaders = 0;
    std::size_t vertexShaders = 0;
    std::size_t pixelShaders = 0;
    std::size_t decodeFailures = 0;
    std::size_t translated = 0;
    std::size_t compiled = 0;
    std::size_t compileFailures = 0;
    /// Shaders translated with at least one unsupported instruction.
    std::size_t shadersWithUnsupported = 0;
    /// Pass programs built (both stages linked by semantic) and compiled.
    std::size_t passes = 0;
    std::size_t passesCompiled = 0;
    /// Passes without shaders, which only set render states.
    std::size_t stateOnlyPasses = 0;
    /// Unsupported instruction kinds and how many shaders contain each.
    std::map<std::string, std::size_t> unsupported;
    /// Control flow opcode use across all shaders, by name.
    std::map<std::string, std::size_t> controlFlow;
    /// First messages of each failure kind, for display.
    std::vector<std::string> failures;
};

/// Parses, translates and compiles every effect, shader and pass. `threads` of 0 uses all hardware
/// threads.
CheckReport checkEffects(const Vfs& vfs, const std::vector<std::string>& paths, unsigned threads = 0);

std::string formatReport(const CheckReport& report);

} // namespace openhorizon::xenos
