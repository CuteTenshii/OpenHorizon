#include "GlslTranslator.h"

#include "base/Error.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <set>

namespace fh::xenos {

namespace {

constexpr std::string_view kComponents = "xyzw";
constexpr std::uint32_t kMaxTemporaries = 64;
constexpr std::uint32_t kDestinationKeep = 7;
constexpr std::uint32_t kDestinationZero = 4;
constexpr std::uint32_t kDestinationOne = 5;
constexpr std::uint32_t kLastVertexFetchConstant = 95;
constexpr std::uint32_t kMaxStreams = 16;
constexpr int kMaxSteps = 65536;
constexpr std::uint32_t kVertexSamplerBase = 16;

/// Helper functions every generated shader carries; unused ones cost
/// nothing after compilation.
constexpr std::string_view kHelpers = R"(
const float XE_FLT_MAX = 3.402823466e+38;

vec4 xe_mul(vec4 a, vec4 b)
{
    // Direct3D 9 multiplication: zero (or a denormal) times anything is +0.
    return mix(a * b, vec4(0.0), lessThan(min(abs(a), abs(b)), vec4(1.17549435e-38)));
}

float xe_mul1(float a, float b)
{
    return min(abs(a), abs(b)) < 1.17549435e-38 ? 0.0 : a * b;
}

float xe_dot4(vec4 a, vec4 b)
{
    vec4 p = xe_mul(a, b);
    return p.x + p.y + p.z + p.w;
}

float xe_dot3(vec4 a, vec4 b)
{
    vec4 p = xe_mul(a, b);
    return p.x + p.y + p.z;
}

vec4 xe_max(vec4 a, vec4 b)
{
    return mix(b, a, greaterThanEqual(a, b));
}

vec4 xe_min(vec4 a, vec4 b)
{
    return mix(b, a, lessThan(a, b));
}

float xe_max1(float a, float b)
{
    return a >= b ? a : b;
}

float xe_min1(float a, float b)
{
    return a < b ? a : b;
}

float xe_max4(vec4 a)
{
    return xe_max1(xe_max1(a.x, a.y), xe_max1(a.z, a.w));
}

float xe_clampmax(float t)
{
    return isinf(t) ? (t < 0.0 ? -XE_FLT_MAX : XE_FLT_MAX) : t;
}

float xe_clampzero(float t)
{
    return isinf(t) ? (t < 0.0 ? -0.0 : 0.0) : t;
}

float xe_rcp(float a)
{
    return a == 1.0 ? 1.0 : 1.0 / a;
}

float xe_rsq(float a)
{
    return a == 1.0 ? 1.0 : inversesqrt(a);
}

float xe_log(float a)
{
    return a == 1.0 ? 0.0 : log2(a);
}

float xe_exp(float a)
{
    return a == 0.0 ? 1.0 : exp2(a);
}

vec4 xe_cube(vec4 s)
{
    // The operand is the direction in .z_xy order.
    vec3 d = vec3(s.z, s.w, s.x);
    vec3 m = abs(d);
    float tc, sc, ma, face;
    if (m.z >= m.x && m.z >= m.y) {
        tc = -d.y;
        sc = d.z < 0.0 ? -d.x : d.x;
        ma = d.z;
        face = d.z < 0.0 ? 5.0 : 4.0;
    } else if (m.y >= m.x) {
        tc = d.y < 0.0 ? -d.z : d.z;
        sc = d.x;
        ma = d.y;
        face = d.y < 0.0 ? 3.0 : 2.0;
    } else {
        tc = -d.y;
        sc = d.x < 0.0 ? d.z : -d.z;
        ma = d.x;
        face = d.x < 0.0 ? 1.0 : 0.0;
    }
    return vec4(tc, sc, 2.0 * ma, face);
}

vec3 xe_cube_direction(vec3 c)
{
    // Inverse of xe_cube followed by the usual `mad r.xy, r.xy, 1/|2ma|, 1.5`:
    // c = (s in [1, 2], t in [1, 2], face id).
    float s = (c.x - 1.5) * 2.0;
    float t = (c.y - 1.5) * 2.0;
    int face = int(clamp(floor(c.z + 0.5), 0.0, 5.0));
    if (face == 0) {
        return vec3(1.0, -t, -s);
    }
    if (face == 1) {
        return vec3(-1.0, -t, s);
    }
    if (face == 2) {
        return vec3(s, 1.0, t);
    }
    if (face == 3) {
        return vec3(s, -1.0, -t);
    }
    if (face == 4) {
        return vec3(s, -t, 1.0);
    }
    return vec3(-s, -t, -1.0);
}

bool xe_bool(uint i)
{
    return ((xe_bools[i >> 7u][(i >> 5u) & 3u] >> (i & 31u)) & 1u) != 0u;
}

uint xe_loop(uint i)
{
    return xe_loops[i >> 2u][i & 3u];
}

uvec4 xe_swap(uvec4 v, uint endian)
{
    if (endian == 1u || endian == 2u) {
        v = ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
    }
    if (endian == 2u || endian == 3u) {
        v = (v << 16u) | (v >> 16u);
    }
    return v;
}

vec4 xe_fixed(uvec4 u, uvec4 bits, bool sgn, bool integer, bool noZero)
{
    if (sgn) {
        ivec4 s = ivec4(u << (32u - bits)) >> ivec4(32u - bits);
        vec4 f = vec4(s);
        if (integer) {
            return f;
        }
        vec4 m = vec4((uvec4(1u) << (bits - 1u)) - 1u);
        return noZero ? (f * 2.0 + 1.0) / (m * 2.0 + 1.0) : max(f / m, vec4(-1.0));
    }
    vec4 f = vec4(u);
    return integer ? f : f / vec4((uvec4(1u) << bits) - 1u);
}

vec4 xe_fixed32(uvec4 u, bool sgn, bool integer)
{
    if (sgn) {
        vec4 f = vec4(ivec4(u));
        return integer ? f : max(f / 2147483647.0, vec4(-1.0));
    }
    vec4 f = vec4(u);
    return integer ? f : f / 4294967295.0;
}

vec3 xe_gamma_encode(vec3 c)
{
    // Xenos piecewise-linear gamma: slopes 4, 2, 1 and 1/2.
    c = clamp(c, 0.0, 1.0);
    vec3 r = c * 4.0;
    r = mix(r, c * 2.0 + 0.125, greaterThanEqual(c, vec3(0.0625)));
    r = mix(r, c + 0.25, greaterThanEqual(c, vec3(0.125)));
    r = mix(r, c * 0.5 + 0.5, greaterThanEqual(c, vec3(0.5)));
    return r;
}
)";

std::string glslFloat(float value)
{
    if (!std::isfinite(value)) {
        return std::format("uintBitsToFloat(0x{:08X}u)", std::bit_cast<std::uint32_t>(value));
    }
    std::string text = std::format("{}", value);
    if (text.find_first_of(".en") == std::string::npos) {
        text += ".0";
    }
    return text;
}

std::string maskSuffix(std::uint32_t mask)
{
    std::string text = ".";
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (((mask >> i) & 1) != 0) {
            text.push_back(kComponents[i]);
        }
    }
    return text;
}

std::string splat(const std::string& scalar, std::uint32_t mask)
{
    const int count = std::popcount(mask);
    return count == 1 ? scalar : std::format("vec{}({})", count, scalar);
}

/// Collected facts about a program, gathered before emitting code.
struct Analysis {
    std::uint32_t maxRegister = 0;
    bool relativeTemporaries = false;
    bool needsDispatcher = false;
    std::set<std::uint32_t> exportedInterpolators;
    bool writesPointSize = false;
};

class Translator {
public:
    Translator(const Program& program, const TranslationOptions& options)
        : m_program(program)
        , m_options(options)
        , m_vertex(program.stage == ShaderStage::Vertex)
    {
    }

    TranslationResult run();

private:
    void analyse();
    void noteRegister(std::uint32_t index, bool relative);
    void unsupported(std::string what);

    void line(const std::string& text);
    void open(const std::string& text);
    void close(const std::string& text = "}");

    std::string temporary(std::uint32_t index, bool relative) const;
    std::string constant(const AluOperand& op) const;
    std::string operand(const AluOperand& op) const;
    std::string condition(const CfInstruction& cf) const;
    std::string exportTarget(std::uint32_t reg);

    void emitBody();
    void emitStraight();
    void emitDispatcher();
    void emitExec(const CfInstruction& cf);
    void emitSlot(std::uint32_t slot, bool fetch);
    void emitAlu(const AluInstruction& alu);
    void emitVectorOp(const AluInstruction& alu);
    void emitScalarOp(const AluInstruction& alu);
    void emitVertexFetch(const FetchInstruction& fetch, std::uint32_t slot);
    void emitTextureFetch(const FetchInstruction& fetch);
    void writeDestination(const FetchInstruction& fetch, const std::string& value);
    std::string sampler(std::uint32_t slot, TextureDimension dimension);

    std::string header() const;
    std::string mainFunction() const;

    const Program& m_program;
    const TranslationOptions& m_options;
    bool m_vertex;
    Analysis m_analysis;
    std::uint32_t m_registerCount = 1;
    TranslationResult m_result;
    std::string m_body;
    int m_indent = 0;
    std::set<std::string> m_unsupported;
    std::set<std::uint32_t> m_streams;
    std::uint32_t m_lastFullFetchStream = 0;
    std::uint32_t m_lastFullFetchStride = 0;
};

void Translator::unsupported(std::string what)
{
    if (m_unsupported.insert(what).second) {
        m_result.unsupported.push_back(std::move(what));
    }
}

void Translator::noteRegister(std::uint32_t index, bool relative)
{
    m_analysis.maxRegister = std::max(m_analysis.maxRegister, index);
    m_analysis.relativeTemporaries = m_analysis.relativeTemporaries || relative;
}

void Translator::analyse()
{
    for (const CfInstruction& cf : m_program.cf) {
        switch (cf.opcode) {
        case CfOpcode::LoopStart:
        case CfOpcode::LoopEnd:
        case CfOpcode::CondCall:
        case CfOpcode::Return:
        case CfOpcode::CondJmp:
            m_analysis.needsDispatcher = true;
            break;
        default:
            break;
        }
        if (!cf.isExec()) {
            continue;
        }
        for (std::uint32_t k = 0; k < cf.count; ++k) {
            const auto w = m_program.slot(cf.address + k);
            if (cf.slotIsFetch(k)) {
                const FetchInstruction f = decodeFetch(w[0], w[1], w[2]);
                noteRegister(f.srcRegister, f.srcRelative);
                noteRegister(f.dstRegister, f.dstRelative);
                continue;
            }
            const AluInstruction a = decodeAlu(w[0], w[1], w[2]);
            const std::uint32_t operands = vectorOperandCount(a.vectorOpcode);
            const ScalarOperands scalarForm = scalarOperands(a.scalarOpcode);
            for (std::uint32_t i = 0; i < 3; ++i) {
                const bool used = (i < operands && a.vectorDoesSomething())
                    || (i == 2 && (scalarForm == ScalarOperands::A || scalarForm == ScalarOperands::AB));
                if (used) {
                    const AluOperand op = a.vectorOperand(i);
                    if (op.temporary) {
                        noteRegister(op.index, op.relative);
                    }
                }
            }
            if (scalarForm == ScalarOperands::ConstantAndTemporary) {
                noteRegister(a.scalarTemporaryOperand().index, false);
            }
            if (a.exportData) {
                if (m_vertex && a.vectorDest < exportreg::kInterpolatorCount) {
                    m_analysis.exportedInterpolators.insert(a.vectorDest);
                }
                if (m_vertex && a.vectorDest == exportreg::kPointSizeEdgeKill) {
                    m_analysis.writesPointSize = true;
                }
                if (!m_vertex && a.vectorDest < exportreg::kColorCount) {
                    m_result.colorOutputs |= 1U << a.vectorDest;
                }
                if (!m_vertex && a.vectorDest == exportreg::kDepth) {
                    m_result.writesDepth = true;
                }
            } else {
                if (a.vectorResultMask() != 0) {
                    noteRegister(a.vectorDest, a.vectorDestRelative);
                }
                if (a.scalarResultMask() != 0) {
                    noteRegister(a.scalarDest, a.scalarDestRelative);
                }
            }
        }
    }
    if (!m_vertex) {
        for (std::uint32_t r = 0; r < m_options.inputLocations.size(); ++r) {
            if (m_options.inputLocations[r] >= 0) {
                noteRegister(r, false);
            }
        }
        if (m_options.parameterRegister) {
            noteRegister(*m_options.parameterRegister, false);
        }
    }
    m_analysis.maxRegister = std::min(m_analysis.maxRegister, kMaxTemporaries - 1);
    m_registerCount = m_analysis.relativeTemporaries ? kMaxTemporaries : m_analysis.maxRegister + 1;
}

void Translator::line(const std::string& text)
{
    m_body.append(static_cast<std::size_t>(m_indent) * 4, ' ');
    m_body += text;
    m_body.push_back('\n');
}

void Translator::open(const std::string& text)
{
    line(text);
    ++m_indent;
}

void Translator::close(const std::string& text)
{
    --m_indent;
    line(text);
}

std::string Translator::temporary(std::uint32_t index, bool relative) const
{
    if (relative) {
        return std::format("r[clamp(xe_aL + {}, 0, {})]", index, m_registerCount - 1);
    }
    return std::format("r[{}]", std::min(index, m_registerCount - 1));
}

std::string Translator::constant(const AluOperand& op) const
{
    if (op.relative) {
        return std::format("xe_consts.c[clamp({} + {}, 0, 255)]", op.constantUsesA0 ? "xe_a0" : "xe_aL", op.index);
    }
    const auto literal = m_options.literals.find(op.index);
    if (literal != m_options.literals.end()) {
        const auto& v = literal->second;
        return std::format("vec4({}, {}, {}, {})", glslFloat(v[0]), glslFloat(v[1]), glslFloat(v[2]), glslFloat(v[3]));
    }
    return std::format("xe_consts.c[{}]", op.index);
}

std::string Translator::operand(const AluOperand& op) const
{
    std::string text = op.temporary ? temporary(op.index, op.relative) : constant(op);
    if (op.absolute) {
        text = "abs(" + text + ")";
    }
    std::string swizzle = ".";
    for (std::uint32_t i = 0; i < 4; ++i) {
        swizzle.push_back(kComponents[op.components[i]]);
    }
    if (swizzle != ".xyzw") {
        text += swizzle;
    }
    if (op.negate) {
        text = "-(" + text + ")";
    }
    return text;
}

std::string Translator::condition(const CfInstruction& cf) const
{
    const bool usesPredicate = cf.testsPredicate()
        || ((cf.opcode == CfOpcode::CondCall || cf.opcode == CfOpcode::CondJmp) && cf.predicated);
    if ((cf.opcode == CfOpcode::CondCall || cf.opcode == CfOpcode::CondJmp) && cf.unconditional) {
        return "true";
    }
    if (usesPredicate) {
        return cf.condition ? "xe_p0" : "!xe_p0";
    }
    return std::format("{}xe_bool({}u)", cf.condition ? "" : "!", cf.boolAddress);
}

std::string Translator::exportTarget(std::uint32_t reg)
{
    if (m_vertex) {
        if (reg < exportreg::kInterpolatorCount) {
            return std::format("xe_o[{}]", reg);
        }
        if (reg == exportreg::kPosition) {
            return "xe_oPos";
        }
        if (reg == exportreg::kPointSizeEdgeKill) {
            return "xe_oPts";
        }
    } else {
        if (reg < exportreg::kColorCount) {
            return std::format("xe_oC[{}]", reg);
        }
        if (reg == exportreg::kDepth) {
            return "xe_oDepth";
        }
    }
    if (reg >= exportreg::kMemoryAddress && reg <= exportreg::kMemoryData4) {
        unsupported("memory export");
    } else {
        unsupported(std::format("export register {}", reg));
    }
    return {};
}

void Translator::emitVectorOp(const AluInstruction& alu)
{
    const auto op = static_cast<VectorOpcode>(alu.vectorOpcode);
    const std::string a = "xe_v0";
    const std::string b = "xe_v1";
    const std::string c = "xe_v2";
    auto set = [&](const std::string& value) { line("xe_v = " + value + ";"); };
    auto pushCompare = [&](const char* comparison) {
        line(std::format("xe_p0 = {0}.w == 0.0 && {1}.w {2} 0.0;", a, b, comparison));
        line(std::format("xe_v = vec4(({0}.x == 0.0 && {1}.x {2} 0.0) ? 0.0 : {0}.x + 1.0);", a, b, comparison));
    };
    auto kill = [&](const char* comparison) {
        if (m_vertex) {
            unsupported("kill in a vertex shader");
            set("vec4(0.0)");
            return;
        }
        m_result.usesKill = true;
        open(std::format("if (any({}({}, {}))) {{", comparison, a, b));
        line("discard;");
        close();
        set("vec4(0.0)");
    };
    switch (op) {
    case VectorOpcode::Add:
        set(a + " + " + b);
        break;
    case VectorOpcode::Mul:
        set(std::format("xe_mul({}, {})", a, b));
        break;
    case VectorOpcode::Max:
        set(std::format("xe_max({}, {})", a, b));
        break;
    case VectorOpcode::Min:
        set(std::format("xe_min({}, {})", a, b));
        break;
    case VectorOpcode::SetEq:
        set(std::format("vec4(equal({}, {}))", a, b));
        break;
    case VectorOpcode::SetGt:
        set(std::format("vec4(greaterThan({}, {}))", a, b));
        break;
    case VectorOpcode::SetGe:
        set(std::format("vec4(greaterThanEqual({}, {}))", a, b));
        break;
    case VectorOpcode::SetNe:
        set(std::format("vec4(notEqual({}, {}))", a, b));
        break;
    case VectorOpcode::Frac:
        set("fract(" + a + ")");
        break;
    case VectorOpcode::Trunc:
        set("trunc(" + a + ")");
        break;
    case VectorOpcode::Floor:
        set("floor(" + a + ")");
        break;
    case VectorOpcode::MulAdd:
        set(std::format("xe_mul({}, {}) + {}", a, b, c));
        break;
    case VectorOpcode::CndEq:
        set(std::format("mix({2}, {1}, equal({0}, vec4(0.0)))", a, b, c));
        break;
    case VectorOpcode::CndGe:
        set(std::format("mix({2}, {1}, greaterThanEqual({0}, vec4(0.0)))", a, b, c));
        break;
    case VectorOpcode::CndGt:
        set(std::format("mix({2}, {1}, greaterThan({0}, vec4(0.0)))", a, b, c));
        break;
    case VectorOpcode::Dot4:
        set(std::format("vec4(xe_dot4({}, {}))", a, b));
        break;
    case VectorOpcode::Dot3:
        set(std::format("vec4(xe_dot3({}, {}))", a, b));
        break;
    case VectorOpcode::Dot2Add:
        set(std::format("vec4(xe_mul1({0}.x, {1}.x) + xe_mul1({0}.y, {1}.y) + {2}.x)", a, b, c));
        break;
    case VectorOpcode::Cube:
        set("xe_cube(" + a + ")");
        break;
    case VectorOpcode::Max4:
        set("vec4(xe_max4(" + a + "))");
        break;
    case VectorOpcode::SetpEqPush:
        pushCompare("==");
        break;
    case VectorOpcode::SetpNePush:
        pushCompare("!=");
        break;
    case VectorOpcode::SetpGtPush:
        pushCompare(">");
        break;
    case VectorOpcode::SetpGePush:
        pushCompare(">=");
        break;
    case VectorOpcode::KillEq:
        kill("equal");
        break;
    case VectorOpcode::KillGt:
        kill("greaterThan");
        break;
    case VectorOpcode::KillGe:
        kill("greaterThanEqual");
        break;
    case VectorOpcode::KillNe:
        kill("notEqual");
        break;
    case VectorOpcode::Dst:
        set(std::format("vec4(1.0, xe_mul1({0}.y, {1}.y), {0}.z, {1}.w)", a, b));
        break;
    case VectorOpcode::MaxA:
        line(std::format("xe_a0 = int(clamp(floor({}.w + 0.5), -256.0, 255.0));", a));
        set(std::format("xe_max({}, {})", a, b));
        break;
    default:
        unsupported(std::format("vector opcode {}", alu.vectorOpcode));
        set("vec4(0.0)");
        break;
    }
}

void Translator::emitScalarOp(const AluInstruction& alu)
{
    const auto op = static_cast<ScalarOpcode>(alu.scalarOpcode);
    const bool constantForm = scalarOperands(alu.scalarOpcode) == ScalarOperands::ConstantAndTemporary;
    const std::string a = constantForm ? "xe_sc.w" : "xe_sa.w";
    const std::string b = constantForm ? "xe_st.x" : "xe_sa.x";
    auto set = [&](const std::string& value) { line("xe_s = " + value + ";"); };
    auto setp = [&](const std::string& test) {
        line(std::format("xe_p0 = {};", test));
        set("xe_p0 ? 0.0 : 1.0");
    };
    auto kill = [&](const std::string& test) {
        if (m_vertex) {
            unsupported("kill in a vertex shader");
            set("0.0");
            return;
        }
        m_result.usesKill = true;
        open(std::format("if ({}) {{", test));
        line("discard;");
        close();
        set("0.0");
    };
    switch (op) {
    case ScalarOpcode::Adds:
        set(a + " + " + b);
        break;
    case ScalarOpcode::AddsPrev:
        set(a + " + xe_ps");
        break;
    case ScalarOpcode::Muls:
        set(std::format("xe_mul1({}, {})", a, b));
        break;
    case ScalarOpcode::MulsPrev:
        set(std::format("xe_mul1({}, xe_ps)", a));
        break;
    case ScalarOpcode::MulsPrev2:
        set(std::format("(xe_ps == -XE_FLT_MAX || isinf(xe_ps) || isnan(xe_ps) || isinf({1}) || isnan({1}) "
                        "|| {1} <= 0.0) ? -XE_FLT_MAX : xe_mul1({0}, xe_ps)",
            a, b));
        break;
    case ScalarOpcode::Maxs:
        set(std::format("xe_max1({}, {})", a, b));
        break;
    case ScalarOpcode::Mins:
        set(std::format("xe_min1({}, {})", a, b));
        break;
    case ScalarOpcode::Seqs:
        set(a + " == 0.0 ? 1.0 : 0.0");
        break;
    case ScalarOpcode::Sgts:
        set(a + " > 0.0 ? 1.0 : 0.0");
        break;
    case ScalarOpcode::Sges:
        set(a + " >= 0.0 ? 1.0 : 0.0");
        break;
    case ScalarOpcode::Snes:
        set(a + " != 0.0 ? 1.0 : 0.0");
        break;
    case ScalarOpcode::Frcs:
        set("fract(" + a + ")");
        break;
    case ScalarOpcode::Truncs:
        set("trunc(" + a + ")");
        break;
    case ScalarOpcode::Floors:
        set("floor(" + a + ")");
        break;
    case ScalarOpcode::Exp:
        set("xe_exp(" + a + ")");
        break;
    case ScalarOpcode::Logc:
        set("xe_clampmax(xe_log(" + a + "))");
        break;
    case ScalarOpcode::Log:
        set("xe_log(" + a + ")");
        break;
    case ScalarOpcode::Rcpc:
        set("xe_clampmax(xe_rcp(" + a + "))");
        break;
    case ScalarOpcode::Rcpf:
        set("xe_clampzero(xe_rcp(" + a + "))");
        break;
    case ScalarOpcode::Rcp:
        set("xe_rcp(" + a + ")");
        break;
    case ScalarOpcode::Rsqc:
        set("xe_clampmax(xe_rsq(" + a + "))");
        break;
    case ScalarOpcode::Rsqf:
        set("xe_clampzero(xe_rsq(" + a + "))");
        break;
    case ScalarOpcode::Rsq:
        set("xe_rsq(" + a + ")");
        break;
    case ScalarOpcode::MaxAs:
        line(std::format("xe_a0 = int(clamp(floor({} + 0.5), -256.0, 255.0));", a));
        set(std::format("xe_max1({}, {})", a, b));
        break;
    case ScalarOpcode::MaxAsf:
        line(std::format("xe_a0 = int(clamp(floor({}), -256.0, 255.0));", a));
        set(std::format("xe_max1({}, {})", a, b));
        break;
    case ScalarOpcode::Subs:
        set(a + " - " + b);
        break;
    case ScalarOpcode::SubsPrev:
        set(a + " - xe_ps");
        break;
    case ScalarOpcode::SetpEq:
        setp(a + " == 0.0");
        break;
    case ScalarOpcode::SetpNe:
        setp(a + " != 0.0");
        break;
    case ScalarOpcode::SetpGt:
        setp(a + " > 0.0");
        break;
    case ScalarOpcode::SetpGe:
        setp(a + " >= 0.0");
        break;
    case ScalarOpcode::SetpInv:
        line(std::format("xe_p0 = {} == 1.0;", a));
        set(std::format("xe_p0 ? 0.0 : ({0} == 0.0 ? 1.0 : {0})", a));
        break;
    case ScalarOpcode::SetpPop:
        line(std::format("xe_p0 = {} - 1.0 <= 0.0;", a));
        set(std::format("xe_p0 ? 0.0 : {} - 1.0", a));
        break;
    case ScalarOpcode::SetpClr:
        line("xe_p0 = false;");
        set("XE_FLT_MAX");
        break;
    case ScalarOpcode::SetpRstr:
        line(std::format("xe_p0 = {} == 0.0;", a));
        set(std::format("xe_p0 ? 0.0 : {}", a));
        break;
    case ScalarOpcode::KillsEq:
        kill(a + " == 0.0");
        break;
    case ScalarOpcode::KillsGt:
        kill(a + " > 0.0");
        break;
    case ScalarOpcode::KillsGe:
        kill(a + " >= 0.0");
        break;
    case ScalarOpcode::KillsNe:
        kill(a + " != 0.0");
        break;
    case ScalarOpcode::KillsOne:
        kill(a + " == 1.0");
        break;
    case ScalarOpcode::Sqrt:
        set("sqrt(" + a + ")");
        break;
    case ScalarOpcode::Mulsc0:
    case ScalarOpcode::Mulsc1:
        set(std::format("xe_mul1({}, {})", a, b));
        break;
    case ScalarOpcode::Addsc0:
    case ScalarOpcode::Addsc1:
        set(a + " + " + b);
        break;
    case ScalarOpcode::Subsc0:
    case ScalarOpcode::Subsc1:
        set(a + " - " + b);
        break;
    case ScalarOpcode::Sin:
        set("sin(" + a + ")");
        break;
    case ScalarOpcode::Cos:
        set("cos(" + a + ")");
        break;
    case ScalarOpcode::RetainPrev:
        set("xe_ps");
        break;
    default:
        unsupported(std::format("scalar opcode {}", alu.scalarOpcode));
        set("0.0");
        break;
    }
}

void Translator::emitAlu(const AluInstruction& alu)
{
    const bool vectorActive = alu.vectorDoesSomething();
    const bool scalarActive = alu.scalarDoesSomething();
    if (!vectorActive && !scalarActive) {
        return;
    }
    if (alu.predicated) {
        open(std::format("if ({}xe_p0) {{", alu.predicateCondition ? "" : "!"));
    } else {
        open("{");
    }
    if (vectorActive) {
        const std::uint32_t operands = vectorOperandCount(alu.vectorOpcode);
        for (std::uint32_t i = 0; i < operands; ++i) {
            line(std::format("vec4 xe_v{} = {};", i, operand(alu.vectorOperand(i))));
        }
    }
    const ScalarOperands scalarForm = scalarOperands(alu.scalarOpcode);
    if (scalarActive) {
        if (scalarForm == ScalarOperands::A || scalarForm == ScalarOperands::AB) {
            line(std::format("vec4 xe_sa = {};", operand(alu.scalarOperand())));
        } else if (scalarForm == ScalarOperands::ConstantAndTemporary) {
            line(std::format("vec4 xe_sc = {};", operand(alu.scalarOperand())));
            line(std::format("vec4 xe_st = {};", operand(alu.scalarTemporaryOperand())));
        }
    }
    if (vectorActive) {
        line("vec4 xe_v;");
        emitVectorOp(alu);
        if (alu.vectorClamp) {
            line("xe_v = clamp(xe_v, 0.0, 1.0);");
        }
    }
    if (scalarActive) {
        line("float xe_s;");
        emitScalarOp(alu);
        if (alu.scalarClamp) {
            line("xe_s = clamp(xe_s, 0.0, 1.0);");
        }
        if (static_cast<ScalarOpcode>(alu.scalarOpcode) != ScalarOpcode::RetainPrev) {
            line("xe_ps = xe_s;");
        }
    }
    if (alu.exportData) {
        const std::string target = exportTarget(alu.vectorDest);
        if (!target.empty()) {
            if (vectorActive && alu.vectorResultMask() != 0) {
                const std::string mask = maskSuffix(alu.vectorResultMask());
                line(target + mask + " = xe_v" + mask + ";");
            }
            if (scalarActive && alu.scalarResultMask() != 0) {
                line(target + maskSuffix(alu.scalarResultMask()) + " = " + splat("xe_s", alu.scalarResultMask()) + ";");
            }
            if (alu.exportOneMask() != 0) {
                line(target + maskSuffix(alu.exportOneMask()) + " = " + splat("1.0", alu.exportOneMask()) + ";");
            }
            if (alu.exportZeroMask() != 0) {
                line(target + maskSuffix(alu.exportZeroMask()) + " = " + splat("0.0", alu.exportZeroMask()) + ";");
            }
        }
    } else {
        if (vectorActive && alu.vectorResultMask() != 0) {
            const std::string mask = maskSuffix(alu.vectorResultMask());
            line(temporary(alu.vectorDest, alu.vectorDestRelative) + mask + " = xe_v" + mask + ";");
        }
        if (scalarActive && alu.scalarResultMask() != 0) {
            line(temporary(alu.scalarDest, alu.scalarDestRelative) + maskSuffix(alu.scalarResultMask()) + " = "
                + splat("xe_s", alu.scalarResultMask()) + ";");
        }
    }
    close();
}

void Translator::writeDestination(const FetchInstruction& fetch, const std::string& value)
{
    const std::string dst = temporary(fetch.dstRegister, fetch.dstRelative);
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t selector = fetch.dstSelector(i);
        if (selector == kDestinationKeep) {
            continue;
        }
        std::string source;
        if (selector < 4) {
            source = std::format("{}.{}", value, kComponents[selector]);
        } else if (selector == kDestinationZero) {
            source = "0.0";
        } else if (selector == kDestinationOne) {
            source = "1.0";
        } else {
            unsupported(std::format("fetch destination selector {}", selector));
            source = "0.0";
        }
        line(std::format("{}.{} = {};", dst, kComponents[i], source));
    }
}

void Translator::emitVertexFetch(const FetchInstruction& fetch, std::uint32_t slot)
{
    VertexFetchLayout layout;
    const auto found = m_options.vertexFetches.find(slot);
    if (found != m_options.vertexFetches.end()) {
        layout = found->second;
    } else {
        layout.stream = kLastVertexFetchConstant - std::min(fetch.constIndex, kLastVertexFetchConstant);
        layout.byteOffset = static_cast<std::uint32_t>(std::max(fetch.offset, 0)) * 4;
        layout.byteStride = fetch.stride * 4;
        layout.format = static_cast<VertexFormat>(fetch.format);
        layout.isSigned = fetch.isSigned;
        layout.integer = !fetch.normalized;
        layout.signedNoZero = fetch.signedNoZero;
        layout.expAdjust = fetch.expAdjust;
        if (fetch.mini) {
            layout.stream = m_lastFullFetchStream;
            layout.byteStride = m_lastFullFetchStride;
        }
    }
    if (!fetch.mini) {
        m_lastFullFetchStream = layout.stream;
        m_lastFullFetchStride = layout.byteStride;
    }
    if (!isKnownVertexFormat(static_cast<std::uint32_t>(layout.format))) {
        unsupported(std::format("vertex fetch without a known format (slot {}, format {})", slot,
            static_cast<std::uint32_t>(layout.format)));
        writeDestination(fetch, "vec4(0.0, 0.0, 0.0, 1.0)");
        return;
    }
    if (layout.stream >= kMaxStreams) {
        unsupported(std::format("vertex stream {}", layout.stream));
        layout.stream %= kMaxStreams;
    }
    if (layout.byteOffset % 4 != 0 || layout.byteStride % 4 != 0) {
        unsupported("vertex element not aligned to 4 bytes");
    }
    m_streams.insert(layout.stream);

    if (fetch.predicated) {
        open(std::format("if ({}xe_p0) {{", fetch.predicateCondition ? "" : "!"));
    } else {
        open("{");
    }
    if (!fetch.mini) {
        const std::string index = std::format("{}.{}", temporary(fetch.srcRegister, fetch.srcRelative),
            kComponents[fetch.srcSwizzle & 3]);
        line(std::format("xe_vindex = uint(max({}, 0.0));",
            fetch.indexRounded ? "floor(" + index + " + 0.5)" : "floor(" + index + ")"));
    }
    const std::uint32_t words = (vertexFormatBytes(layout.format) + 3) / 4;
    line(std::format("uint xe_at = xe_streamBase[{}u][{}u] + xe_vindex * {}u + {}u;", layout.stream / 4,
        layout.stream % 4, layout.byteStride / 4, layout.byteOffset / 4));
    std::string load = "uvec4(";
    for (std::uint32_t w = 0; w < 4; ++w) {
        load += w < words ? std::format("xe_vb{}[xe_at + {}u]", layout.stream, w) : std::string("0u");
        load += w < 3 ? ", " : ")";
    }
    line(std::format("uvec4 xe_w = xe_swap({}, {}u);", load, layout.endian));
    const std::string s = layout.isSigned ? "true" : "false";
    const std::string i = layout.integer ? "true" : "false";
    const std::string nz = layout.signedNoZero ? "true" : "false";
    auto fixed = [&](const std::string& values, const std::string& bits) {
        return std::format("xe_fixed({}, {}, {}, {}, {})", values, bits, s, i, nz);
    };
    std::string value;
    switch (layout.format) {
    case VertexFormat::F8_8_8_8:
        value = fixed("(uvec4(xe_w.x, xe_w.x >> 8u, xe_w.x >> 16u, xe_w.x >> 24u) & 0xFFu)", "uvec4(8u)");
        break;
    case VertexFormat::F2_10_10_10:
        value = fixed("(uvec4(xe_w.x, xe_w.x >> 10u, xe_w.x >> 20u, xe_w.x >> 30u) & uvec4(0x3FFu, 0x3FFu, 0x3FFu, "
                      "3u))",
            "uvec4(10u, 10u, 10u, 2u)");
        break;
    case VertexFormat::F10_11_11:
        value = "vec4(" + fixed("(uvec4(xe_w.x, xe_w.x >> 11u, xe_w.x >> 22u, 0u) & uvec4(0x7FFu, 0x7FFu, 0x3FFu, 0u))",
                              "uvec4(11u, 11u, 10u, 1u)")
            + ".xyz, 1.0)";
        break;
    case VertexFormat::F11_11_10:
        value = "vec4(" + fixed("(uvec4(xe_w.x, xe_w.x >> 10u, xe_w.x >> 21u, 0u) & uvec4(0x3FFu, 0x7FFu, 0x7FFu, 0u))",
                              "uvec4(10u, 11u, 11u, 1u)")
            + ".xyz, 1.0)";
        break;
    case VertexFormat::F16_16:
        value = "vec4(" + fixed("(uvec4(xe_w.x, xe_w.x >> 16u, 0u, 0u) & 0xFFFFu)", "uvec4(16u)") + ".xy, 0.0, 1.0)";
        break;
    case VertexFormat::F16_16_16_16:
        value = fixed("(uvec4(xe_w.x, xe_w.x >> 16u, xe_w.y, xe_w.y >> 16u) & 0xFFFFu)", "uvec4(16u)");
        break;
    case VertexFormat::F16_16_Float:
        value = "vec4(unpackHalf2x16(xe_w.x), 0.0, 1.0)";
        break;
    case VertexFormat::F16_16_16_16_Float:
        value = "vec4(unpackHalf2x16(xe_w.x), unpackHalf2x16(xe_w.y))";
        break;
    case VertexFormat::F32:
        value = std::format("vec4(xe_fixed32(xe_w, {}, {}).x, 0.0, 0.0, 1.0)", s, i);
        break;
    case VertexFormat::F32_32:
        value = std::format("vec4(xe_fixed32(xe_w, {}, {}).xy, 0.0, 1.0)", s, i);
        break;
    case VertexFormat::F32_32_32_32:
        value = std::format("xe_fixed32(xe_w, {}, {})", s, i);
        break;
    case VertexFormat::F32_Float:
        value = "vec4(uintBitsToFloat(xe_w.x), 0.0, 0.0, 1.0)";
        break;
    case VertexFormat::F32_32_Float:
        value = "vec4(uintBitsToFloat(xe_w.xy), 0.0, 1.0)";
        break;
    case VertexFormat::F32_32_32_32_Float:
        value = "uintBitsToFloat(xe_w)";
        break;
    case VertexFormat::F32_32_32_Float:
        value = "vec4(uintBitsToFloat(xe_w.xyz), 1.0)";
        break;
    case VertexFormat::Undefined:
        value = "vec4(0.0, 0.0, 0.0, 1.0)";
        break;
    }
    line("vec4 xe_raw = " + value + ";");
    const bool floatFormat = layout.format == VertexFormat::F16_16_Float
        || layout.format == VertexFormat::F16_16_16_16_Float || layout.format == VertexFormat::F32_Float
        || layout.format == VertexFormat::F32_32_Float || layout.format == VertexFormat::F32_32_32_32_Float
        || layout.format == VertexFormat::F32_32_32_Float;
    if (layout.expAdjust != 0 && !floatFormat) {
        line(std::format("xe_raw *= {};", glslFloat(std::ldexp(1.0F, layout.expAdjust))));
    }
    std::string selected = "vec4(";
    for (std::uint32_t c = 0; c < 4; ++c) {
        const std::uint32_t selector = layout.selectors[c];
        if (selector < 4) {
            selected += std::format("xe_raw.{}", kComponents[selector]);
        } else {
            selected += selector == kDestinationOne ? "1.0" : "0.0";
        }
        selected += c < 3 ? ", " : ")";
    }
    line("vec4 xe_f = " + selected + ";");
    writeDestination(fetch, "xe_f");
    close();
}

std::string Translator::sampler(std::uint32_t slot, TextureDimension dimension)
{
    const TextureDimension kind = dimension == TextureDimension::D1 ? TextureDimension::D2 : dimension;
    std::string name = std::format("xe_tex{}", slot);
    if (kind == TextureDimension::D3) {
        name += "_3d";
    } else if (kind == TextureDimension::Cube) {
        name += "_cube";
    }
    const bool known = std::any_of(m_result.samplers.begin(), m_result.samplers.end(),
        [&](const SamplerUse& use) { return use.glslName == name; });
    if (!known) {
        m_result.samplers.push_back({slot, kind, name});
    }
    return name;
}

void Translator::emitTextureFetch(const FetchInstruction& fetch)
{
    const auto op = static_cast<FetchOpcode>(fetch.opcode);
    const std::string source = temporary(fetch.srcRegister, fetch.srcRelative);
    std::string coords = source + ".";
    for (std::uint32_t i = 0; i < 3; ++i) {
        coords.push_back(kComponents[fetch.srcComponent(i)]);
    }
    if (fetch.predicated) {
        open(std::format("if ({}xe_p0) {{", fetch.predicateCondition ? "" : "!"));
    } else {
        open("{");
    }
    switch (op) {
    case FetchOpcode::SetLod:
        line(std::format("xe_lod = {}.{};", source, kComponents[fetch.srcComponent(0)]));
        close();
        return;
    case FetchOpcode::SetGradientsHorizontal:
        line("xe_gradH = " + coords + ";");
        close();
        return;
    case FetchOpcode::SetGradientsVertical:
        line("xe_gradV = " + coords + ";");
        close();
        return;
    case FetchOpcode::GetGradients:
        if (m_vertex) {
            writeDestination(fetch, "vec4(0.0)");
        } else {
            line(std::format("vec2 xe_g = {}.{}{};", source, kComponents[fetch.srcComponent(0)],
                kComponents[fetch.srcComponent(1)]));
            writeDestination(fetch, "vec4(dFdx(xe_g.x), dFdy(xe_g.x), dFdx(xe_g.y), dFdy(xe_g.y))");
        }
        close();
        return;
    case FetchOpcode::GetBorderColorFraction:
        writeDestination(fetch, "vec4(0.0)");
        close();
        return;
    case FetchOpcode::TextureFetch:
    case FetchOpcode::GetComputedLod:
    case FetchOpcode::GetWeights:
        break;
    default:
        unsupported(std::format("fetch opcode {}", fetch.opcode));
        writeDestination(fetch, "vec4(0.0)");
        close();
        return;
    }

    const std::string tex = sampler(fetch.constIndex, fetch.dimension);
    line("vec3 xe_c = " + coords + ";");
    std::string coord;
    std::string sizeType;
    switch (fetch.dimension) {
    case TextureDimension::D1:
        coord = "vec2(xe_c.x, 0.5)";
        sizeType = "vec2";
        break;
    case TextureDimension::D2:
        coord = "xe_c.xy";
        sizeType = "vec2";
        break;
    case TextureDimension::D3:
        coord = "xe_c";
        sizeType = "vec3";
        break;
    case TextureDimension::Cube:
        coord = "xe_cube_direction(xe_c)";
        break;
    }
    if (fetch.dimension != TextureDimension::Cube) {
        line(std::format("{0} xe_uv = {1};", sizeType, coord));
        if (fetch.unnormalizedCoordinates) {
            line(std::format("xe_uv /= {}(textureSize({}, 0));", sizeType, tex));
        }
        const bool offset = fetch.offsetX != 0.0F || fetch.offsetY != 0.0F
            || (fetch.dimension == TextureDimension::D3 && fetch.offsetZ != 0.0F);
        if (offset) {
            const std::string delta = sizeType == "vec2"
                ? std::format("vec2({}, {})", glslFloat(fetch.offsetX),
                      glslFloat(fetch.dimension == TextureDimension::D1 ? 0.0F : fetch.offsetY))
                : std::format("vec3({}, {}, {})", glslFloat(fetch.offsetX), glslFloat(fetch.offsetY),
                      glslFloat(fetch.offsetZ));
            line(std::format("xe_uv += {} / {}(textureSize({}, 0));", delta, sizeType, tex));
        }
        coord = "xe_uv";
    }

    if (op == FetchOpcode::GetComputedLod) {
        if (m_vertex) {
            writeDestination(fetch, "vec4(0.0)");
        } else {
            writeDestination(fetch, std::format("vec4(textureQueryLod({}, {}).y, 0.0, 0.0, 0.0)", tex, coord));
        }
        close();
        return;
    }
    if (op == FetchOpcode::GetWeights) {
        if (fetch.dimension == TextureDimension::Cube) {
            writeDestination(fetch, "vec4(0.0)");
        } else {
            line(std::format("{0} xe_texel = xe_uv * {0}(textureSize({1}, 0)) - 0.5;", sizeType, tex));
            writeDestination(fetch,
                sizeType == "vec2" ? "vec4(fract(xe_texel), 0.0, 0.0)" : "vec4(fract(xe_texel), 0.0)");
        }
        close();
        return;
    }

    const std::string bias = glslFloat(fetch.lodBias);
    std::string sample;
    if (m_vertex) {
        sample = std::format("textureLod({}, {}, {}{})", tex, coord, fetch.useRegisterLod ? "xe_lod + " : "", bias);
    } else if (fetch.useRegisterGradients) {
        const std::string swizzle = fetch.dimension == TextureDimension::D1 || fetch.dimension == TextureDimension::D2
            ? ".xy"
            : "";
        sample = std::format("textureGrad({0}, {1}, xe_gradH{2}, xe_gradV{2})", tex, coord, swizzle);
    } else if (!fetch.useComputedLod) {
        sample = std::format("textureLod({}, {}, {}{})", tex, coord, fetch.useRegisterLod ? "xe_lod + " : "", bias);
    } else if (fetch.useRegisterLod) {
        sample = std::format("texture({}, {}, xe_lod + {})", tex, coord, bias);
    } else if (fetch.lodBias != 0.0F) {
        sample = std::format("texture({}, {}, {})", tex, coord, bias);
    } else {
        sample = std::format("texture({}, {})", tex, coord);
    }
    line("vec4 xe_f = " + sample + ";");
    writeDestination(fetch, "xe_f");
    close();
}

void Translator::emitSlot(std::uint32_t slot, bool fetch)
{
    const auto w = m_program.slot(slot);
    if (fetch) {
        const FetchInstruction f = decodeFetch(w[0], w[1], w[2]);
        if (f.isVertexFetch()) {
            if (!m_vertex) {
                unsupported("vertex fetch in a pixel shader");
                return;
            }
            emitVertexFetch(f, slot);
        } else {
            emitTextureFetch(f);
        }
        return;
    }
    emitAlu(decodeAlu(w[0], w[1], w[2]));
}

void Translator::emitExec(const CfInstruction& cf)
{
    const bool conditional = cf.testsBool() || cf.testsPredicate();
    if (conditional) {
        open(std::format("if ({}) {{", condition(cf)));
    }
    for (std::uint32_t k = 0; k < cf.count; ++k) {
        emitSlot(cf.address + k, cf.slotIsFetch(k));
    }
    if (cf.endsShader()) {
        line("return;");
    }
    if (conditional) {
        close();
    }
}

void Translator::emitStraight()
{
    line("void xe_body()");
    open("{");
    for (std::size_t i = 0; i < m_program.cf.size(); ++i) {
        const CfInstruction& cf = m_program.cf[i];
        if (!cf.isExec()) {
            continue;
        }
        line(std::format("// {} {}", i, cfOpcodeName(cf.opcode)));
        emitExec(cf);
    }
    close();
}

void Translator::emitDispatcher()
{
    line("void xe_body()");
    open("{");
    line("int xe_pc = 0;");
    line("int xe_loopIteration[4] = int[4](0, 0, 0, 0);");
    line("int xe_loopSavedAL[4] = int[4](0, 0, 0, 0);");
    line("int xe_loopDepth = 0;");
    line("int xe_returnTo[4] = int[4](0, 0, 0, 0);");
    line("int xe_callDepth = 0;");
    open(std::format("for (int xe_step = 0; xe_step < {}; ++xe_step) {{", kMaxSteps));
    open("switch (xe_pc) {");
    for (std::size_t i = 0; i < m_program.cf.size(); ++i) {
        const CfInstruction& cf = m_program.cf[i];
        const std::uint32_t next = static_cast<std::uint32_t>(i + 1);
        line(std::format("case {}: // {}", i, cfOpcodeName(cf.opcode)));
        ++m_indent;
        switch (cf.opcode) {
        case CfOpcode::LoopStart:
            open("{");
            line(std::format("uint xe_constant = xe_loop({}u);", cf.loopId));
            open("if (xe_loopDepth < 4) {");
            line("xe_loopSavedAL[xe_loopDepth] = xe_aL;");
            line("xe_loopIteration[xe_loopDepth] = 0;");
            line("++xe_loopDepth;");
            close();
            if (!cf.repeat) {
                line("xe_aL = int((xe_constant >> 8u) & 0xFFu);");
            }
            open("if ((xe_constant & 0xFFu) == 0u) {");
            line("xe_loopDepth = max(xe_loopDepth - 1, 0);");
            line("xe_aL = xe_loopSavedAL[xe_loopDepth];");
            line(std::format("xe_pc = {};", cf.address));
            line("continue;");
            close();
            close();
            break;
        case CfOpcode::LoopEnd:
            open("{");
            line(std::format("uint xe_constant = xe_loop({}u);", cf.loopId));
            line("int xe_level = max(xe_loopDepth - 1, 0);");
            line("xe_loopIteration[xe_level] += 1;");
            line("xe_aL += bitfieldExtract(int(xe_constant), 16, 8);");
            line(std::format("bool xe_break = {};",
                cf.predicatedBreak ? std::string(cf.condition ? "xe_p0" : "!xe_p0") : std::string("false")));
            open("if (!xe_break && uint(xe_loopIteration[xe_level]) < (xe_constant & 0xFFu)) {");
            line(std::format("xe_pc = {};", cf.address));
            line("continue;");
            close();
            line("xe_aL = xe_loopSavedAL[xe_level];");
            line("xe_loopDepth = xe_level;");
            close();
            break;
        case CfOpcode::CondCall:
            open(std::format("if ({}) {{", condition(cf)));
            open("if (xe_callDepth < 4) {");
            line(std::format("xe_returnTo[xe_callDepth] = {};", next));
            line("++xe_callDepth;");
            close();
            line(std::format("xe_pc = {};", cf.address));
            line("continue;");
            close();
            break;
        case CfOpcode::Return:
            open("if (xe_callDepth > 0) {");
            line("--xe_callDepth;");
            line("xe_pc = xe_returnTo[xe_callDepth];");
            line("continue;");
            close();
            break;
        case CfOpcode::CondJmp:
            open(std::format("if ({}) {{", condition(cf)));
            line(std::format("xe_pc = {};", cf.address));
            line("continue;");
            close();
            break;
        default:
            if (cf.isExec()) {
                emitExec(cf);
            }
            break;
        }
        --m_indent;
    }
    line("default:");
    line("    return;");
    close();
    line("return;");
    close();
    close();
}

std::string Translator::header() const
{
    std::string h = "#version 450 core\n";
    h += std::format("// Translated from Xenos {} shader microcode ({} slots).\n", stageName(m_program.stage),
        m_program.slotCount());
    if (m_vertex) {
        h += "layout(std140, binding = 0) uniform XeVertexConstants\n{\n    vec4 c[256];\n} xe_consts;\n";
    } else {
        h += "layout(std140, binding = 1) uniform XePixelConstants\n{\n    vec4 c[256];\n} xe_consts;\n";
    }
    h += "layout(std140, binding = 2) uniform XeControlConstants\n{\n    uvec4 xe_bools[2];\n    uvec4 xe_loops[8];\n};\n";
    h += "layout(std140, binding = 3) uniform XeSystemConstants\n"
         "{\n"
         "    vec4 xe_clipScale;\n"
         "    vec4 xe_clipOffset;\n"
         "    vec4 xe_screen;\n"
         "    vec4 xe_alphaTest;\n"
         "    uvec4 xe_flags;\n"
         "    uvec4 xe_streamBase[4];\n"
         "};\n";
    for (const std::uint32_t stream : m_streams) {
        h += std::format("layout(std430, binding = {0}) readonly buffer XeVertexStream{0}\n{{\n    uint xe_vb{0}[];\n}};\n",
            stream);
    }
    for (const SamplerUse& use : m_result.samplers) {
        const char* type = use.dimension == TextureDimension::D3 ? "sampler3D"
            : use.dimension == TextureDimension::Cube            ? "samplerCube"
                                                                 : "sampler2D";
        const auto name = m_options.samplerNames.find(use.slot);
        h += std::format("layout(binding = {}) uniform {} {};{}\n", use.slot, type, use.glslName,
            name != m_options.samplerNames.end() ? " // " + name->second : std::string());
    }
    if (m_vertex) {
        for (std::uint32_t k = 0; k < m_options.exportLocations.size(); ++k) {
            if (m_options.exportLocations[k] >= 0) {
                h += std::format("layout(location = {0}) out vec4 xe_var{0};\n", m_options.exportLocations[k]);
            }
        }
    } else {
        for (std::uint32_t r = 0; r < m_options.inputLocations.size(); ++r) {
            if (m_options.inputLocations[r] >= 0) {
                h += std::format("layout(location = {0}) in vec4 xe_var{0};\n", m_options.inputLocations[r]);
            }
        }
        for (std::uint32_t c = 0; c < exportreg::kColorCount; ++c) {
            if (((m_result.colorOutputs >> c) & 1) != 0) {
                h += std::format("layout(location = {0}) out vec4 xe_color{0};\n", c);
            }
        }
    }
    h += std::format("\nvec4 r[{}];\n", m_registerCount);
    h += "float xe_ps = 0.0;\nbool xe_p0 = false;\nint xe_a0 = 0;\nint xe_aL = 0;\n";
    if (m_vertex) {
        h += "uint xe_vindex = 0u;\nvec4 xe_o[16];\nvec4 xe_oPos = vec4(0.0);\nvec4 xe_oPts = vec4(0.0);\n";
    } else {
        h += "vec4 xe_oC[4];\nvec4 xe_oDepth = vec4(0.0);\n";
    }
    h += "float xe_lod = 0.0;\nvec3 xe_gradH = vec3(0.0);\nvec3 xe_gradV = vec3(0.0);\n";
    h += kHelpers;
    return h;
}

std::string Translator::mainFunction() const
{
    std::string m = "\nvoid main()\n{\n";
    m += std::format("    for (int i = 0; i < {}; ++i) {{\n        r[i] = vec4(0.0);\n    }}\n", m_registerCount);
    if (m_vertex) {
        m += "    for (int i = 0; i < 16; ++i) {\n        xe_o[i] = vec4(0.0);\n    }\n";
        m += "    r[0].x = float(gl_VertexID);\n";
        m += "    xe_body();\n";
        m += "    gl_Position = vec4(xe_oPos.xyz * xe_clipScale.xyz + xe_oPos.w * xe_clipOffset.xyz, xe_oPos.w);\n";
        if (m_analysis.writesPointSize) {
            m += "    gl_PointSize = xe_oPts.x;\n";
        }
        for (std::uint32_t k = 0; k < m_options.exportLocations.size(); ++k) {
            if (m_options.exportLocations[k] >= 0) {
                m += std::format("    xe_var{} = xe_o[{}];\n", m_options.exportLocations[k], k);
            }
        }
    } else {
        m += "    for (int i = 0; i < 4; ++i) {\n        xe_oC[i] = vec4(0.0);\n    }\n";
        for (std::uint32_t r = 0; r < m_options.inputLocations.size(); ++r) {
            if (m_options.inputLocations[r] >= 0 && r < m_registerCount) {
                m += std::format("    r[{}] = xe_var{};\n", r, m_options.inputLocations[r]);
            }
        }
        if (m_options.parameterRegister && *m_options.parameterRegister < m_registerCount) {
            // x: pixel x, sign set for back faces; y: pixel y, from the top
            // when xe_screen.x is set (as Direct3D counts); z, w: 0 for
            // triangles.
            m += "    float xe_px = abs(floor(gl_FragCoord.x));\n";
            m += "    xe_px = gl_FrontFacing ? xe_px : uintBitsToFloat(floatBitsToUint(xe_px) | 0x80000000u);\n";
            m += "    float xe_py = xe_screen.x != 0.0 ? floor(xe_screen.y - gl_FragCoord.y) : floor(gl_FragCoord.y);\n";
            m += std::format("    r[{}] = vec4(xe_px, abs(xe_py), 0.0, 0.0);\n", *m_options.parameterRegister);
        }
        m += "    xe_body();\n";
        if ((m_result.colorOutputs & 1) != 0) {
            m += "    uint xe_alphaFunction = uint(xe_alphaTest.x);\n";
            m += "    float xe_alpha = xe_oC[0].a;\n";
            m += "    float xe_reference = xe_alphaTest.y;\n";
            m += "    bool xe_pass = xe_alphaFunction == 0u || xe_alphaFunction == 8u\n"
                 "        || (xe_alphaFunction == 2u && xe_alpha < xe_reference)\n"
                 "        || (xe_alphaFunction == 3u && xe_alpha == xe_reference)\n"
                 "        || (xe_alphaFunction == 4u && xe_alpha <= xe_reference)\n"
                 "        || (xe_alphaFunction == 5u && xe_alpha > xe_reference)\n"
                 "        || (xe_alphaFunction == 6u && xe_alpha != xe_reference)\n"
                 "        || (xe_alphaFunction == 7u && xe_alpha >= xe_reference);\n";
            m += "    if (!xe_pass) {\n        discard;\n    }\n";
        }
        for (std::uint32_t c = 0; c < exportreg::kColorCount; ++c) {
            if (((m_result.colorOutputs >> c) & 1) != 0) {
                m += std::format("    xe_color{0} = xe_oC[{0}];\n", c);
                m += std::format("    if ((xe_flags.x & 1u) != 0u) {{\n        xe_color{0}.rgb = "
                                 "xe_gamma_encode(xe_color{0}.rgb);\n    }}\n",
                    c);
            }
        }
        if (m_result.writesDepth) {
            m += "    gl_FragDepth = xe_oDepth.x * xe_clipScale.z + xe_clipOffset.z;\n";
        }
    }
    m += "}\n";
    return m;
}

TranslationResult Translator::run()
{
    analyse();
    if (m_analysis.needsDispatcher) {
        emitDispatcher();
    } else {
        emitStraight();
    }
    m_result.streams.assign(m_streams.begin(), m_streams.end());
    if (m_vertex) {
        for (std::uint32_t k = 0; k < m_options.exportLocations.size(); ++k) {
            if (m_options.exportLocations[k] >= 0) {
                m_result.varyings.push_back(m_options.exportLocations[k]);
            }
        }
    } else {
        for (std::uint32_t r = 0; r < m_options.inputLocations.size(); ++r) {
            if (m_options.inputLocations[r] >= 0) {
                m_result.varyings.push_back(m_options.inputLocations[r]);
            }
        }
    }
    m_result.glsl = header() + "\n" + m_body + mainFunction();
    return std::move(m_result);
}

} // namespace

VertexFetchLayout layoutFromDeclaration(const VertexDeclElement& element, std::uint32_t stride)
{
    VertexFetchLayout layout;
    layout.stream = element.stream;
    layout.byteOffset = element.offset;
    layout.byteStride = stride;
    layout.format = element.format();
    layout.endian = element.endian();
    layout.isSigned = element.isSigned();
    layout.integer = element.isInteger();
    for (std::uint32_t i = 0; i < 4; ++i) {
        layout.selectors[i] = static_cast<std::uint8_t>(element.selector(i));
    }
    return layout;
}

std::uint32_t samplerFetchConstant(ShaderStage stage, std::uint32_t samplerRegister)
{
    return stage == ShaderStage::Vertex ? samplerRegister + kVertexSamplerBase : samplerRegister;
}

TranslationOptions defaultOptions(const Shader& shader, const VertexDeclaration* declaration)
{
    TranslationOptions options;
    for (const LiteralConstant& literal : shader.literals) {
        options.literals[literal.reg] = literal.value;
    }
    for (const ConstantInfo& c : shader.constants.constants) {
        if (c.set == RegisterSet::Sampler) {
            for (std::uint32_t k = 0; k < c.registerCount; ++k) {
                options.samplerNames[samplerFetchConstant(shader.stage, c.registerIndex + k)] = c.name;
            }
        }
    }
    options.exportLocations.fill(-1);
    for (const Interpolator& interpolator : shader.interpolators) {
        if (interpolator.reg < 16) {
            if (shader.stage == ShaderStage::Vertex) {
                options.exportLocations[interpolator.reg] = static_cast<int>(interpolator.reg);
            } else {
                options.inputLocations[interpolator.reg] = static_cast<int>(interpolator.reg);
            }
        }
    }
    options.parameterRegister = shader.parameterRegister();
    if (shader.stage == ShaderStage::Vertex && declaration != nullptr) {
        for (const VertexElementUse& use : shader.vertexElements) {
            if (const VertexDeclElement* element = declaration->find(use.usage, use.usageIndex)) {
                options.vertexFetches[use.fetchSlot] = layoutFromDeclaration(*element, declaration->stride(element->stream));
            }
        }
    }
    return options;
}

TranslationResult translate(const Program& program, const TranslationOptions& options)
{
    Translator translator(program, options);
    return translator.run();
}

TranslationResult translate(const Shader& shader, const VertexDeclaration* declaration)
{
    const Program program = decodeProgram(shader.stage, shader.microcode);
    return translate(program, defaultOptions(shader, declaration));
}

} // namespace fh::xenos
