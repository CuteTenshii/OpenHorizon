#include "ShaderCheck.h"

#include "Effect.h"
#include "GlslTranslator.h"
#include "ShaderProgram.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "vfs/Vfs.h"

#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <thread>

namespace openhorizon::xenos {

namespace {

constexpr std::size_t kMaxFailureMessages = 20;
constexpr std::size_t kMaxMessageLength = 600;

void merge(CheckReport& into, const CheckReport& from)
{
    into.effects += from.effects;
    into.effectParseFailures += from.effectParseFailures;
    into.shaders += from.shaders;
    into.vertexShaders += from.vertexShaders;
    into.pixelShaders += from.pixelShaders;
    into.decodeFailures += from.decodeFailures;
    into.translated += from.translated;
    into.compiled += from.compiled;
    into.compileFailures += from.compileFailures;
    into.shadersWithUnsupported += from.shadersWithUnsupported;
    into.passes += from.passes;
    into.passesCompiled += from.passesCompiled;
    into.stateOnlyPasses += from.stateOnlyPasses;
    for (const auto& [what, count] : from.unsupported) {
        into.unsupported[what] += count;
    }
    for (const auto& [what, count] : from.controlFlow) {
        into.controlFlow[what] += count;
    }
    for (const std::string& message : from.failures) {
        if (into.failures.size() < kMaxFailureMessages) {
            into.failures.push_back(message);
        }
    }
}

void fail(CheckReport& report, std::string message)
{
    if (message.size() > kMaxMessageLength) {
        message.resize(kMaxMessageLength);
        message += "...";
    }
    if (report.failures.size() < kMaxFailureMessages) {
        report.failures.push_back(std::move(message));
    }
}

void checkEffect(const Vfs& vfs, const std::string& path, CheckReport& report)
{
    ++report.effects;
    Effect effect;
    try {
        const Bytes data = vfs.readOrThrow(path);
        effect = parseEffect(data, path);
    } catch (const DataError& e) {
        ++report.effectParseFailures;
        fail(report, std::format("parse {}: {}", path, e.what()));
        return;
    }
    for (std::size_t i = 0; i < effect.shaders.size(); ++i) {
        const Shader& shader = effect.shaders[i];
        ++report.shaders;
        ++(shader.stage == ShaderStage::Vertex ? report.vertexShaders : report.pixelShaders);
        Program program;
        try {
            program = decodeProgram(shader.stage, shader.microcode);
        } catch (const DataError& e) {
            ++report.decodeFailures;
            fail(report, std::format("decode {} shader {}: {}", path, i, e.what()));
            continue;
        }
        for (const CfInstruction& cf : program.cf) {
            ++report.controlFlow[std::string(cfOpcodeName(cf.opcode))];
        }
        const std::optional<VertexDeclaration> declaration
            = shader.stage == ShaderStage::Vertex ? effect.inputDeclaration(shader) : std::nullopt;
        const TranslationResult result
            = translate(program, defaultOptions(shader, declaration ? &*declaration : nullptr));
        ++report.translated;
        if (!result.unsupported.empty()) {
            ++report.shadersWithUnsupported;
            for (const std::string& what : result.unsupported) {
                ++report.unsupported[what];
            }
        }
        const auto error = compileGlsl(result.glsl, shader.stage, std::format("{}#{}", path, i));
        if (error) {
            ++report.compileFailures;
            fail(report, std::format("compile {} shader {}: {}", path, i, *error));
        } else {
            ++report.compiled;
        }
    }
    for (const Technique& technique : effect.techniques) {
        for (const Pass& pass : technique.passes) {
            if (pass.vertexShader < 0 && pass.pixelShader < 0) {
                // A pass that only restores render states ("Cleanup").
                ++report.stateOnlyPasses;
                continue;
            }
            ++report.passes;
            try {
                const ShaderProgramSource program = buildProgram(effect, pass);
                const auto vertexError = compileGlsl(program.vertexGlsl, ShaderStage::Vertex, path);
                const auto pixelError = compileGlsl(program.fragmentGlsl, ShaderStage::Pixel, path);
                if (!vertexError && !pixelError) {
                    ++report.passesCompiled;
                } else {
                    fail(report, std::format("pass {}/{}/{}: {}", path, technique.name, pass.name,
                        vertexError ? *vertexError : *pixelError));
                }
            } catch (const DataError& e) {
                fail(report, std::format("pass {}/{}/{}: {}", path, technique.name, pass.name, e.what()));
            }
        }
    }
}

} // namespace

std::optional<std::string> compileGlsl(std::string_view glsl, ShaderStage stage, std::string_view name)
{
    const shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);
    options.SetSourceLanguage(shaderc_source_language_glsl);
    const shaderc_shader_kind kind
        = stage == ShaderStage::Vertex ? shaderc_glsl_vertex_shader : shaderc_glsl_fragment_shader;
    const shaderc::SpvCompilationResult result
        = compiler.CompileGlslToSpv(glsl.data(), glsl.size(), kind, std::string(name).c_str(), options);
    if (result.GetCompilationStatus() == shaderc_compilation_status_success) {
        return std::nullopt;
    }
    return result.GetErrorMessage();
}

std::vector<std::string> listEffects(const Vfs& vfs)
{
    std::vector<std::string> effects;
    for (const std::string& path : vfs.list("media")) {
        if (endsWithNoCase(path, ".fxobj")) {
            effects.push_back(path);
        }
    }
    return effects;
}

CheckReport checkEffects(const Vfs& vfs, const std::vector<std::string>& paths, unsigned threads)
{
    if (threads == 0) {
        threads = std::max(1U, std::thread::hardware_concurrency());
    }
    CheckReport total;
    std::mutex mutex;
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        while (true) {
            const std::size_t index = next.fetch_add(1);
            if (index >= paths.size()) {
                return;
            }
            CheckReport one;
            checkEffect(vfs, paths[index], one);
            const std::lock_guard lock(mutex);
            merge(total, one);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back(worker);
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    return total;
}

std::string formatReport(const CheckReport& report)
{
    std::string out;
    out += std::format("effects:            {} ({} failed to parse)\n", report.effects, report.effectParseFailures);
    out += std::format("shaders:            {} ({} vertex, {} pixel)\n", report.shaders, report.vertexShaders,
        report.pixelShaders);
    out += std::format("microcode decoded:  {} ({} failed)\n", report.shaders - report.decodeFailures,
        report.decodeFailures);
    out += std::format("translated:         {}\n", report.translated);
    out += std::format("compiled:           {} ({} failed)\n", report.compiled, report.compileFailures);
    out += std::format("with unsupported:   {}\n", report.shadersWithUnsupported);
    out += std::format("pass programs:      {} built and compiled of {} ({} state-only passes skipped)\n",
        report.passesCompiled, report.passes, report.stateOnlyPasses);
    if (!report.unsupported.empty()) {
        out += "unsupported instructions (shaders containing each):\n";
        for (const auto& [what, count] : report.unsupported) {
            out += std::format("  {:6}  {}\n", count, what);
        }
    }
    out += "control flow instructions used:\n";
    for (const auto& [what, count] : report.controlFlow) {
        out += std::format("  {:8}  {}\n", count, what);
    }
    if (!report.failures.empty()) {
        out += "first failures:\n";
        for (const std::string& message : report.failures) {
            out += "  " + message + "\n";
        }
    }
    return out;
}

} // namespace openhorizon::xenos
