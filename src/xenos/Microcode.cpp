#include "Microcode.h"

#include "base/Error.h"

#include <algorithm>

namespace openhorizon::xenos {

namespace {

constexpr std::uint32_t bit(std::uint64_t value, unsigned index)
{
    return static_cast<std::uint32_t>((value >> index) & 1);
}

constexpr std::uint32_t field(std::uint64_t value, unsigned first, unsigned width)
{
    return static_cast<std::uint32_t>((value >> first) & ((std::uint64_t{1} << width) - 1));
}

/// Sign-extends the `width`-bit field at `first`.
constexpr std::int32_t signedField(std::uint32_t value, unsigned first, unsigned width)
{
    const std::uint32_t raw = field(value, first, width);
    const std::uint32_t sign = 1U << (width - 1);
    return static_cast<std::int32_t>(raw ^ sign) - static_cast<std::int32_t>(sign);
}

struct VectorOpcodeInfo {
    const char* name;
    std::uint32_t operands;
};

constexpr std::array<VectorOpcodeInfo, 30> kVectorOpcodes{{
    {"add", 2},
    {"mul", 2},
    {"max", 2},
    {"min", 2},
    {"seq", 2},
    {"sgt", 2},
    {"sge", 2},
    {"sne", 2},
    {"frc", 1},
    {"trunc", 1},
    {"floor", 1},
    {"mad", 3},
    {"cndeq", 3},
    {"cndge", 3},
    {"cndgt", 3},
    {"dp4", 2},
    {"dp3", 2},
    {"dp2add", 3},
    {"cube", 2},
    {"max4", 1},
    {"setp_eq_push", 2},
    {"setp_ne_push", 2},
    {"setp_gt_push", 2},
    {"setp_ge_push", 2},
    {"kill_eq", 2},
    {"kill_gt", 2},
    {"kill_ge", 2},
    {"kill_ne", 2},
    {"dst", 2},
    {"maxa", 2},
}};

struct ScalarOpcodeInfo {
    const char* name;
    ScalarOperands operands;
};

constexpr std::array<ScalarOpcodeInfo, 51> kScalarOpcodes{{
    {"adds", ScalarOperands::AB},
    {"adds_prev", ScalarOperands::A},
    {"muls", ScalarOperands::AB},
    {"muls_prev", ScalarOperands::A},
    {"muls_prev2", ScalarOperands::AB},
    {"maxs", ScalarOperands::AB},
    {"mins", ScalarOperands::AB},
    {"seqs", ScalarOperands::A},
    {"sgts", ScalarOperands::A},
    {"sges", ScalarOperands::A},
    {"snes", ScalarOperands::A},
    {"frcs", ScalarOperands::A},
    {"truncs", ScalarOperands::A},
    {"floors", ScalarOperands::A},
    {"exp", ScalarOperands::A},
    {"logc", ScalarOperands::A},
    {"log", ScalarOperands::A},
    {"rcpc", ScalarOperands::A},
    {"rcpf", ScalarOperands::A},
    {"rcp", ScalarOperands::A},
    {"rsqc", ScalarOperands::A},
    {"rsqf", ScalarOperands::A},
    {"rsq", ScalarOperands::A},
    {"maxas", ScalarOperands::AB},
    {"maxasf", ScalarOperands::AB},
    {"subs", ScalarOperands::AB},
    {"subs_prev", ScalarOperands::A},
    {"setp_eq", ScalarOperands::A},
    {"setp_ne", ScalarOperands::A},
    {"setp_gt", ScalarOperands::A},
    {"setp_ge", ScalarOperands::A},
    {"setp_inv", ScalarOperands::A},
    {"setp_pop", ScalarOperands::A},
    {"setp_clr", ScalarOperands::None},
    {"setp_rstr", ScalarOperands::A},
    {"kills_eq", ScalarOperands::A},
    {"kills_gt", ScalarOperands::A},
    {"kills_ge", ScalarOperands::A},
    {"kills_ne", ScalarOperands::A},
    {"kills_one", ScalarOperands::A},
    {"sqrt", ScalarOperands::A},
    {nullptr, ScalarOperands::None},
    {"mulsc", ScalarOperands::ConstantAndTemporary},
    {"mulsc", ScalarOperands::ConstantAndTemporary},
    {"addsc", ScalarOperands::ConstantAndTemporary},
    {"addsc", ScalarOperands::ConstantAndTemporary},
    {"subsc", ScalarOperands::ConstantAndTemporary},
    {"subsc", ScalarOperands::ConstantAndTemporary},
    {"sin", ScalarOperands::A},
    {"cos", ScalarOperands::A},
    {"retain_prev", ScalarOperands::None},
}};

std::array<std::uint8_t, 4> aluComponents(std::uint32_t swizzle)
{
    std::array<std::uint8_t, 4> components{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        components[i] = static_cast<std::uint8_t>(swizzledComponent(swizzle, i));
    }
    return components;
}

} // namespace

std::string_view stageName(ShaderStage stage)
{
    return stage == ShaderStage::Vertex ? "vertex" : "pixel";
}

std::string_view cfOpcodeName(CfOpcode opcode)
{
    switch (opcode) {
    case CfOpcode::Nop:
        return "nop";
    case CfOpcode::Exec:
        return "exec";
    case CfOpcode::ExecEnd:
        return "exece";
    case CfOpcode::CondExec:
        return "cexec";
    case CfOpcode::CondExecEnd:
        return "cexece";
    case CfOpcode::CondExecPred:
        return "pexec";
    case CfOpcode::CondExecPredEnd:
        return "pexece";
    case CfOpcode::LoopStart:
        return "loop";
    case CfOpcode::LoopEnd:
        return "endloop";
    case CfOpcode::CondCall:
        return "ccall";
    case CfOpcode::Return:
        return "ret";
    case CfOpcode::CondJmp:
        return "cjmp";
    case CfOpcode::Alloc:
        return "alloc";
    case CfOpcode::CondExecPredClean:
        return "cexec_pc";
    case CfOpcode::CondExecPredCleanEnd:
        return "cexece_pc";
    case CfOpcode::MarkVsFetchDone:
        return "vfetch_done";
    }
    return "?";
}

bool CfInstruction::isExec() const
{
    switch (opcode) {
    case CfOpcode::Exec:
    case CfOpcode::ExecEnd:
    case CfOpcode::CondExec:
    case CfOpcode::CondExecEnd:
    case CfOpcode::CondExecPred:
    case CfOpcode::CondExecPredEnd:
    case CfOpcode::CondExecPredClean:
    case CfOpcode::CondExecPredCleanEnd:
        return true;
    default:
        return false;
    }
}

bool CfInstruction::endsShader() const
{
    return opcode == CfOpcode::ExecEnd || opcode == CfOpcode::CondExecEnd || opcode == CfOpcode::CondExecPredEnd
        || opcode == CfOpcode::CondExecPredCleanEnd;
}

bool CfInstruction::testsBool() const
{
    return opcode == CfOpcode::CondExec || opcode == CfOpcode::CondExecEnd || opcode == CfOpcode::CondExecPredClean
        || opcode == CfOpcode::CondExecPredCleanEnd;
}

bool CfInstruction::testsPredicate() const
{
    return opcode == CfOpcode::CondExecPred || opcode == CfOpcode::CondExecPredEnd;
}

CfInstruction decodeCf(std::uint64_t bits)
{
    CfInstruction cf;
    cf.bits = bits & 0xFFFF'FFFF'FFFFULL;
    cf.opcode = static_cast<CfOpcode>(field(bits, 44, 4));
    cf.addressAbsolute = bit(bits, 43) != 0;
    switch (cf.opcode) {
    case CfOpcode::Exec:
    case CfOpcode::ExecEnd:
    case CfOpcode::CondExecPred:
    case CfOpcode::CondExecPredEnd:
    case CfOpcode::CondExec:
    case CfOpcode::CondExecEnd:
    case CfOpcode::CondExecPredClean:
    case CfOpcode::CondExecPredCleanEnd:
        cf.address = field(bits, 0, 12);
        cf.count = field(bits, 12, 3);
        cf.yield = bit(bits, 15) != 0;
        cf.sequence = field(bits, 16, 12);
        if (cf.testsBool()) {
            cf.boolAddress = field(bits, 34, 8);
            cf.condition = bit(bits, 42) != 0;
            cf.predicateClean = cf.opcode == CfOpcode::CondExecPredClean || cf.opcode == CfOpcode::CondExecPredCleanEnd;
        } else {
            cf.predicateClean = bit(bits, 41) != 0;
            cf.condition = cf.testsPredicate() && bit(bits, 42) != 0;
        }
        break;
    case CfOpcode::LoopStart:
        cf.address = field(bits, 0, 13);
        cf.repeat = bit(bits, 13) != 0;
        cf.loopId = field(bits, 16, 5);
        break;
    case CfOpcode::LoopEnd:
        cf.address = field(bits, 0, 13);
        cf.loopId = field(bits, 16, 5);
        cf.predicatedBreak = bit(bits, 21) != 0;
        cf.condition = bit(bits, 42) != 0;
        break;
    case CfOpcode::CondCall:
    case CfOpcode::CondJmp:
        cf.address = field(bits, 0, 13);
        cf.unconditional = bit(bits, 13) != 0;
        cf.predicated = bit(bits, 14) != 0;
        cf.backward = cf.opcode == CfOpcode::CondJmp && bit(bits, 33) != 0;
        cf.boolAddress = field(bits, 34, 8);
        cf.condition = bit(bits, 42) != 0;
        break;
    case CfOpcode::Alloc:
        cf.allocSize = field(bits, 0, 3);
        cf.allocType = static_cast<AllocType>(field(bits, 41, 2));
        break;
    case CfOpcode::Nop:
    case CfOpcode::Return:
    case CfOpcode::MarkVsFetchDone:
        break;
    }
    return cf;
}

std::array<std::uint64_t, 2> unpackCfPair(std::uint32_t w0, std::uint32_t w1, std::uint32_t w2)
{
    const std::uint64_t first = w0 | (std::uint64_t{w1 & 0xFFFF} << 32);
    const std::uint64_t second = (w1 >> 16) | (std::uint64_t{w2} << 16);
    return {first, second};
}

std::string_view fetchOpcodeName(std::uint32_t opcode)
{
    switch (static_cast<FetchOpcode>(opcode)) {
    case FetchOpcode::VertexFetch:
        return "vfetch";
    case FetchOpcode::TextureFetch:
        return "tfetch";
    case FetchOpcode::GetBorderColorFraction:
        return "getBCF";
    case FetchOpcode::GetComputedLod:
        return "getCompTexLOD";
    case FetchOpcode::GetGradients:
        return "getGradients";
    case FetchOpcode::GetWeights:
        return "getWeights";
    case FetchOpcode::SetLod:
        return "setTexLOD";
    case FetchOpcode::SetGradientsHorizontal:
        return "setGradientH";
    case FetchOpcode::SetGradientsVertical:
        return "setGradientV";
    }
    return {};
}

std::string_view dimensionName(TextureDimension dimension)
{
    switch (dimension) {
    case TextureDimension::D1:
        return "1D";
    case TextureDimension::D2:
        return "2D";
    case TextureDimension::D3:
        return "3D";
    case TextureDimension::Cube:
        return "Cube";
    }
    return "?";
}

std::string_view vertexFormatName(VertexFormat format)
{
    switch (format) {
    case VertexFormat::Undefined:
        return "undefined";
    case VertexFormat::F8_8_8_8:
        return "8_8_8_8";
    case VertexFormat::F2_10_10_10:
        return "2_10_10_10";
    case VertexFormat::F10_11_11:
        return "10_11_11";
    case VertexFormat::F11_11_10:
        return "11_11_10";
    case VertexFormat::F16_16:
        return "16_16";
    case VertexFormat::F16_16_16_16:
        return "16_16_16_16";
    case VertexFormat::F16_16_Float:
        return "16_16_FLOAT";
    case VertexFormat::F16_16_16_16_Float:
        return "16_16_16_16_FLOAT";
    case VertexFormat::F32:
        return "32";
    case VertexFormat::F32_32:
        return "32_32";
    case VertexFormat::F32_32_32_32:
        return "32_32_32_32";
    case VertexFormat::F32_Float:
        return "32_FLOAT";
    case VertexFormat::F32_32_Float:
        return "32_32_FLOAT";
    case VertexFormat::F32_32_32_32_Float:
        return "32_32_32_32_FLOAT";
    case VertexFormat::F32_32_32_Float:
        return "32_32_32_FLOAT";
    }
    return "unknown";
}

bool isKnownVertexFormat(std::uint32_t format)
{
    switch (static_cast<VertexFormat>(format)) {
    case VertexFormat::F8_8_8_8:
    case VertexFormat::F2_10_10_10:
    case VertexFormat::F10_11_11:
    case VertexFormat::F11_11_10:
    case VertexFormat::F16_16:
    case VertexFormat::F16_16_16_16:
    case VertexFormat::F16_16_Float:
    case VertexFormat::F16_16_16_16_Float:
    case VertexFormat::F32:
    case VertexFormat::F32_32:
    case VertexFormat::F32_32_32_32:
    case VertexFormat::F32_Float:
    case VertexFormat::F32_32_Float:
    case VertexFormat::F32_32_32_32_Float:
    case VertexFormat::F32_32_32_Float:
        return true;
    case VertexFormat::Undefined:
        return false;
    }
    return false;
}

std::uint32_t vertexFormatBytes(VertexFormat format)
{
    switch (format) {
    case VertexFormat::F8_8_8_8:
    case VertexFormat::F2_10_10_10:
    case VertexFormat::F10_11_11:
    case VertexFormat::F11_11_10:
    case VertexFormat::F16_16:
    case VertexFormat::F16_16_Float:
    case VertexFormat::F32:
    case VertexFormat::F32_Float:
        return 4;
    case VertexFormat::F16_16_16_16:
    case VertexFormat::F16_16_16_16_Float:
    case VertexFormat::F32_32:
    case VertexFormat::F32_32_Float:
        return 8;
    case VertexFormat::F32_32_32_Float:
        return 12;
    case VertexFormat::F32_32_32_32:
    case VertexFormat::F32_32_32_32_Float:
        return 16;
    case VertexFormat::Undefined:
        return 0;
    }
    return 0;
}

std::uint32_t vertexFormatComponents(VertexFormat format)
{
    switch (format) {
    case VertexFormat::F32:
    case VertexFormat::F32_Float:
        return 1;
    case VertexFormat::F16_16:
    case VertexFormat::F16_16_Float:
    case VertexFormat::F32_32:
    case VertexFormat::F32_32_Float:
        return 2;
    case VertexFormat::F10_11_11:
    case VertexFormat::F11_11_10:
    case VertexFormat::F32_32_32_Float:
        return 3;
    case VertexFormat::F8_8_8_8:
    case VertexFormat::F2_10_10_10:
    case VertexFormat::F16_16_16_16:
    case VertexFormat::F16_16_16_16_Float:
    case VertexFormat::F32_32_32_32:
    case VertexFormat::F32_32_32_32_Float:
        return 4;
    case VertexFormat::Undefined:
        return 0;
    }
    return 0;
}

FetchInstruction decodeFetch(std::uint32_t w0, std::uint32_t w1, std::uint32_t w2)
{
    FetchInstruction f;
    f.words = {w0, w1, w2};
    f.opcode = field(w0, 0, 5);
    f.srcRegister = field(w0, 5, 6);
    f.srcRelative = bit(w0, 11) != 0;
    f.dstRegister = field(w0, 12, 6);
    f.dstRelative = bit(w0, 18) != 0;
    f.dstSwizzle = field(w1, 0, 12);
    f.predicated = bit(w1, 31) != 0;
    f.predicateCondition = bit(w2, 31) != 0;
    if (f.isVertexFetch()) {
        f.constIndex = field(w0, 20, 5) * 3 + field(w0, 25, 2);
        f.prefetchCount = field(w0, 27, 3);
        f.srcSwizzle = field(w0, 30, 2);
        f.isSigned = bit(w1, 12) != 0;
        f.normalized = bit(w1, 13) == 0;
        f.signedNoZero = bit(w1, 14) != 0;
        f.indexRounded = bit(w1, 15) != 0;
        f.format = field(w1, 16, 6);
        f.expAdjust = signedField(w1, 24, 6);
        f.mini = bit(w1, 30) != 0;
        f.stride = field(w2, 0, 8);
        f.offset = signedField(w2, 8, 23);
        return f;
    }
    f.fetchValidOnly = bit(w0, 19) != 0;
    f.constIndex = field(w0, 20, 5);
    f.unnormalizedCoordinates = bit(w0, 25) != 0;
    f.srcSwizzle = field(w0, 26, 6);
    f.magFilter = static_cast<TextureFilter>(field(w1, 12, 2));
    f.minFilter = static_cast<TextureFilter>(field(w1, 14, 2));
    f.mipFilter = static_cast<TextureFilter>(field(w1, 16, 2));
    f.anisoFilter = field(w1, 18, 3);
    f.volMagFilter = static_cast<TextureFilter>(field(w1, 24, 2));
    f.volMinFilter = static_cast<TextureFilter>(field(w1, 26, 2));
    f.useComputedLod = bit(w1, 28) != 0;
    f.useRegisterLod = bit(w1, 29) != 0;
    f.useRegisterGradients = bit(w2, 0) != 0;
    f.sampleAtCentre = bit(w2, 1) != 0;
    f.lodBias = static_cast<float>(signedField(w2, 2, 7)) / 16.0F;
    f.dimension = static_cast<TextureDimension>(field(w2, 14, 2));
    f.offsetX = static_cast<float>(signedField(w2, 16, 5)) * 0.5F;
    f.offsetY = static_cast<float>(signedField(w2, 21, 5)) * 0.5F;
    f.offsetZ = static_cast<float>(signedField(w2, 26, 5)) * 0.5F;
    return f;
}

const char* vectorOpcodeName(std::uint32_t opcode)
{
    return opcode < kVectorOpcodes.size() ? kVectorOpcodes[opcode].name : nullptr;
}

std::uint32_t vectorOperandCount(std::uint32_t opcode)
{
    return opcode < kVectorOpcodes.size() ? kVectorOpcodes[opcode].operands : 0;
}

const char* scalarOpcodeName(std::uint32_t opcode)
{
    return opcode < kScalarOpcodes.size() ? kScalarOpcodes[opcode].name : nullptr;
}

ScalarOperands scalarOperands(std::uint32_t opcode)
{
    return opcode < kScalarOpcodes.size() ? kScalarOpcodes[opcode].operands : ScalarOperands::None;
}

AluInstruction decodeAlu(std::uint32_t w0, std::uint32_t w1, std::uint32_t w2)
{
    AluInstruction a;
    a.words = {w0, w1, w2};
    a.vectorDest = field(w0, 0, 6);
    a.vectorDestRelative = bit(w0, 6) != 0;
    a.absConstants = bit(w0, 7) != 0;
    a.scalarDest = field(w0, 8, 6);
    a.scalarDestRelative = bit(w0, 14) != 0;
    a.exportData = bit(w0, 15) != 0;
    a.vectorWriteMask = field(w0, 16, 4);
    a.scalarWriteMask = field(w0, 20, 4);
    a.vectorClamp = bit(w0, 24) != 0;
    a.scalarClamp = bit(w0, 25) != 0;
    a.scalarOpcode = field(w0, 26, 6);

    a.srcSwizzle = {field(w1, 16, 8), field(w1, 8, 8), field(w1, 0, 8)};
    a.srcNegate = {bit(w1, 26) != 0, bit(w1, 25) != 0, bit(w1, 24) != 0};
    a.predicateCondition = bit(w1, 27) != 0;
    a.predicated = bit(w1, 28) != 0;
    a.constantsUseA0 = bit(w1, 29) != 0;
    a.const1Relative = bit(w1, 30) != 0;
    a.const0Relative = bit(w1, 31) != 0;

    a.srcRegister = {field(w2, 16, 8), field(w2, 8, 8), field(w2, 0, 8)};
    a.vectorOpcode = field(w2, 24, 5);
    a.srcTemporary = {bit(w2, 31) != 0, bit(w2, 30) != 0, bit(w2, 29) != 0};
    return a;
}

AluOperand AluInstruction::vectorOperand(std::uint32_t i) const
{
    AluOperand op;
    const std::uint32_t reg = srcRegister[i];
    op.temporary = srcTemporary[i];
    op.negate = srcNegate[i];
    op.components = aluComponents(srcSwizzle[i]);
    if (op.temporary) {
        op.index = reg & 0x3F;
        op.relative = (reg & 0x40) != 0;
        op.absolute = (reg & 0x80) != 0;
        return op;
    }
    op.index = reg;
    op.absolute = absConstants;
    op.constantUsesA0 = constantsUseA0;
    // The first constant among the operands uses const0Relative and any later
    // one const1Relative, whichever source slots they are in.
    switch (i) {
    case 0:
        op.relative = const0Relative;
        break;
    case 1:
        op.relative = srcTemporary[0] ? const0Relative : const1Relative;
        break;
    default:
        op.relative = (srcTemporary[0] && srcTemporary[1]) ? const0Relative : const1Relative;
        break;
    }
    return op;
}

AluOperand AluInstruction::scalarOperand() const
{
    if (scalarOperands(scalarOpcode) != ScalarOperands::ConstantAndTemporary) {
        return vectorOperand(2);
    }
    AluOperand op = vectorOperand(2);
    op.temporary = false;
    op.index = srcRegister[2];
    op.absolute = absConstants;
    op.constantUsesA0 = constantsUseA0;
    op.relative = (srcTemporary[0] && srcTemporary[1]) ? const0Relative : const1Relative;
    return op;
}

AluOperand AluInstruction::scalarTemporaryOperand() const
{
    AluOperand op;
    op.temporary = true;
    // The register number is spread over the opcode's low bit, the third
    // source's select bit and the unused middle of its swizzle.
    op.index = (scalarOpcode & 1) | ((srcTemporary[2] ? 1U : 0U) << 1) | (srcSwizzle[2] & 0x3C);
    op.absolute = absConstants;
    op.negate = srcNegate[2];
    op.components = aluComponents(srcSwizzle[2]);
    return op;
}

std::uint32_t AluInstruction::vectorResultMask() const
{
    return exportData ? (vectorWriteMask & ~scalarWriteMask) : vectorWriteMask;
}

std::uint32_t AluInstruction::scalarResultMask() const
{
    return exportData ? (scalarWriteMask & ~vectorWriteMask) : scalarWriteMask;
}

std::uint32_t AluInstruction::exportOneMask() const
{
    return exportData ? (vectorWriteMask & scalarWriteMask) : 0;
}

std::uint32_t AluInstruction::exportZeroMask() const
{
    return (exportData && scalarDestRelative) ? (0xF & ~(vectorWriteMask | scalarWriteMask)) : 0;
}

bool AluInstruction::vectorDoesSomething() const
{
    if (vectorResultMask() != 0 || exportOneMask() != 0 || exportZeroMask() != 0) {
        return true;
    }
    const auto op = static_cast<VectorOpcode>(vectorOpcode);
    return (op >= VectorOpcode::SetpEqPush && op <= VectorOpcode::KillNe) || op == VectorOpcode::MaxA;
}

bool AluInstruction::scalarDoesSomething() const
{
    return static_cast<ScalarOpcode>(scalarOpcode) != ScalarOpcode::RetainPrev || scalarResultMask() != 0;
}

std::array<std::uint32_t, 3> Program::slot(std::uint32_t index) const
{
    const std::size_t at = static_cast<std::size_t>(index) * 3;
    if (at + 3 > words.size()) {
        throw DataError("microcode slot {} is past the end ({} slots)", index, slotCount());
    }
    return {words[at], words[at + 1], words[at + 2]};
}

Program decodeProgram(ShaderStage stage, std::span<const std::uint32_t> words)
{
    if (words.empty() || words.size() % 3 != 0) {
        throw DataError("microcode of {} words is not a whole number of 3-word slots", words.size());
    }
    Program program;
    program.stage = stage;
    program.words.assign(words.begin(), words.end());
    std::size_t limit = program.slotCount();
    bool ends = false;
    for (std::size_t slot = 0; slot < limit; ++slot) {
        const auto pair = unpackCfPair(words[slot * 3], words[slot * 3 + 1], words[slot * 3 + 2]);
        for (const std::uint64_t bits : pair) {
            CfInstruction cf = decodeCf(bits);
            if (cf.isExec() && cf.count > 0) {
                if (cf.address + cf.count > program.slotCount()) {
                    throw DataError("control flow {} runs slots {}-{} of {}", program.cf.size(), cf.address,
                        cf.address + cf.count - 1, program.slotCount());
                }
                if (cf.address <= slot) {
                    throw DataError("control flow {} runs slot {} inside the control flow program", program.cf.size(),
                        cf.address);
                }
                limit = std::min<std::size_t>(limit, cf.address);
            }
            ends = ends || cf.endsShader();
            program.cf.push_back(cf);
        }
    }
    if (!ends) {
        throw DataError("no control flow instruction ends the program");
    }
    const std::size_t cfCount = program.cf.size();
    for (std::size_t i = 0; i < cfCount; ++i) {
        const CfInstruction& cf = program.cf[i];
        const bool targetsCf = cf.opcode == CfOpcode::LoopStart || cf.opcode == CfOpcode::LoopEnd
            || cf.opcode == CfOpcode::CondCall || cf.opcode == CfOpcode::CondJmp;
        if (targetsCf && cf.address > cfCount) {
            throw DataError("control flow {} ({}) targets {} of {}", i, cfOpcodeName(cf.opcode), cf.address, cfCount);
        }
    }
    return program;
}

} // namespace openhorizon::xenos
