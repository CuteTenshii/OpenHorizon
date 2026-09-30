#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/// Decoding of Xenos (Xbox 360 GPU) shader microcode.
///
/// A program is an array of big-endian 32-bit words, read here as host
/// integers. It is organised in 96-bit slots of three words. The control flow
/// program comes first: two 48-bit control flow instructions per slot (the
/// first in word 0 and the low half of word 1, the second in the high half of
/// word 1 and word 2). ALU and fetch instructions take one slot each and are
/// addressed by slot index; an `exec` control flow instruction runs up to six
/// consecutive slots, and two bits per slot in its sequence field say whether
/// the slot is a fetch (bit 0) and whether it must wait for earlier results
/// (bit 1). The control flow program ends where the lowest slot any exec
/// refers to begins.
///
/// The bit layouts follow the publicly documented R400/Xenos/Adreno 2xx
/// encodings (AMD R600 ISA document, freedreno's a2xx notes, Xenia's ISA
/// descriptions); every instruction form below occurs in the game's shaders
/// and was checked against them for consistency (verified: exec, alloc,
/// vertex fetch, texture fetch, ALU with vector and scalar halves and exports;
/// the loop, call and jump encodings are from the documentation only).
namespace openhorizon::xenos {

enum class ShaderStage : std::uint8_t {
    Vertex,
    Pixel,
};

std::string_view stageName(ShaderStage stage);

/// Control flow opcodes (bits 44-47 of a control flow instruction).
enum class CfOpcode : std::uint8_t {
    Nop = 0,
    Exec = 1,
    ExecEnd = 2,
    /// Runs the slots if boolean constant `boolAddress` equals `condition`.
    CondExec = 3,
    CondExecEnd = 4,
    /// Runs the slots if the predicate equals `condition`.
    CondExecPred = 5,
    CondExecPredEnd = 6,
    LoopStart = 7,
    LoopEnd = 8,
    CondCall = 9,
    Return = 10,
    CondJmp = 11,
    Alloc = 12,
    /// As CondExec (boolean constant), with the predicate-clean hint set.
    CondExecPredClean = 13,
    CondExecPredCleanEnd = 14,
    MarkVsFetchDone = 15,
};

std::string_view cfOpcodeName(CfOpcode opcode);

/// What an `alloc` reserves.
enum class AllocType : std::uint8_t {
    None = 0,
    /// Vertex position export (vertex shaders).
    Position = 1,
    /// Interpolators (vertex shaders) or colour outputs (pixel shaders).
    Parameters = 2,
    /// Memory export.
    Memory = 3,
};

/// One decoded 48-bit control flow instruction. Only the fields of its
/// opcode's form are meaningful; the rest are zero.
struct CfInstruction {
    CfOpcode opcode = CfOpcode::Nop;
    /// The raw 48 bits.
    std::uint64_t bits = 0;
    /// Exec forms: first slot. Loops, calls, jumps: target control flow
    /// instruction index.
    std::uint32_t address = 0;
    /// Exec forms: number of slots (0-6).
    std::uint32_t count = 0;
    /// Exec forms: two bits per slot, fetch (bit 0) and serialize (bit 1).
    std::uint32_t sequence = 0;
    bool yield = false;
    bool predicateClean = false;
    /// Boolean constant index (0-255) of conditional execs, calls and jumps.
    std::uint32_t boolAddress = 0;
    /// The value the boolean constant or predicate must have.
    bool condition = false;
    /// Relative addressing inside the exec uses a0 instead of aL.
    bool addressAbsolute = false;
    /// Loops: integer (loop) constant index 0-31.
    std::uint32_t loopId = 0;
    /// LoopStart: keep the current aL instead of resetting it.
    bool repeat = false;
    /// LoopEnd: leave the loop when the predicate equals `condition`.
    bool predicatedBreak = false;
    /// CondCall and CondJmp: ignore conditions.
    bool unconditional = false;
    /// CondCall and CondJmp: test the predicate instead of a boolean.
    bool predicated = false;
    /// CondJmp: direction hint (1 = backwards).
    bool backward = false;
    /// Alloc: number of registers minus one, and what is reserved.
    std::uint32_t allocSize = 0;
    AllocType allocType = AllocType::None;

    bool isExec() const;
    /// Whether the shader stops after this instruction when it runs.
    bool endsShader() const;
    /// Exec forms: whether slot `i` of the sequence is a fetch.
    bool slotIsFetch(std::uint32_t i) const { return ((sequence >> (2 * i)) & 1) != 0; }
    /// Exec forms: whether slot `i` waits for earlier results.
    bool slotSerialized(std::uint32_t i) const { return ((sequence >> (2 * i + 1)) & 1) != 0; }
    /// Exec forms that test a boolean constant.
    bool testsBool() const;
    /// Exec forms that test the predicate.
    bool testsPredicate() const;
};

/// Decodes the 48 bits of one control flow instruction.
CfInstruction decodeCf(std::uint64_t bits);

/// The two control flow instructions of slot words `w0..w2`.
std::array<std::uint64_t, 2> unpackCfPair(std::uint32_t w0, std::uint32_t w1, std::uint32_t w2);

/// Fetch opcodes (bits 0-4 of word 0).
enum class FetchOpcode : std::uint8_t {
    VertexFetch = 0,
    TextureFetch = 1,
    GetBorderColorFraction = 16,
    GetComputedLod = 17,
    GetGradients = 18,
    GetWeights = 19,
    SetLod = 24,
    SetGradientsHorizontal = 25,
    SetGradientsVertical = 26,
};

std::string_view fetchOpcodeName(std::uint32_t opcode);

/// Texture dimension of a texture fetch.
enum class TextureDimension : std::uint8_t {
    D1 = 0,
    D2 = 1,
    /// 3D or stacked 2D.
    D3 = 2,
    Cube = 3,
};

std::string_view dimensionName(TextureDimension dimension);

/// Data formats a vertex fetch can read (the GPU surface format numbers).
enum class VertexFormat : std::uint8_t {
    Undefined = 0,
    F8_8_8_8 = 6,
    F2_10_10_10 = 7,
    F10_11_11 = 16,
    F11_11_10 = 17,
    F16_16 = 25,
    F16_16_16_16 = 26,
    F16_16_Float = 31,
    F16_16_16_16_Float = 32,
    F32 = 33,
    F32_32 = 34,
    F32_32_32_32 = 35,
    F32_Float = 36,
    F32_32_Float = 37,
    F32_32_32_32_Float = 38,
    F32_32_32_Float = 57,
};

std::string_view vertexFormatName(VertexFormat format);
/// Size in bytes of one element of `format`, 0 for unknown formats.
std::uint32_t vertexFormatBytes(VertexFormat format);
/// Number of components `format` delivers (the rest read as 0, 0, 1).
std::uint32_t vertexFormatComponents(VertexFormat format);
bool isKnownVertexFormat(std::uint32_t format);

/// Texture filter override in a texture fetch.
enum class TextureFilter : std::uint8_t {
    Point = 0,
    Linear = 1,
    /// Mip filter only: always the base level.
    BaseMap = 2,
    /// Use the fetch constant's setting.
    FromFetchConstant = 3,
};

/// One decoded fetch slot.
struct FetchInstruction {
    std::uint32_t opcode = 0;
    std::array<std::uint32_t, 3> words{};

    std::uint32_t srcRegister = 0;
    bool srcRelative = false;
    std::uint32_t dstRegister = 0;
    bool dstRelative = false;
    /// Four 3-bit selectors, x first: 0-3 source component, 4 = 0.0,
    /// 5 = 1.0, 7 = keep the destination component.
    std::uint32_t dstSwizzle = 0;
    /// Vertex fetch: 2 bits choosing the index component. Texture fetch:
    /// three 2-bit absolute component selectors for the coordinates.
    std::uint32_t srcSwizzle = 0;
    bool predicated = false;
    bool predicateCondition = false;

    /// Vertex fetch constant 0-95 (three per 6-word constant slot), or
    /// texture fetch constant 0-31.
    std::uint32_t constIndex = 0;

    // Vertex fetch fields.
    std::uint32_t format = 0;
    bool isSigned = false;
    /// False for integer (unnormalized) data.
    bool normalized = true;
    /// Signed normalized data uses the "no zero" mapping (2x+1)/(2^n-1)
    /// instead of clamping -2^(n-1) to -1.
    bool signedNoZero = false;
    bool indexRounded = false;
    std::int32_t expAdjust = 0;
    /// Reuses the address of the previous full fetch.
    bool mini = false;
    std::uint32_t prefetchCount = 0;
    /// Stride and offset in 32-bit words.
    std::uint32_t stride = 0;
    std::int32_t offset = 0;

    // Texture fetch fields.
    bool fetchValidOnly = false;
    bool unnormalizedCoordinates = false;
    TextureFilter magFilter = TextureFilter::FromFetchConstant;
    TextureFilter minFilter = TextureFilter::FromFetchConstant;
    TextureFilter mipFilter = TextureFilter::FromFetchConstant;
    /// 0 = off, 1-5 = 1:1 to 16:1, 7 = fetch constant.
    std::uint32_t anisoFilter = 7;
    TextureFilter volMagFilter = TextureFilter::FromFetchConstant;
    TextureFilter volMinFilter = TextureFilter::FromFetchConstant;
    bool useComputedLod = true;
    bool useRegisterLod = false;
    bool useRegisterGradients = false;
    /// Sample at pixel centres (true) or at the sample location.
    bool sampleAtCentre = false;
    float lodBias = 0.0F;
    /// Offsets in texels (half-texel steps).
    float offsetX = 0.0F;
    float offsetY = 0.0F;
    float offsetZ = 0.0F;
    TextureDimension dimension = TextureDimension::D2;

    bool isVertexFetch() const { return opcode == static_cast<std::uint32_t>(FetchOpcode::VertexFetch); }
    /// Destination selector of component `i` (see dstSwizzle).
    std::uint32_t dstSelector(std::uint32_t i) const { return (dstSwizzle >> (3 * i)) & 7; }
    /// Texture fetch: source component used for coordinate `i`.
    std::uint32_t srcComponent(std::uint32_t i) const { return (srcSwizzle >> (2 * i)) & 3; }
};

FetchInstruction decodeFetch(std::uint32_t w0, std::uint32_t w1, std::uint32_t w2);

/// Vector ALU opcodes (bits 24-28 of word 2).
enum class VectorOpcode : std::uint8_t {
    Add = 0,
    Mul = 1,
    Max = 2,
    Min = 3,
    SetEq = 4,
    SetGt = 5,
    SetGe = 6,
    SetNe = 7,
    Frac = 8,
    Trunc = 9,
    Floor = 10,
    MulAdd = 11,
    CndEq = 12,
    CndGe = 13,
    CndGt = 14,
    Dot4 = 15,
    Dot3 = 16,
    Dot2Add = 17,
    Cube = 18,
    Max4 = 19,
    SetpEqPush = 20,
    SetpNePush = 21,
    SetpGtPush = 22,
    SetpGePush = 23,
    KillEq = 24,
    KillGt = 25,
    KillGe = 26,
    KillNe = 27,
    Dst = 28,
    MaxA = 29,
};

/// Scalar ALU opcodes (bits 26-31 of word 0).
enum class ScalarOpcode : std::uint8_t {
    Adds = 0,
    AddsPrev = 1,
    Muls = 2,
    MulsPrev = 3,
    MulsPrev2 = 4,
    Maxs = 5,
    Mins = 6,
    Seqs = 7,
    Sgts = 8,
    Sges = 9,
    Snes = 10,
    Frcs = 11,
    Truncs = 12,
    Floors = 13,
    Exp = 14,
    Logc = 15,
    Log = 16,
    Rcpc = 17,
    Rcpf = 18,
    Rcp = 19,
    Rsqc = 20,
    Rsqf = 21,
    Rsq = 22,
    MaxAs = 23,
    MaxAsf = 24,
    Subs = 25,
    SubsPrev = 26,
    SetpEq = 27,
    SetpNe = 28,
    SetpGt = 29,
    SetpGe = 30,
    SetpInv = 31,
    SetpPop = 32,
    SetpClr = 33,
    SetpRstr = 34,
    KillsEq = 35,
    KillsGt = 36,
    KillsGe = 37,
    KillsNe = 38,
    KillsOne = 39,
    Sqrt = 40,
    Mulsc0 = 42,
    Mulsc1 = 43,
    Addsc0 = 44,
    Addsc1 = 45,
    Subsc0 = 46,
    Subsc1 = 47,
    Sin = 48,
    Cos = 49,
    RetainPrev = 50,
};

/// Name of a vector opcode, or nullptr for an undefined one (30, 31).
const char* vectorOpcodeName(std::uint32_t opcode);
/// Number of sources a defined vector opcode reads (1-3).
std::uint32_t vectorOperandCount(std::uint32_t opcode);
/// Name of a scalar opcode, or nullptr for an undefined one (41, 51-63).
const char* scalarOpcodeName(std::uint32_t opcode);

/// How a defined scalar opcode takes its operands.
enum class ScalarOperands : std::uint8_t {
    /// No operand.
    None,
    /// The `a` component of the third source (the W selector).
    A,
    /// The `a` and `b` components of the third source (W and X selectors).
    AB,
    /// A constant's `a` component and a temporary's `b` component (mulsc,
    /// addsc, subsc).
    ConstantAndTemporary,
};

ScalarOperands scalarOperands(std::uint32_t opcode);

/// Export register numbers of ALU instructions with the export bit set.
namespace exportreg {
/// Vertex shaders: interpolators 0-15.
inline constexpr std::uint32_t kInterpolatorCount = 16;
/// Pixel shaders: colour targets 0-3.
inline constexpr std::uint32_t kColorCount = 4;
inline constexpr std::uint32_t kMemoryAddress = 32;
inline constexpr std::uint32_t kMemoryData0 = 33;
inline constexpr std::uint32_t kMemoryData4 = 37;
/// Pixel shaders: depth in x.
inline constexpr std::uint32_t kDepth = 61;
/// Vertex shaders: clip-space position.
inline constexpr std::uint32_t kPosition = 62;
/// Vertex shaders: point size (x), edge flag (y), kill vertex (z).
inline constexpr std::uint32_t kPointSizeEdgeKill = 63;
} // namespace exportreg

/// One ALU operand after the instruction's addressing rules are applied.
struct AluOperand {
    /// Temporary register (true) or float constant (false).
    bool temporary = true;
    /// Register or constant index (temporaries 0-63, constants 0-255).
    std::uint32_t index = 0;
    /// Index is added to aL (temporaries, or constants in aL mode) or a0
    /// (constants in a0 mode, see `constantUsesA0`).
    bool relative = false;
    bool constantUsesA0 = false;
    bool absolute = false;
    bool negate = false;
    /// Absolute component read for result component x, y, z, w.
    std::array<std::uint8_t, 4> components{0, 1, 2, 3};
};

/// One decoded ALU slot: a vector and a scalar operation issued together.
struct AluInstruction {
    std::array<std::uint32_t, 3> words{};

    std::uint32_t vectorOpcode = 0;
    std::uint32_t scalarOpcode = 0;

    std::uint32_t vectorDest = 0;
    bool vectorDestRelative = false;
    std::uint32_t vectorWriteMask = 0;
    bool vectorClamp = false;

    std::uint32_t scalarDest = 0;
    bool scalarDestRelative = false;
    std::uint32_t scalarWriteMask = 0;
    bool scalarClamp = false;

    /// Both results go to export register `vectorDest`: vector-only mask
    /// bits take the vector result, scalar-only bits the scalar result, both
    /// bits 1.0, and, when `scalarDestRelative` is set, neither bit 0.0.
    bool exportData = false;
    bool predicated = false;
    bool predicateCondition = false;

    bool absConstants = false;
    bool constantsUseA0 = false;
    bool const0Relative = false;
    bool const1Relative = false;

    /// Raw source fields, index 0 = src1.
    std::array<std::uint32_t, 3> srcRegister{};
    std::array<std::uint32_t, 3> srcSwizzle{};
    std::array<bool, 3> srcNegate{};
    std::array<bool, 3> srcTemporary{};

    /// Vector operand `i` (0-2) with addressing applied.
    AluOperand vectorOperand(std::uint32_t i) const;
    /// The scalar operation's third-source operand (A and AB forms), or the
    /// constant operand of the ConstantAndTemporary form.
    AluOperand scalarOperand() const;
    /// The temporary operand of the ConstantAndTemporary form.
    AluOperand scalarTemporaryOperand() const;

    /// Components written by the vector result (for exports: excluding the
    /// ones the scalar result or constants take).
    std::uint32_t vectorResultMask() const;
    std::uint32_t scalarResultMask() const;
    /// Export components set to 1.0 and to 0.0.
    std::uint32_t exportOneMask() const;
    std::uint32_t exportZeroMask() const;
    /// Whether the scalar operation has an effect beyond its result (it
    /// always updates the previous-scalar register except retain_prev).
    bool vectorDoesSomething() const;
    bool scalarDoesSomething() const;
};

AluInstruction decodeAlu(std::uint32_t w0, std::uint32_t w1, std::uint32_t w2);

/// A decoded program: the control flow instructions and the raw slots.
struct Program {
    ShaderStage stage = ShaderStage::Vertex;
    std::vector<std::uint32_t> words;
    /// Control flow instructions in order; the index is the address loops,
    /// calls and jumps use.
    std::vector<CfInstruction> cf;

    std::size_t slotCount() const { return words.size() / 3; }
    std::array<std::uint32_t, 3> slot(std::uint32_t index) const;
};

/// Splits microcode into control flow and instruction slots. Throws
/// DataError when an exec refers past the end, the size is not a whole
/// number of slots, or no instruction ends the program.
Program decodeProgram(ShaderStage stage, std::span<const std::uint32_t> words);

/// Absolute component index read for result component `component` from an
/// ALU source swizzle (each 2-bit field is added to the component index).
constexpr std::uint32_t swizzledComponent(std::uint32_t swizzle, std::uint32_t component)
{
    return ((swizzle >> (2 * component)) + component) & 3;
}

} // namespace openhorizon::xenos
