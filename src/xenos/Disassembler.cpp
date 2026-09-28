#include "Disassembler.h"

#include "Effect.h"

#include "base/Error.h"

#include <format>

namespace fh::xenos {

namespace {

constexpr std::string_view kComponents = "xyzw";

std::string maskText(std::uint32_t mask)
{
    std::string text = ".";
    for (std::uint32_t i = 0; i < 4; ++i) {
        text.push_back(((mask >> i) & 1) != 0 ? kComponents[i] : '_');
    }
    return text;
}

std::string exportName(std::uint32_t reg, ShaderStage stage)
{
    if (stage == ShaderStage::Vertex) {
        if (reg == exportreg::kPosition) {
            return "oPos";
        }
        if (reg == exportreg::kPointSizeEdgeKill) {
            return "oPts";
        }
        if (reg < exportreg::kInterpolatorCount) {
            return std::format("o{}", reg);
        }
    } else {
        if (reg == exportreg::kDepth) {
            return "oDepth";
        }
        if (reg < exportreg::kColorCount) {
            return std::format("oC{}", reg);
        }
    }
    if (reg == exportreg::kMemoryAddress) {
        return "eA";
    }
    if (reg >= exportreg::kMemoryData0 && reg <= exportreg::kMemoryData4) {
        return std::format("eM{}", reg - exportreg::kMemoryData0);
    }
    return std::format("export{}", reg);
}

std::string operandText(const AluOperand& op, std::uint32_t componentCount, bool scalarPair)
{
    std::string base;
    if (op.temporary) {
        base = op.relative ? std::format("r[{}+aL]", op.index) : std::format("r{}", op.index);
    } else if (op.relative) {
        base = std::format("c[{}+{}]", op.index, op.constantUsesA0 ? "a0" : "aL");
    } else {
        base = std::format("c{}", op.index);
    }
    if (op.absolute) {
        base = "|" + base + "|";
    }
    if (op.negate) {
        base = "-" + base;
    }
    std::string swizzle = ".";
    if (scalarPair) {
        // Scalar operations read a from the W selector and b from the X one.
        swizzle.push_back(kComponents[op.components[3]]);
        if (componentCount > 1) {
            swizzle.push_back(kComponents[op.components[0]]);
        }
    } else {
        for (std::uint32_t i = 0; i < 4; ++i) {
            swizzle.push_back(kComponents[op.components[i]]);
        }
        if (swizzle == ".xyzw") {
            swizzle.clear();
        }
    }
    return base + swizzle;
}

std::string predicateText(bool predicated, bool condition)
{
    if (!predicated) {
        return {};
    }
    return condition ? "(p0) " : "(!p0) ";
}

std::string filterText(TextureFilter filter)
{
    switch (filter) {
    case TextureFilter::Point:
        return "point";
    case TextureFilter::Linear:
        return "linear";
    case TextureFilter::BaseMap:
        return "basemap";
    case TextureFilter::FromFetchConstant:
        return "keep";
    }
    return "?";
}

} // namespace

std::string disassembleCf(const CfInstruction& cf)
{
    std::string text(cfOpcodeName(cf.opcode));
    if (cf.isExec()) {
        std::string sequence;
        for (std::uint32_t i = 0; i < cf.count; ++i) {
            sequence.push_back(cf.slotIsFetch(i) ? 'F' : 'A');
            if (cf.slotSerialized(i)) {
                sequence.push_back('s');
            }
        }
        if (cf.testsBool()) {
            text += std::format(" {}b{}", cf.condition ? "" : "!", cf.boolAddress);
        } else if (cf.testsPredicate()) {
            text += cf.condition ? " p0" : " !p0";
        }
        text += std::format(" slots {}-{} [{}]", cf.address, cf.address + cf.count - (cf.count > 0 ? 1 : 0), sequence);
        if (cf.addressAbsolute) {
            text += " a0";
        }
        if (cf.predicateClean) {
            text += " pclean";
        }
        if (cf.yield) {
            text += " yield";
        }
        return text;
    }
    switch (cf.opcode) {
    case CfOpcode::LoopStart:
        text += std::format(" i{} skip-to {}{}", cf.loopId, cf.address, cf.repeat ? " repeat" : "");
        break;
    case CfOpcode::LoopEnd:
        text += std::format(" i{} body {}", cf.loopId, cf.address);
        if (cf.predicatedBreak) {
            text += cf.condition ? " break-if p0" : " break-if !p0";
        }
        break;
    case CfOpcode::CondCall:
    case CfOpcode::CondJmp:
        if (cf.unconditional) {
            text += std::format(" {}", cf.address);
        } else if (cf.predicated) {
            text += std::format(" {} {}", cf.condition ? "p0" : "!p0", cf.address);
        } else {
            text += std::format(" {}b{} {}", cf.condition ? "" : "!", cf.boolAddress, cf.address);
        }
        break;
    case CfOpcode::Alloc: {
        static constexpr std::array<std::string_view, 4> kTypes{"none", "position", "interpolators/colors", "memory"};
        text += std::format(" {} size={}", kTypes[static_cast<std::size_t>(cf.allocType)], cf.allocSize + 1);
        break;
    }
    default:
        break;
    }
    return text;
}

std::string disassembleFetch(const FetchInstruction& fetch)
{
    std::string text = predicateText(fetch.predicated, fetch.predicateCondition);
    const std::string_view name = fetchOpcodeName(fetch.opcode);
    text += name.empty() ? std::format("fetch{}", fetch.opcode) : std::string(name);
    std::string dst = fetch.dstRelative ? std::format("r[{}+aL]", fetch.dstRegister) : std::format("r{}", fetch.dstRegister);
    std::string dstSwizzle = ".";
    for (std::uint32_t i = 0; i < 4; ++i) {
        static constexpr std::string_view kSelectors = "xyzw01?_";
        dstSwizzle.push_back(kSelectors[fetch.dstSelector(i)]);
    }
    const std::string src
        = fetch.srcRelative ? std::format("r[{}+aL]", fetch.srcRegister) : std::format("r{}", fetch.srcRegister);
    if (fetch.isVertexFetch()) {
        text += fetch.mini ? "_mini" : "_full";
        text += std::format(" {}{}, {}.{}, vf{}", dst, dstSwizzle, src, kComponents[fetch.srcSwizzle & 3],
            fetch.constIndex);
        text += std::format(" format={} {} {}", vertexFormatName(static_cast<VertexFormat>(fetch.format)),
            fetch.isSigned ? "signed" : "unsigned", fetch.normalized ? "normalized" : "integer");
        text += std::format(" offset={} stride={}", fetch.offset, fetch.stride);
        if (fetch.expAdjust != 0) {
            text += std::format(" exp={}", fetch.expAdjust);
        }
        if (fetch.indexRounded) {
            text += " round";
        }
        return text;
    }
    std::string coords = ".";
    for (std::uint32_t i = 0; i < 3; ++i) {
        coords.push_back(kComponents[fetch.srcComponent(i)]);
    }
    const auto op = static_cast<FetchOpcode>(fetch.opcode);
    if (op == FetchOpcode::SetLod || op == FetchOpcode::SetGradientsHorizontal
        || op == FetchOpcode::SetGradientsVertical) {
        return text + std::format(" {}{}", src, coords);
    }
    text += std::format(" {}{}, {}{}, tf{} {}", dst, dstSwizzle, src, coords, fetch.constIndex,
        dimensionName(fetch.dimension));
    if (fetch.unnormalizedCoordinates) {
        text += " unnormalized";
    }
    if (fetch.magFilter != TextureFilter::FromFetchConstant) {
        text += " mag=" + filterText(fetch.magFilter);
    }
    if (fetch.minFilter != TextureFilter::FromFetchConstant) {
        text += " min=" + filterText(fetch.minFilter);
    }
    if (fetch.mipFilter != TextureFilter::FromFetchConstant) {
        text += " mip=" + filterText(fetch.mipFilter);
    }
    if (fetch.anisoFilter != 7) {
        text += std::format(" aniso={}", fetch.anisoFilter);
    }
    if (!fetch.useComputedLod) {
        text += " nocomplod";
    }
    if (fetch.useRegisterLod) {
        text += " reglod";
    }
    if (fetch.useRegisterGradients) {
        text += " reggrad";
    }
    if (fetch.lodBias != 0.0F) {
        text += std::format(" bias={}", fetch.lodBias);
    }
    if (fetch.offsetX != 0.0F || fetch.offsetY != 0.0F || fetch.offsetZ != 0.0F) {
        text += std::format(" offset=({},{},{})", fetch.offsetX, fetch.offsetY, fetch.offsetZ);
    }
    return text;
}

std::string disassembleAlu(const AluInstruction& alu, ShaderStage stage)
{
    std::string text = predicateText(alu.predicated, alu.predicateCondition);
    const char* vectorName = vectorOpcodeName(alu.vectorOpcode);
    const char* scalarName = scalarOpcodeName(alu.scalarOpcode);

    std::string vectorPart;
    if (alu.vectorDoesSomething()) {
        std::string dest;
        if (alu.exportData) {
            dest = exportName(alu.vectorDest, stage);
        } else {
            dest = alu.vectorDestRelative ? std::format("r[{}+aL]", alu.vectorDest) : std::format("r{}", alu.vectorDest);
        }
        vectorPart = std::format("{}{} {}{}", vectorName != nullptr ? vectorName : "vop?",
            alu.vectorClamp ? "_sat" : "", dest, maskText(alu.vectorResultMask()));
        if (vectorName == nullptr) {
            vectorPart += std::format(" (opcode {})", alu.vectorOpcode);
        }
        const std::uint32_t operands = vectorOperandCount(alu.vectorOpcode);
        for (std::uint32_t i = 0; i < operands; ++i) {
            vectorPart += ", " + operandText(alu.vectorOperand(i), 4, false);
        }
        if (alu.exportOneMask() != 0) {
            vectorPart += " one" + maskText(alu.exportOneMask());
        }
        if (alu.exportZeroMask() != 0) {
            vectorPart += " zero" + maskText(alu.exportZeroMask());
        }
    }

    std::string scalarPart;
    if (alu.scalarDoesSomething()) {
        std::string dest;
        if (alu.exportData) {
            dest = exportName(alu.vectorDest, stage);
        } else {
            dest = alu.scalarDestRelative ? std::format("r[{}+aL]", alu.scalarDest) : std::format("r{}", alu.scalarDest);
        }
        scalarPart = std::format("{}{} {}{}", scalarName != nullptr ? scalarName : "sop?",
            alu.scalarClamp ? "_sat" : "", dest, maskText(alu.scalarResultMask()));
        if (scalarName == nullptr) {
            scalarPart += std::format(" (opcode {})", alu.scalarOpcode);
        }
        switch (scalarOperands(alu.scalarOpcode)) {
        case ScalarOperands::None:
            break;
        case ScalarOperands::A:
            scalarPart += ", " + operandText(alu.scalarOperand(), 1, true);
            break;
        case ScalarOperands::AB:
            scalarPart += ", " + operandText(alu.scalarOperand(), 2, true);
            break;
        case ScalarOperands::ConstantAndTemporary: {
            AluOperand constant = alu.scalarOperand();
            scalarPart += ", " + operandText(constant, 1, true);
            AluOperand temporary = alu.scalarTemporaryOperand();
            temporary.components[3] = temporary.components[0];
            scalarPart += ", " + operandText(temporary, 1, true);
            break;
        }
        }
    }
    if (vectorPart.empty() && scalarPart.empty()) {
        return text + "nop";
    }
    if (vectorPart.empty()) {
        return text + scalarPart;
    }
    if (scalarPart.empty()) {
        return text + vectorPart;
    }
    return text + vectorPart + "  +  " + scalarPart;
}

std::string disassemble(const Program& program)
{
    std::string out = std::format("; {} shader, {} slots, {} control flow instructions\n", stageName(program.stage),
        program.slotCount(), program.cf.size());
    for (std::size_t i = 0; i < program.cf.size(); ++i) {
        const CfInstruction& cf = program.cf[i];
        out += std::format("{:3}  {}\n", i, disassembleCf(cf));
        if (!cf.isExec()) {
            continue;
        }
        for (std::uint32_t k = 0; k < cf.count; ++k) {
            const std::uint32_t index = cf.address + k;
            const auto words = program.slot(index);
            const std::string body = cf.slotIsFetch(k) ? disassembleFetch(decodeFetch(words[0], words[1], words[2]))
                                                       : disassembleAlu(decodeAlu(words[0], words[1], words[2]),
                                                             program.stage);
            out += std::format("       {:3}  {:08x} {:08x} {:08x}  {}\n", index, words[0], words[1], words[2], body);
        }
    }
    return out;
}

std::string describeShader(const Shader& shader)
{
    std::string out = std::format("{} shader at 0x{:x}: {} microcode words, program control 0x{:08x}, "
                                  "context 0x{:x}, interface hash 0x{:x}\n",
        stageName(shader.stage), shader.fileOffset, shader.microcode.size(), shader.programControl, shader.contextMisc,
        shader.interfaceHash);
    if (!shader.debugPath.empty()) {
        out += std::format("  debug info: {}\n", shader.debugPath);
    }
    for (const VertexElementUse& e : shader.vertexElements) {
        out += std::format("  input  {}{} fetched by slot {}\n", usageName(e.usage), e.usageIndex, e.fetchSlot);
    }
    for (const Interpolator& i : shader.interpolators) {
        out += std::format("  interp {}{} register {} mask{}\n", usageName(i.usage), i.usageIndex, i.reg,
            maskText(i.mask));
    }
    if (const auto reg = shader.parameterRegister()) {
        out += std::format("  pixel parameters (position, facing) in r{}\n", *reg);
    }
    out += std::format("  constant table ({}, {} entries):\n", shader.constants.target, shader.constants.constants.size());
    for (const ConstantInfo& c : shader.constants.constants) {
        const char prefix = c.set == RegisterSet::Bool ? 'b'
            : c.set == RegisterSet::Int4                ? 'i'
            : c.set == RegisterSet::Sampler             ? 's'
                                                        : 'c';
        out += std::format("    {}{:<3} x{:<3} {:<12} {}\n", prefix, c.registerIndex, c.registerCount, c.typeName(),
            c.name);
    }
    for (const LiteralConstant& l : shader.literals) {
        out += std::format("  literal c{} = ({}, {}, {}, {})\n", l.reg, l.value[0], l.value[1], l.value[2], l.value[3]);
    }
    try {
        out += disassemble(decodeProgram(shader.stage, shader.microcode));
    } catch (const DataError& e) {
        out += std::format("  microcode does not decode: {}\n", e.what());
    }
    return out;
}

} // namespace fh::xenos
