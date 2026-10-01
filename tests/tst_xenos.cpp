// The Xenos shader library on hand-assembled microcode, a built effect container
// and the game's effects.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "vfs/Vfs.h"
#include "xenos/Disassembler.h"
#include "xenos/Effect.h"
#include "xenos/GlslTranslator.h"
#include "xenos/Microcode.h"
#include "xenos/ShaderCheck.h"
#include "xenos/ShaderProgram.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

using openhorizon::test::gameDir;

namespace {

using openhorizon::Bytes;
namespace xenos = openhorizon::xenos;

constexpr std::uint32_t kFetchBit = 1;
constexpr std::uint32_t kOne = 5;

/// A 48-bit exec-family control flow instruction.
std::uint64_t cfExec(xenos::CfOpcode opcode, std::uint32_t address, std::uint32_t count, std::uint32_t sequence,
    std::uint32_t boolAddress = 0, bool condition = false)
{
    return address | (std::uint64_t{count} << 12) | (std::uint64_t{sequence} << 16)
        | (std::uint64_t{boolAddress} << 34) | (std::uint64_t{condition ? 1U : 0U} << 42)
        | (std::uint64_t{static_cast<std::uint32_t>(opcode)} << 44);
}

/// Loop start/end, call and jump: target address, loop id, opcode.
std::uint64_t cfTarget(xenos::CfOpcode opcode, std::uint32_t address, std::uint32_t loopId = 0,
    std::uint32_t boolAddress = 0, bool condition = false, bool unconditional = false)
{
    return address | (std::uint64_t{unconditional ? 1U : 0U} << 13) | (std::uint64_t{loopId} << 16)
        | (std::uint64_t{boolAddress} << 34) | (std::uint64_t{condition ? 1U : 0U} << 42)
        | (std::uint64_t{static_cast<std::uint32_t>(opcode)} << 44);
}

void appendCfPair(std::vector<std::uint32_t>& words, std::uint64_t a, std::uint64_t b)
{
    words.push_back(static_cast<std::uint32_t>(a));
    words.push_back(static_cast<std::uint32_t>((a >> 32) & 0xFFFF) | static_cast<std::uint32_t>((b & 0xFFFF) << 16));
    words.push_back(static_cast<std::uint32_t>(b >> 16));
}

/// Fields of one ALU slot, encoded by alu().
struct AluFields {
    std::uint32_t vectorOpcode = 2;
    std::uint32_t vectorDest = 0;
    std::uint32_t vectorMask = 0;
    std::uint32_t scalarOpcode = 50;
    std::uint32_t scalarDest = 0;
    std::uint32_t scalarMask = 0;
    bool exportData = false;
    bool scalarDestRelative = false;
    bool vectorClamp = false;
    /// Raw source registers (temporaries: index | 0x40 relative | 0x80 abs).
    std::array<std::uint32_t, 3> reg{};
    std::array<bool, 3> temporary{true, true, true};
    std::array<std::uint32_t, 3> swizzle{};
    std::array<bool, 3> negate{};
};

std::array<std::uint32_t, 3> alu(const AluFields& f)
{
    const std::uint32_t w0 = f.vectorDest | (f.scalarDest << 8) | ((f.scalarDestRelative ? 1U : 0U) << 14)
        | ((f.exportData ? 1U : 0U) << 15) | (f.vectorMask << 16) | (f.scalarMask << 20)
        | ((f.vectorClamp ? 1U : 0U) << 24) | (f.scalarOpcode << 26);
    const std::uint32_t w1 = f.swizzle[2] | (f.swizzle[1] << 8) | (f.swizzle[0] << 16) | ((f.negate[2] ? 1U : 0U) << 24)
        | ((f.negate[1] ? 1U : 0U) << 25) | ((f.negate[0] ? 1U : 0U) << 26);
    const std::uint32_t w2 = f.reg[2] | (f.reg[1] << 8) | (f.reg[0] << 16) | (f.vectorOpcode << 24)
        | ((f.temporary[2] ? 1U : 0U) << 29) | ((f.temporary[1] ? 1U : 0U) << 30) | ((f.temporary[0] ? 1U : 0U) << 31);
    return {w0, w1, w2};
}

/// A vertex fetch from stream 0 (fetch constant 95) indexed by r0.x.
std::array<std::uint32_t, 3> vfetch(std::uint32_t dst, std::uint32_t dstSwizzle, std::uint32_t format, bool isSigned,
    bool integer, std::uint32_t strideWords, std::uint32_t offsetWords)
{
    const std::uint32_t w0 = (dst << 12) | (1U << 19) | (31U << 20) | (2U << 25);
    const std::uint32_t w1 = dstSwizzle | ((isSigned ? 1U : 0U) << 12) | ((integer ? 1U : 0U) << 13) | (format << 16);
    const std::uint32_t w2 = strideWords | (offsetWords << 8);
    return {w0, w1, w2};
}

/// A 2D texture fetch of fetch constant `slot` at src.xy into dst.xyzw.
std::array<std::uint32_t, 3> tfetch2d(std::uint32_t dst, std::uint32_t src, std::uint32_t slot)
{
    const std::uint32_t srcSwizzle = 0 | (1U << 2);
    const std::uint32_t w0 = 1U | (src << 5) | (dst << 12) | (1U << 19) | (slot << 20) | (srcSwizzle << 26);
    const std::uint32_t w1 = 0x688U | (3U << 12) | (3U << 14) | (3U << 16) | (7U << 18) | (3U << 24) | (3U << 26)
        | (1U << 28);
    const std::uint32_t w2 = 1U << 14;
    return {w0, w1, w2};
}

void append(std::vector<std::uint32_t>& words, const std::array<std::uint32_t, 3>& slot)
{
    words.insert(words.end(), slot.begin(), slot.end());
}

bool contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

void checkCompiles(const std::string& glsl, xenos::ShaderStage stage)
{
    const auto error = xenos::compileGlsl(glsl, stage, "test");
    CHECK(!error.has_value());
    if (error) {
        std::fprintf(stderr, "%s\n%s\n", error->c_str(), glsl.c_str());
    }
}

/// Fetches a float3 position into r1 (w = 1), r2 = r1 * c4 with rcp(c5.w) in
/// r2.w; exports r2 as position and r1 as interpolator 0.
std::vector<std::uint32_t> vertexProgram()
{
    std::vector<std::uint32_t> words;
    appendCfPair(words, cfExec(xenos::CfOpcode::Exec, 1, 1, kFetchBit),
        cfExec(xenos::CfOpcode::ExecEnd, 2, 3, 0));
    append(words, vfetch(1, 0 | (1U << 3) | (2U << 6) | (kOne << 9), 57, true, true, 3, 0));
    AluFields mul;
    mul.vectorOpcode = static_cast<std::uint32_t>(xenos::VectorOpcode::Mul);
    mul.vectorDest = 2;
    mul.vectorMask = 0x7;
    mul.reg = {1, 4, 5};
    mul.temporary = {true, false, false};
    mul.scalarOpcode = static_cast<std::uint32_t>(xenos::ScalarOpcode::Rcp);
    mul.scalarDest = 2;
    mul.scalarMask = 0x8;
    append(words, alu(mul));
    AluFields position;
    position.vectorDest = xenos::exportreg::kPosition;
    position.vectorMask = 0xF;
    position.exportData = true;
    position.reg = {2, 2, 0};
    append(words, alu(position));
    AluFields interpolator;
    interpolator.vectorDest = 0;
    interpolator.vectorMask = 0xF;
    interpolator.exportData = true;
    interpolator.reg = {1, 1, 0};
    append(words, alu(interpolator));
    return words;
}

/// Exports tf3 sampled at r0.xy times c2 as colour 0, alpha forced to one by
/// the export's constant mask.
std::vector<std::uint32_t> pixelProgram()
{
    std::vector<std::uint32_t> words;
    appendCfPair(words, cfExec(xenos::CfOpcode::Exec, 1, 1, kFetchBit), cfExec(xenos::CfOpcode::ExecEnd, 2, 1, 0));
    append(words, tfetch2d(1, 0, 3));
    AluFields colour;
    colour.vectorOpcode = static_cast<std::uint32_t>(xenos::VectorOpcode::Mul);
    colour.vectorDest = 0;
    colour.vectorMask = 0xF;
    colour.scalarMask = 0x8;
    colour.exportData = true;
    colour.reg = {1, 2, 0};
    colour.temporary = {true, false, true};
    append(words, alu(colour));
    return words;
}

TEST_CASE("decodesControlFlow", "[xenos]")
{
    const auto pair = xenos::unpackCfPair(0x12345678, 0x9ABCDEF0, 0x0FEDCBA9);
    CHECK(pair[0] == std::uint64_t{0xDEF012345678});
    CHECK(pair[1] == std::uint64_t{0x0FEDCBA99ABC});

    const xenos::CfInstruction exec
        = xenos::decodeCf(cfExec(xenos::CfOpcode::CondExecEnd, 9, 5, 0b1000'0001, 131, true));
    CHECK(exec.opcode == xenos::CfOpcode::CondExecEnd);
    CHECK(exec.address == 9U);
    CHECK(exec.count == 5U);
    CHECK(exec.slotIsFetch(0));
    CHECK(!exec.slotIsFetch(1));
    CHECK(!exec.slotIsFetch(3));
    CHECK(exec.slotSerialized(3));
    CHECK(exec.testsBool());
    CHECK(exec.boolAddress == 131U);
    CHECK(exec.condition);
    CHECK(exec.endsShader());

    const xenos::CfInstruction loopEnd = xenos::decodeCf(cfTarget(xenos::CfOpcode::LoopEnd, 4, 3));
    CHECK(loopEnd.opcode == xenos::CfOpcode::LoopEnd);
    CHECK(loopEnd.address == 4U);
    CHECK(loopEnd.loopId == 3U);

    const std::uint64_t allocBits = 2U | (std::uint64_t{2} << 41) | (std::uint64_t{12} << 44);
    const xenos::CfInstruction alloc = xenos::decodeCf(allocBits);
    CHECK(alloc.allocType == xenos::AllocType::Parameters);
    CHECK(alloc.allocSize == 2U);
    CHECK(xenos::disassembleCf(alloc) == std::string("alloc interpolators/colors size=3"));
}

TEST_CASE("decodesAluAndFetches", "[xenos]")
{
    // The first ALU slot of a track vertex shader:
    // mul r0.xyz_, r0.yzww, c7.xyzz  +  sges r2.___w, |r0|.x
    const xenos::AluInstruction a = xenos::decodeAlu(0x24870200, 0x0015c06c, 0xa1000780);
    CHECK(a.vectorOpcode == static_cast<std::uint32_t>(xenos::VectorOpcode::Mul));
    CHECK(a.scalarOpcode == static_cast<std::uint32_t>(xenos::ScalarOpcode::Sges));
    CHECK(a.vectorWriteMask == 0x7U);
    CHECK(a.scalarDest == 2U);
    const xenos::AluOperand first = a.vectorOperand(0);
    CHECK(first.temporary);
    CHECK(first.components[0] == 1);
    CHECK(first.components[3] == 3);
    const xenos::AluOperand second = a.vectorOperand(1);
    CHECK(!second.temporary);
    CHECK(second.index == 7U);
    const xenos::AluOperand scalar = a.scalarOperand();
    CHECK(scalar.absolute);
    CHECK(scalar.components[3] == 0);
    CHECK(xenos::disassembleAlu(a, xenos::ShaderStage::Vertex)
        == std::string("mul r0.xyz_, r0.yzww, c7.xyzz  +  sges r2.___w, |r0|.x"));

    // mulsc r0.___w, c255.y, r0.w: the temporary is in the opcode's low bit,
    // the third source's select bit and its swizzle.
    const xenos::AluInstruction m = xenos::decodeAlu(0xa8810005, 0x006c1b83, 0xc10500ff);
    const xenos::AluOperand temporary = m.scalarTemporaryOperand();
    const xenos::AluOperand constant = m.scalarOperand();
    CHECK(temporary.index == 0U);
    CHECK(temporary.components[0] == 3);
    CHECK(!constant.temporary);
    CHECK(constant.index == 255U);
    CHECK(constant.components[3] == 1);

    const auto v = vfetch(1, 0x688, 57, true, true, 5, 3);
    const xenos::FetchInstruction f = xenos::decodeFetch(v[0], v[1], v[2]);
    CHECK(f.isVertexFetch());
    CHECK(f.constIndex == 95U);
    CHECK(f.format == 57U);
    CHECK(f.stride == 5U);
    CHECK(f.offset == 3);
    CHECK(!f.normalized);

    // tfetch r3.wzyx, r3.zyz, tf13 2D offset=(0.5,0.5,0), from a track pixel shader.
    const xenos::FetchInstruction t = xenos::decodeFetch(0x98d83061, 0x1f1ff053, 0x00214000);
    CHECK(t.opcode == 1U);
    CHECK(t.constIndex == 13U);
    CHECK(t.dimension == xenos::TextureDimension::D2);
    CHECK_THAT(t.offsetX, WithinAbs(0.5, 1e-6));
    CHECK_THAT(t.offsetY, WithinAbs(0.5, 1e-6));
    CHECK(t.srcComponent(0) == 2U);
    CHECK(t.dstSelector(0) == 3U);
}

TEST_CASE("translatesVertexShader", "[xenos]")
{
    const xenos::Program program = xenos::decodeProgram(xenos::ShaderStage::Vertex, vertexProgram());
    CHECK(program.cf.size() == std::size_t{2});
    const std::string listing = xenos::disassemble(program);
    CHECK(contains(listing, "vfetch_full r1.xyz1, r0.x, vf95 format=32_32_32_FLOAT"));
    CHECK(contains(listing, "mul r2.xyz_, r1, c4  +  rcp r2.___w, c5.w"));
    CHECK(contains(listing, "max oPos.xyzw, r2, r2"));

    xenos::TranslationOptions options;
    options.exportLocations.fill(-1);
    options.exportLocations[0] = 0;
    options.literals[5] = {0.0F, 0.0F, 0.0F, 4.0F};
    const xenos::TranslationResult result = xenos::translate(program, options);
    CHECK(result.unsupported.empty());
    CHECK(result.streams.size() == std::size_t{1});
    CHECK(contains(result.glsl, "uint xe_at = xe_streamBase[0u][0u] + xe_vindex * 3u + 0u;"));
    CHECK(contains(result.glsl, "vec4(uintBitsToFloat(xe_w.xyz), 1.0)"));
    CHECK(contains(result.glsl, "xe_v = xe_mul(xe_v0, xe_v1);"));
    CHECK(contains(result.glsl, "vec4 xe_v1 = xe_consts.c[4];"));
    CHECK(contains(result.glsl, "vec4 xe_sa = vec4(0.0, 0.0, 0.0, 4.0);"));
    CHECK(contains(result.glsl, "xe_s = xe_rcp(xe_sa.w);"));
    CHECK(contains(result.glsl, "xe_oPos.xyzw = xe_v.xyzw;"));
    CHECK(contains(result.glsl, "xe_var0 = xe_o[0];"));
    CHECK(!contains(result.glsl, "switch (xe_pc)"));
    checkCompiles(result.glsl, xenos::ShaderStage::Vertex);

    // Through a declaration element: big-endian USHORT2N at byte 16 of a 20-byte vertex.
    xenos::VertexDeclElement uv;
    uv.offset = 16;
    uv.type = 0x2C2059;
    uv.usage = static_cast<std::uint8_t>(xenos::DeclUsage::Texcoord);
    CHECK(uv.format() == xenos::VertexFormat::F16_16);
    CHECK(uv.endian() == 1U);
    CHECK(!uv.isSigned());
    CHECK(uv.selector(2) == 4U);
    CHECK(uv.selector(3) == 5U);
    options.vertexFetches[1] = xenos::layoutFromDeclaration(uv, 20);
    const xenos::TranslationResult bound = xenos::translate(program, options);
    CHECK(contains(bound.glsl, "xe_vindex * 5u + 4u;"));
    CHECK(contains(bound.glsl, "xe_swap(uvec4(xe_vb0[xe_at + 0u], 0u, 0u, 0u), 1u)"));
    CHECK(contains(bound.glsl, "vec4 xe_f = vec4(xe_raw.x, xe_raw.y, 0.0, 1.0);"));
    checkCompiles(bound.glsl, xenos::ShaderStage::Vertex);
}

TEST_CASE("translatesPixelShader", "[xenos]")
{
    const xenos::Program program = xenos::decodeProgram(xenos::ShaderStage::Pixel, pixelProgram());
    const std::string listing = xenos::disassemble(program);
    CHECK(contains(listing, "tfetch r1.xyzw, r0.xyx, tf3 2D"));
    CHECK(contains(listing, "mul oC0.xyz_, r1, c2 one.___w"));

    xenos::TranslationOptions options;
    options.inputLocations[0] = 0;
    options.parameterRegister = 5;
    const xenos::TranslationResult result = xenos::translate(program, options);
    CHECK(result.unsupported.empty());
    CHECK(result.colorOutputs == 1U);
    CHECK(result.samplers.size() == std::size_t{1});
    CHECK(result.samplers[0].slot == 3U);
    CHECK(contains(result.glsl, "layout(binding = 3) uniform sampler2D xe_tex3;"));
    CHECK(contains(result.glsl, "vec4 xe_f = texture(xe_tex3, xe_uv);"));
    CHECK(contains(result.glsl, "xe_oC[0].w = 1.0;"));
    CHECK(contains(result.glsl, "r[0] = xe_var0;"));
    CHECK(contains(result.glsl, "gl_FrontFacing"));
    CHECK(contains(result.glsl, "layout(location = 0) out vec4 xe_color0;"));
    checkCompiles(result.glsl, xenos::ShaderStage::Pixel);
}

TEST_CASE("translatesLoopsAndJumps", "[xenos]")
{
    // 0 loop i2 skip-to 3; 1 exec (r0 += c0); 2 endloop i2 body 1;
    // 3 cjmp !b140 to 5; 4 exec (kill if r0 > c1); 5 exece (oC0 = r0).
    std::vector<std::uint32_t> words;
    appendCfPair(words, cfTarget(xenos::CfOpcode::LoopStart, 3, 2), cfExec(xenos::CfOpcode::Exec, 3, 1, 0));
    appendCfPair(words, cfTarget(xenos::CfOpcode::LoopEnd, 1, 2), cfTarget(xenos::CfOpcode::CondJmp, 5, 0, 140, false));
    appendCfPair(words, cfExec(xenos::CfOpcode::Exec, 4, 1, 0), cfExec(xenos::CfOpcode::ExecEnd, 5, 1, 0));
    AluFields add;
    add.vectorOpcode = static_cast<std::uint32_t>(xenos::VectorOpcode::Add);
    add.vectorMask = 0xF;
    add.reg = {0, 0, 0};
    add.temporary = {true, false, true};
    append(words, alu(add));
    AluFields kill;
    kill.vectorOpcode = static_cast<std::uint32_t>(xenos::VectorOpcode::KillGt);
    kill.reg = {0, 1, 0};
    kill.temporary = {true, false, true};
    append(words, alu(kill));
    AluFields colour;
    colour.vectorDest = 0;
    colour.vectorMask = 0xF;
    colour.exportData = true;
    append(words, alu(colour));

    const xenos::Program program = xenos::decodeProgram(xenos::ShaderStage::Pixel, words);
    CHECK(program.cf.size() == std::size_t{6});
    CHECK(program.cf[0].opcode == xenos::CfOpcode::LoopStart);
    CHECK(program.cf[0].loopId == 2U);
    CHECK(program.cf[3].opcode == xenos::CfOpcode::CondJmp);
    CHECK(program.cf[3].boolAddress == 140U);
    const std::string listing = xenos::disassemble(program);
    CHECK(contains(listing, "loop i2 skip-to 3"));
    CHECK(contains(listing, "cjmp !b140 5"));
    CHECK(contains(listing, "kill_gt r0.____, r0, c1"));

    const xenos::TranslationResult result = xenos::translate(program, xenos::TranslationOptions{});
    CHECK(result.usesKill);
    CHECK(contains(result.glsl, "switch (xe_pc) {"));
    CHECK(contains(result.glsl, "uint xe_constant = xe_loop(2u);"));
    CHECK(contains(result.glsl, "if (!xe_bool(140u)) {"));
    CHECK(contains(result.glsl, "discard;"));
    checkCompiles(result.glsl, xenos::ShaderStage::Pixel);

    xenos::ControlConstants control;
    control.setBool(140, true);
    control.setLoop(2, 3, 1, -1);
    CHECK(control.bools[4] == 1U << 12);
    CHECK(control.loops[2] == (3U | (1U << 8) | (0xFFU << 16)));
    const auto packed = control.pack();
    CHECK(packed.size() == std::size_t{160});
}

TEST_CASE("rejectsBrokenMicrocode", "[xenos]")
{
    std::vector<std::uint32_t> words;
    appendCfPair(words, cfExec(xenos::CfOpcode::ExecEnd, 7, 2, 0), 0);
    CHECK_THROWS_AS(xenos::decodeProgram(xenos::ShaderStage::Vertex, words), std::exception);
    words.push_back(0);
    CHECK_THROWS_AS(xenos::decodeProgram(xenos::ShaderStage::Vertex, words), std::exception);
    std::vector<std::uint32_t> endless;
    appendCfPair(endless, cfExec(xenos::CfOpcode::Exec, 1, 1, 0), 0);
    append(endless, alu(AluFields{}));
    CHECK_THROWS_AS(xenos::decodeProgram(xenos::ShaderStage::Vertex, endless), std::exception);
}

/// Builds big-endian data.
class Writer {
public:
    void u32(std::uint32_t v)
    {
        for (int shift = 24; shift >= 0; shift -= 8) {
            m_bytes.push_back(static_cast<std::uint8_t>(v >> shift));
        }
    }
    void u16(std::uint16_t v)
    {
        m_bytes.push_back(static_cast<std::uint8_t>(v >> 8));
        m_bytes.push_back(static_cast<std::uint8_t>(v));
    }
    void u8(std::uint8_t v) { m_bytes.push_back(v); }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void text(std::string_view s)
    {
        m_bytes.insert(m_bytes.end(), s.begin(), s.end());
        m_bytes.push_back(0);
    }
    void pad(std::size_t alignment)
    {
        while (m_bytes.size() % alignment != 0) {
            m_bytes.push_back(0);
        }
    }
    void zeros(std::size_t count) { m_bytes.insert(m_bytes.end(), count, 0); }
    void at(std::size_t offset, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            m_bytes[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
        }
    }
    void textAt(std::size_t offset, std::string_view s) { std::copy(s.begin(), s.end(), m_bytes.begin() + offset); }
    void append(const Bytes& more) { m_bytes.insert(m_bytes.end(), more.begin(), more.end()); }
    std::size_t size() const { return m_bytes.size(); }
    const Bytes& bytes() const { return m_bytes; }

private:
    Bytes m_bytes;
};

struct TestConstant {
    std::string name;
    std::uint16_t set;
    std::uint16_t reg;
    std::uint16_t count;
    std::uint16_t type;
};

/// A shader blob as Effect.h documents it.
Bytes shaderBlob(xenos::ShaderStage stage, const std::vector<std::uint32_t>& microcode,
    const std::vector<TestConstant>& constants, const std::vector<std::uint32_t>& elements,
    const std::vector<std::uint32_t>& interpolators)
{
    const bool vertex = stage == xenos::ShaderStage::Vertex;
    Writer table;
    table.u32(28);
    table.u32(0);
    table.u32(vertex ? 0xFFFE0300 : 0xFFFF0300);
    table.u32(static_cast<std::uint32_t>(constants.size()));
    table.u32(28);
    table.u32(0);
    table.u32(0);
    const std::size_t records = table.size();
    table.zeros(constants.size() * 20);
    std::vector<std::size_t> typeOffsets;
    for (const TestConstant& c : constants) {
        typeOffsets.push_back(table.size());
        table.u16(c.set == 3 ? 4 : 1);
        table.u16(c.type);
        table.u16(1);
        table.u16(c.set == 3 ? 1 : 4);
        table.u16(1);
        table.u16(0);
        table.u32(0);
    }
    for (std::size_t i = 0; i < constants.size(); ++i) {
        const std::size_t name = table.size();
        table.text(constants[i].name);
        const std::size_t r = records + i * 20;
        table.at(r, static_cast<std::uint32_t>(name));
        table.at(r + 4, (std::uint32_t{constants[i].set} << 16) | constants[i].reg);
        table.at(r + 8, std::uint32_t{constants[i].count} << 16);
        table.at(r + 12, static_cast<std::uint32_t>(typeOffsets[i]));
    }
    const std::size_t target = table.size();
    table.text(vertex ? "vs_3_0" : "ps_3_0");
    table.at(4, static_cast<std::uint32_t>(target));
    table.at(24, static_cast<std::uint32_t>(target));
    table.pad(4);

    Writer blob;
    blob.zeros(36);
    const std::size_t constantOffset = blob.size();
    blob.u32(static_cast<std::uint32_t>(table.size()));
    blob.append(table.bytes());
    const std::size_t literalOffset = blob.size();
    blob.u32(0);
    blob.u32(1);
    blob.u32(0);
    blob.u32(0);
    blob.u32(20);
    blob.u16(vertex ? 255 : 511);
    blob.u16(4);
    blob.u32(0);
    blob.zeros(12);
    const std::size_t infoOffset = blob.size();
    const auto microcodeBytes = static_cast<std::uint32_t>(microcode.size() * 4);
    blob.u32(16);
    blob.u32(microcodeBytes);
    blob.u32(vertex ? 0x00010002 : 0x10000200);
    blob.u32(vertex ? 0 : 8);
    blob.u32(0);
    blob.u32(0x1234);
    if (vertex) {
        blob.u32(1);
        blob.u32(static_cast<std::uint32_t>(elements.size()));
        blob.u32(0);
        blob.u32(0x290);
        for (const std::uint32_t e : elements) {
            blob.u32(e);
        }
    } else {
        blob.u32(0);
        blob.u32(1);
    }
    for (const std::uint32_t i : interpolators) {
        blob.u32(i);
    }
    const std::size_t virtualSize = blob.size();
    blob.f32(0.5F);
    blob.f32(1.0F);
    blob.f32(2.0F);
    blob.f32(3.0F);
    for (const std::uint32_t w : microcode) {
        blob.u32(w);
    }
    blob.at(0, vertex ? xenos::kVertexShaderMagic : xenos::kPixelShaderMagic);
    blob.at(4, static_cast<std::uint32_t>(virtualSize));
    blob.at(8, static_cast<std::uint32_t>(blob.size() - virtualSize));
    blob.at(16, static_cast<std::uint32_t>(constantOffset));
    blob.at(20, static_cast<std::uint32_t>(literalOffset));
    blob.at(24, static_cast<std::uint32_t>(infoOffset));
    return blob.bytes();
}

/// Technique "Main", pass "p0" with the test shaders, declaration float3
/// position and USHORT2N uv.
Bytes testEffect()
{
    const Bytes vertex = shaderBlob(xenos::ShaderStage::Vertex, vertexProgram(),
        {{"WorldViewProj", 2, 4, 1, 3}, {"Fade", 2, 5, 1, 3}}, {0x00000001},
        {0x0000F050});
    const Bytes pixel = shaderBlob(xenos::ShaderStage::Pixel, pixelProgram(),
        {{"Tint", 2, 2, 1, 3}, {"DiffuseSampler", 3, 3, 1, 12}, {"UseFog", 0, 5, 1, 1}}, {}, {0x0000F050});

    Writer file;
    file.u32(0x101);
    file.u32(0xCAFEF00D);
    file.u32(0);
    file.u32(1);
    file.u32(1);
    file.u32(1);
    file.u32(0);
    file.u32(0xA3D70141);
    file.u32(0);
    file.u32(0);
    const std::size_t base = file.size();
    file.zeros(0x400);
    file.at(base + 0x200, 0x210);
    file.at(base + 0x20C, 1);
    file.at(base + 0x210, 0x220);
    file.at(base + 0x220, 0x300);
    file.at(base + 0x224, 1);
    file.at(base + 0x230, 0x240);
    file.at(base + 0x240, 0x308);
    file.at(base + 0x248, 0x260);
    file.at(base + 0x260 + 0x44, 0xFFFFFFFF);
    const auto vertexObject = static_cast<std::uint32_t>(file.size() - base);
    file.at(base + 0x260 + 0x48, vertexObject);
    file.u32(0);
    file.u32(static_cast<std::uint32_t>(vertex.size()));
    file.append(vertex);
    const auto pixelObject = static_cast<std::uint32_t>(file.size() - base);
    file.at(base + 0x260 + 0x4C, pixelObject);
    file.u32(0);
    file.u32(static_cast<std::uint32_t>(pixel.size()));
    file.append(pixel);
    file.textAt(base + 0x300, "Main");
    file.textAt(base + 0x308, "p0");
    file.at(base - 8, static_cast<std::uint32_t>(file.size() - base));

    file.u32(0x00100005);
    file.u32(1);
    file.zeros(12);
    file.u32(0xFFFF0000);
    file.u32(2);
    file.u32(0);
    file.u32(0xFF000000);
    file.zeros(16);
    file.u16(0);
    file.u16(0);
    file.u32(0x2A23B9);
    file.u8(0);
    file.u8(0);
    file.u8(0);
    file.u8(0);
    file.u16(0);
    file.u16(12);
    file.u32(0x2C2059);
    file.u8(0);
    file.u8(5);
    file.u8(0);
    file.u8(0);
    file.u32(0);
    file.text("position");
    file.text("uv");
    file.at(8, static_cast<std::uint32_t>(file.size()));
    return file.bytes();
}

TEST_CASE("parsesEffectContainer", "[xenos]")
{
    const Bytes data = testEffect();
    const xenos::Effect effect = xenos::parseEffect(data, "test.fxobj");
    CHECK(effect.techniques.size() == std::size_t{1});
    CHECK(effect.shaders.size() == std::size_t{2});
    CHECK(effect.declarations.size() == std::size_t{1});
    if (effect.techniques.size() != 1 || effect.shaders.size() != 2 || effect.declarations.size() != 1) {
        return;
    }
    const xenos::Technique& technique = effect.techniques[0];
    CHECK(technique.name == std::string("Main"));
    CHECK(technique.passes.size() == std::size_t{1});
    CHECK(technique.passes[0].name == std::string("p0"));
    CHECK(technique.passes[0].vertexShader == 0);
    CHECK(technique.passes[0].pixelShader == 1);

    const xenos::Shader& vertex = effect.shaders[0];
    CHECK(vertex.stage == xenos::ShaderStage::Vertex);
    CHECK(vertex.microcode == vertexProgram());
    CHECK(vertex.constants.target == std::string("vs_3_0"));
    CHECK(vertex.constants.constants.size() == std::size_t{2});
    const xenos::ConstantInfo* wvp = vertex.constants.find("WorldViewProj");
    CHECK(wvp != nullptr);
    if (wvp != nullptr) {
        CHECK(wvp->set == xenos::RegisterSet::Float4);
        CHECK(wvp->registerIndex == 4);
        CHECK(wvp->columns == 4);
    }
    CHECK(vertex.literals.size() == std::size_t{1});
    if (!vertex.literals.empty()) {
        CHECK(vertex.literals[0].reg == 255U);
        CHECK_THAT(vertex.literals[0].value[3], WithinAbs(3.0, 1e-6));
    }
    CHECK(vertex.vertexElements.size() == std::size_t{1});
    if (!vertex.vertexElements.empty()) {
        CHECK(vertex.vertexElements[0].fetchSlot == 1U);
        CHECK(vertex.vertexElements[0].usage == 0U);
    }
    CHECK(vertex.interpolators.size() == std::size_t{1});
    if (!vertex.interpolators.empty()) {
        CHECK(vertex.interpolators[0].usage == 5U);
        CHECK(vertex.interpolators[0].reg == 0U);
        CHECK(vertex.interpolators[0].mask == 0xFU);
    }

    const xenos::Shader& pixel = effect.shaders[1];
    CHECK(pixel.stage == xenos::ShaderStage::Pixel);
    CHECK(pixel.literals.size() == std::size_t{1});
    if (!pixel.literals.empty()) {
        CHECK(pixel.literals[0].reg == 255U);
    }
    const xenos::ConstantInfo* sampler = pixel.constants.atRegister(xenos::RegisterSet::Sampler, 3);
    CHECK((sampler != nullptr && sampler->name == "DiffuseSampler"));

    const xenos::VertexDeclaration& declaration = effect.declarations[0];
    CHECK(declaration.elements.size() == std::size_t{2});
    CHECK(declaration.stride(0) == 16U);
    CHECK(declaration.elements[0].format() == xenos::VertexFormat::F32_32_32_Float);
    CHECK(declaration.elements[1].name == std::string("uv"));
    CHECK(effect.declarationFor(vertex) == &declaration);

    const xenos::ShaderProgramSource program = xenos::buildProgram(effect, "Main");
    CHECK(program.unsupported.empty());
    CHECK(contains(program.vertexGlsl, "xe_vindex * 4u + 0u;"));
    CHECK(contains(program.vertexGlsl, "xe_var0 = xe_o[0];"));
    CHECK(contains(program.fragmentGlsl, "r[0] = xe_var0;"));
    const auto tint = program.findConstant("Tint");
    CHECK(tint.size() == std::size_t{1});
    if (!tint.empty()) {
        CHECK(tint[0]->stage == xenos::ShaderStage::Pixel);
        CHECK(tint[0]->index == 2U);
    }
    const auto fog = program.findConstant("UseFog");
    CHECK((!fog.empty() && fog[0]->index == 133U));
    const xenos::SamplerBinding* diffuse = program.findSampler("DiffuseSampler");
    CHECK((diffuse != nullptr && diffuse->unit == 3));
    CHECK(program.vertexInput.elements.size() == std::size_t{2});
    CHECK(program.vertexInput.strides.size() == std::size_t{1});
    CHECK(program.colorOutputs == 1U);
    checkCompiles(program.vertexGlsl, xenos::ShaderStage::Vertex);
    checkCompiles(program.fragmentGlsl, xenos::ShaderStage::Pixel);

    Bytes truncated(data.begin(), data.begin() + 0x100);
    CHECK_THROWS_AS(xenos::parseEffect(truncated, "truncated.fxobj"), std::exception);
}

TEST_CASE("packsSystemConstants", "[xenos]")
{
    xenos::SystemConstants system;
    system.clipScale[2] = -1.0F;
    system.clipOffset[2] = 1.0F;
    system.streamBase[5] = 77;
    const auto bytes = system.pack();
    CHECK(bytes.size() == std::size_t{144});
    float z = 0.0F;
    std::memcpy(&z, bytes.data() + 8, 4);
    CHECK_THAT(z, WithinAbs(-1.0, 0.0));
    std::uint32_t base = 0;
    std::memcpy(&base, bytes.data() + 80 + 5 * 4, 4);
    CHECK(base == 77U);
}

/// Every effect parsed, every shader and pass translated and compiled.
void checkAllTranslate(const xenos::CheckReport& report)
{
    CHECK(report.effectParseFailures == std::size_t{0});
    CHECK(report.decodeFailures == std::size_t{0});
    CHECK(report.translated == report.shaders);
    CHECK(report.compiled == report.shaders);
    CHECK(report.shadersWithUnsupported == std::size_t{0});
    CHECK(report.passesCompiled == report.passes);
}

// A fast stand-in for translatesEveryGameShader: a car material with conditional and
// predicated exec and jumps, a two-pass blended track material, and a render-state-only pass.
TEST_CASE("translatesRepresentativeGameShaders", "[xenos]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const std::vector<std::string> paths{"media/shaders/cars/rim_carbon_fiber3.fxobj",
        "media/tracks/colorado/bin/shaders/track/add_diff_opac_rgba.fxobj",
        "media/shaders/v2/effects/particle_smoke.fxobj"};
    const xenos::CheckReport report = xenos::checkEffects(vfs, paths);
    std::fprintf(stderr, "%s", xenos::formatReport(report).c_str());
    CHECK(report.effects == paths.size());
    CHECK(report.shaders == std::size_t{8});
    checkAllTranslate(report);
    CHECK(report.stateOnlyPasses > 0);
    for (const char* opcode : {"cexec_pc", "pexec", "cjmp"}) {
        CHECK(report.controlFlow.contains(opcode));
    }
}

TEST_CASE("translatesEveryGameShader", "[xenos][slow][.]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const std::vector<std::string> paths = xenos::listEffects(vfs);
    CHECK(paths.size() > 300);
    const xenos::CheckReport report = xenos::checkEffects(vfs, paths);
    std::fprintf(stderr, "%s", xenos::formatReport(report).c_str());
    CHECK(report.shaders > 4000);
    checkAllTranslate(report);
}

} // namespace
