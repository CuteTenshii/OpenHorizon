// Lists, disassembles, translates and checks the game's compiled effects
// (.fxobj): the Xenos shaders, their constant tables and the GLSL the
// engine generates from them.

#include "base/Log.h"
#include "vfs/Vfs.h"
#include "xenos/Disassembler.h"
#include "xenos/Effect.h"
#include "xenos/GlslTranslator.h"
#include "xenos/ShaderCheck.h"
#include "xenos/ShaderProgram.h"

#include <cstdio>
#include <format>
#include <string>

namespace {

namespace xenos = fh::xenos;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1shader DISC list\n"
        "       fh1shader DISC dis PATH\n"
        "       fh1shader DISC glsl PATH\n"
        "       fh1shader DISC check\n");
    return 2;
}

void print(const std::string& text)
{
    std::fwrite(text.data(), 1, text.size(), stdout);
}

int list(const fh::Vfs& vfs)
{
    std::size_t total = 0;
    for (const std::string& path : xenos::listEffects(vfs)) {
        try {
            const xenos::Effect effect = xenos::parseEffect(vfs.readOrThrow(path), path);
            std::size_t vertex = 0;
            for (const xenos::Shader& shader : effect.shaders) {
                vertex += shader.stage == xenos::ShaderStage::Vertex ? 1 : 0;
            }
            total += effect.shaders.size();
            print(std::format("{}  techniques {}  vertex {}  pixel {}\n", path, effect.techniques.size(), vertex,
                effect.shaders.size() - vertex));
        } catch (const fh::DataError& e) {
            print(std::format("{}  error: {}\n", path, e.what()));
        }
    }
    print(std::format("{} shaders\n", total));
    return 0;
}

void printEffectSummary(const xenos::Effect& effect)
{
    for (const xenos::Technique& technique : effect.techniques) {
        print(std::format("technique {}\n", technique.name));
        for (const xenos::Pass& pass : technique.passes) {
            print(std::format("  pass {}: vertex shader {}, pixel shader {}\n", pass.name, pass.vertexShader,
                pass.pixelShader));
        }
    }
    for (std::size_t d = 0; d < effect.declarations.size(); ++d) {
        print(std::format("vertex declaration {}\n", d));
        for (const xenos::VertexDeclElement& e : effect.declarations[d].elements) {
            print(std::format("  stream {} +{:<3} {}{:<2} {:<18} type 0x{:06x} {}\n", e.stream, e.offset,
                xenos::usageName(e.usage), e.usageIndex, xenos::vertexFormatName(e.format()), e.type, e.name));
        }
    }
}

int disassemble(const fh::Vfs& vfs, const std::string& path)
{
    const xenos::Effect effect = xenos::parseEffect(vfs.readOrThrow(path), path);
    printEffectSummary(effect);
    for (std::size_t i = 0; i < effect.shaders.size(); ++i) {
        print(std::format("\n#{} {}", i, xenos::describeShader(effect.shaders[i])));
    }
    return 0;
}

int glsl(const fh::Vfs& vfs, const std::string& path)
{
    const xenos::Effect effect = xenos::parseEffect(vfs.readOrThrow(path), path);
    for (const xenos::Technique& technique : effect.techniques) {
        for (const xenos::Pass& pass : technique.passes) {
            print(std::format("//// technique {} pass {}\n", technique.name, pass.name));
            try {
                const xenos::ShaderProgramSource program = xenos::buildProgram(effect, pass);
                print("//// constants\n");
                for (const xenos::ConstantBinding& c : program.constants) {
                    print(std::format("//   {:<40} {:<6} {:<7} {} x{}\n", c.name, xenos::stageName(c.stage),
                        xenos::registerSetName(c.set), c.index, c.count));
                }
                print("//// samplers\n");
                for (const xenos::SamplerBinding& s : program.samplers) {
                    print(std::format("//   {:<40} unit {} {} {}\n", s.name, s.unit, xenos::dimensionName(s.dimension),
                        s.glslName));
                }
                print("//// vertex input\n");
                for (const xenos::VertexInputElement& e : program.vertexInput.elements) {
                    print(std::format("//   {:<20} {}{} stream {} +{} {}\n", e.name, xenos::usageName(e.usage),
                        e.usageIndex, e.stream, e.byteOffset, xenos::vertexFormatName(e.format)));
                }
                for (const std::string& what : program.unsupported) {
                    print(std::format("//// unsupported: {}\n", what));
                }
                print("//// vertex shader\n" + program.vertexGlsl);
                print("//// fragment shader\n" + program.fragmentGlsl + "\n");
            } catch (const fh::DataError& e) {
                print(std::format("//// error: {}\n", e.what()));
            }
        }
    }
    return 0;
}

int check(const fh::Vfs& vfs)
{
    const std::vector<std::string> paths = xenos::listEffects(vfs);
    const xenos::CheckReport report = xenos::checkEffects(vfs, paths);
    print(xenos::formatReport(report));
    const bool clean = report.effectParseFailures == 0 && report.decodeFailures == 0 && report.compileFailures == 0
        && report.passesCompiled == report.passes;
    return clean ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        return usage();
    }
    try {
        fh::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::string command = argv[2];
        if (command == "list") {
            return list(vfs);
        }
        if (command == "check") {
            return check(vfs);
        }
        if (argc < 4) {
            return usage();
        }
        if (command == "dis") {
            return disassemble(vfs, argv[3]);
        }
        if (command == "glsl") {
            return glsl(vfs, argv[3]);
        }
        return usage();
    } catch (const std::exception& e) {
        fh::logError("{}", e.what());
        return 1;
    }
}
