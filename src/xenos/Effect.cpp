#include "Effect.h"

#include "base/Error.h"

#include <algorithm>
#include <bit>
#include <format>

namespace fh::xenos {

namespace {

constexpr std::uint32_t kEffectVersion = 0x101;
constexpr std::uint32_t kBlockMagic = 0xA3D70141;
constexpr std::size_t kBlockHeaderBytes = 12;
constexpr std::size_t kFileHeaderBytes = 0x1C;
constexpr std::size_t kDeclarationCountOffset = 0x10;
constexpr std::size_t kTechniqueArrayOffset = 0x200;
constexpr std::size_t kTechniqueCountOffset = 0x20C;
constexpr std::size_t kTechniqueHeaderBytes = 16;
constexpr std::size_t kStateRecordShaders = 0x44;
constexpr std::uint32_t kStateRecordMarker = 0xFFFFFFFF;
constexpr std::size_t kShaderObjectHeader = 8;
constexpr std::size_t kShaderHeaderBytes = 36;
constexpr std::size_t kDeclarationHeaderBytes = 0x34;
constexpr std::size_t kDeclarationCountField = 0x18;
constexpr std::uint32_t kMaxCount = 4096;
constexpr std::uint32_t kConstantTableHeaderBytes = 28;
constexpr std::size_t kConstantRecordBytes = 20;
constexpr std::size_t kTypeInfoBytes = 16;
constexpr std::uint32_t kLiteralEntryBytes = 8;
constexpr std::uint32_t kLiteralTailBytes = 12;
constexpr std::uint32_t kPixelConstantBase = 256;
constexpr std::size_t kVertexInfoFixedWords = 10;
constexpr std::size_t kPixelInfoFixedWords = 8;
constexpr std::uint32_t kParameterGenBit = 1U << 18;

/// The NUL-terminated text at `offset` of `data`; DataError past the end.
std::string stringAt(ByteView data, std::size_t offset, const std::string& source)
{
    if (offset >= data.size()) {
        throw DataError("{}: string offset 0x{:x} is past the end (0x{:x})", source, offset, data.size());
    }
    const auto* first = reinterpret_cast<const char*>(data.data() + offset);
    const auto* end = reinterpret_cast<const char*>(data.data() + data.size());
    return std::string(first, std::find(first, end, '\0'));
}

std::uint32_t u32At(ByteView data, std::size_t offset, const std::string& source)
{
    if (offset + 4 > data.size()) {
        throw DataError("{}: offset 0x{:x} is past the end (0x{:x})", source, offset, data.size());
    }
    return loadBe32(data.data() + offset);
}

} // namespace

std::string_view usageName(std::uint32_t usage)
{
    switch (static_cast<DeclUsage>(usage)) {
    case DeclUsage::Position:
        return "POSITION";
    case DeclUsage::BlendWeight:
        return "BLENDWEIGHT";
    case DeclUsage::BlendIndices:
        return "BLENDINDICES";
    case DeclUsage::Normal:
        return "NORMAL";
    case DeclUsage::PointSize:
        return "PSIZE";
    case DeclUsage::Texcoord:
        return "TEXCOORD";
    case DeclUsage::Tangent:
        return "TANGENT";
    case DeclUsage::Binormal:
        return "BINORMAL";
    case DeclUsage::TessFactor:
        return "TESSFACTOR";
    case DeclUsage::PositionT:
        return "POSITIONT";
    case DeclUsage::Color:
        return "COLOR";
    case DeclUsage::Fog:
        return "FOG";
    case DeclUsage::Depth:
        return "DEPTH";
    case DeclUsage::Sample:
        return "SAMPLE";
    }
    return "USAGE?";
}

std::string_view registerSetName(RegisterSet set)
{
    switch (set) {
    case RegisterSet::Bool:
        return "bool";
    case RegisterSet::Int4:
        return "int4";
    case RegisterSet::Float4:
        return "float4";
    case RegisterSet::Sampler:
        return "sampler";
    }
    return "?";
}

std::string_view ConstantInfo::typeName() const
{
    switch (parameterType) {
    case 0:
        return "void";
    case 1:
        return "bool";
    case 2:
        return "int";
    case 3:
        return "float";
    case 4:
        return "string";
    case 5:
        return "texture";
    case 10:
        return "sampler";
    case 11:
        return "sampler1D";
    case 12:
        return "sampler2D";
    case 13:
        return "sampler3D";
    case 14:
        return "samplerCUBE";
    default:
        return "type?";
    }
}

const ConstantInfo* ConstantTable::find(std::string_view name) const
{
    for (const ConstantInfo& c : constants) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

const ConstantInfo* ConstantTable::atRegister(RegisterSet set, std::uint32_t index) const
{
    for (const ConstantInfo& c : constants) {
        if (c.set == set && index >= c.registerIndex && index < std::uint32_t{c.registerIndex} + c.registerCount) {
            return &c;
        }
    }
    return nullptr;
}

ConstantTable parseConstantTable(ByteView table, const std::string& source)
{
    BeReader r(table, source + " constant table");
    ConstantTable result;
    r.expect(kConstantTableHeaderBytes, "constant table header size");
    const std::uint32_t creator = r.u32();
    result.version = r.u32();
    const std::uint32_t count = r.u32();
    const std::uint32_t records = r.u32();
    r.u32(); // flags
    const std::uint32_t target = r.u32();
    if (count > kMaxCount) {
        r.fail(std::format("{} constants", count));
    }
    result.creator = stringAt(table, creator, source);
    result.target = stringAt(table, target, source);
    result.constants.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        r.seek(records + static_cast<std::size_t>(i) * kConstantRecordBytes);
        ConstantInfo c;
        const std::uint32_t name = r.u32();
        const std::uint16_t set = r.u16();
        if (set > static_cast<std::uint16_t>(RegisterSet::Sampler)) {
            r.fail(std::format("constant {} has register set {}", i, set));
        }
        c.set = static_cast<RegisterSet>(set);
        c.registerIndex = r.u16();
        c.registerCount = r.u16();
        r.u16(); // reserved
        const std::uint32_t typeInfo = r.u32();
        const std::uint32_t defaultValue = r.u32();
        c.name = stringAt(table, name, source);
        if (typeInfo != 0) {
            BeReader t(table, source + " constant type");
            t.seek(typeInfo);
            t.need(kTypeInfoBytes);
            c.parameterClass = t.u16();
            c.parameterType = t.u16();
            c.rows = t.u16();
            c.columns = t.u16();
            c.elements = t.u16();
            c.structMembers = t.u16();
        }
        if (defaultValue != 0 && c.set == RegisterSet::Float4) {
            BeReader d(table, source + " constant default");
            d.seek(defaultValue);
            const std::size_t floats = static_cast<std::size_t>(c.registerCount) * 4;
            if (d.remaining() >= floats * 4) {
                c.defaultValue.reserve(floats);
                for (std::size_t k = 0; k < floats; ++k) {
                    c.defaultValue.push_back(d.f32());
                }
            }
        }
        result.constants.push_back(std::move(c));
    }
    return result;
}

std::optional<std::uint32_t> Shader::parameterRegister() const
{
    if (stage != ShaderStage::Pixel || (programControl & kParameterGenBit) == 0) {
        return std::nullopt;
    }
    return (contextMisc >> 8) & 0xFF;
}

Shader parseShader(ByteView blob, const std::string& source)
{
    BeReader r(blob, source);
    Shader shader;
    const std::uint32_t magic = r.u32();
    if (magic == kVertexShaderMagic) {
        shader.stage = ShaderStage::Vertex;
    } else if (magic == kPixelShaderMagic) {
        shader.stage = ShaderStage::Pixel;
    } else {
        r.fail(std::format("not a shader blob (magic 0x{:08x})", magic));
    }
    const std::uint32_t virtualSize = r.u32();
    const std::uint32_t physicalSize = r.u32();
    const std::uint32_t debugOffset = r.u32();
    const std::uint32_t constantOffset = r.u32();
    const std::uint32_t literalOffset = r.u32();
    const std::uint32_t infoOffset = r.u32();
    if (std::uint64_t{virtualSize} + physicalSize > blob.size() || infoOffset >= virtualSize
        || infoOffset < kShaderHeaderBytes) {
        r.fail(std::format("sizes 0x{:x} + 0x{:x} or info offset 0x{:x} do not fit 0x{:x} bytes", virtualSize,
            physicalSize, infoOffset, blob.size()));
    }
    const ByteView physical = blob.subspan(virtualSize, physicalSize);

    if (debugOffset != 0) {
        r.seek(debugOffset);
        r.u32();
        const std::uint32_t length = r.u32();
        shader.debugPath = r.fixedString(length);
    }

    if (constantOffset != 0) {
        r.seek(constantOffset);
        const std::uint32_t size = r.u32();
        shader.constants = parseConstantTable(r.bytes(size), source);
    }

    // Info first: the literal table is only meaningful with the microcode
    // offset known.
    r.seek(infoOffset);
    const std::size_t infoWords = (virtualSize - infoOffset) / 4;
    std::vector<std::uint32_t> info;
    info.reserve(infoWords);
    for (std::size_t i = 0; i < infoWords; ++i) {
        info.push_back(r.u32());
    }
    const std::size_t fixed = shader.stage == ShaderStage::Vertex ? kVertexInfoFixedWords : kPixelInfoFixedWords;
    if (info.size() < fixed) {
        r.fail(std::format("shader info has {} words", info.size()));
    }
    const std::uint32_t microcodeOffset = info[0];
    const std::uint32_t microcodeSize = info[1];
    shader.programControl = info[2];
    shader.contextMisc = info[3];
    shader.interfaceHash = info[5];
    if (std::uint64_t{microcodeOffset} + microcodeSize > physical.size() || microcodeSize % 4 != 0) {
        r.fail(std::format("microcode 0x{:x}+0x{:x} outside the physical part (0x{:x})", microcodeOffset,
            microcodeSize, physical.size()));
    }
    shader.microcode.reserve(microcodeSize / 4);
    for (std::uint32_t i = 0; i < microcodeSize; i += 4) {
        shader.microcode.push_back(loadBe32(physical.data() + microcodeOffset + i));
    }

    std::size_t next = fixed;
    std::size_t patchCount = 0;
    if (shader.stage == ShaderStage::Vertex) {
        shader.unknownInfo = {info[4], info[6], info[9]};
        const std::size_t elementCount = info[7];
        patchCount = info[8];
        if (elementCount + patchCount > info.size() - fixed) {
            r.fail(std::format("{} vertex elements and {} patches in {} info words", elementCount, patchCount,
                info.size()));
        }
        for (std::size_t i = 0; i < elementCount; ++i) {
            const std::uint32_t word = info[next++];
            shader.vertexElements.push_back({word, word & 0xFFF, (word >> 12) & 0xF, (word >> 16) & 0xF});
        }
    } else {
        shader.unknownInfo = {info[4], info[6], info[7]};
    }
    const std::size_t interpolatorEnd = info.size() - patchCount;
    while (next < interpolatorEnd) {
        const std::uint32_t word = info[next++];
        shader.interpolators.push_back({word, (word >> 4) & 0xF, word & 0xF, (word >> 8) & 0xF, (word >> 12) & 0xF});
    }
    while (next < info.size()) {
        shader.exportPatches.push_back(info[next++]);
    }

    if (literalOffset != 0) {
        r.seek(literalOffset + 16);
        const std::uint32_t size = r.u32();
        if (size < kLiteralTailBytes || (size - kLiteralTailBytes) % kLiteralEntryBytes != 0) {
            r.fail(std::format("literal table of {} bytes", size));
        }
        const std::uint32_t entries = (size - kLiteralTailBytes) / kLiteralEntryBytes;
        const std::uint32_t base = shader.stage == ShaderStage::Pixel ? kPixelConstantBase : 0;
        for (std::uint32_t e = 0; e < entries; ++e) {
            const std::uint16_t reg = r.u16();
            const std::uint16_t floats = r.u16();
            const std::uint32_t offset = r.u32();
            if (reg < base || floats % 4 != 0 || std::uint64_t{offset} + std::uint64_t{floats} * 4 > physical.size()
                || reg - base + floats / 4 > 256) {
                r.fail(std::format("literal entry {} (register {}, {} floats at 0x{:x})", e, reg, floats, offset));
            }
            for (std::uint32_t k = 0; k < floats / 4U; ++k) {
                LiteralConstant literal;
                literal.reg = reg - base + k;
                for (std::uint32_t c = 0; c < 4; ++c) {
                    literal.value[c] = loadBeF32(physical.data() + offset + (k * 4 + c) * 4);
                }
                shader.literals.push_back(literal);
            }
        }
    }
    return shader;
}

std::uint32_t VertexDeclaration::stride(std::uint16_t stream) const
{
    std::uint32_t end = 0;
    for (const VertexDeclElement& e : elements) {
        if (e.stream == stream) {
            end = std::max(end, std::uint32_t{e.offset} + e.byteSize());
        }
    }
    return (end + 3) / 4 * 4;
}

const VertexDeclElement* VertexDeclaration::find(std::uint32_t usage, std::uint32_t usageIndex) const
{
    for (const VertexDeclElement& e : elements) {
        if (e.usage == usage && e.usageIndex == usageIndex) {
            return &e;
        }
    }
    return nullptr;
}

const Technique* Effect::findTechnique(std::string_view name) const
{
    for (const Technique& t : techniques) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

const VertexDeclaration* Effect::declarationFor(const Shader& shader) const
{
    for (const VertexDeclaration& declaration : declarations) {
        const bool covers = std::all_of(shader.vertexElements.begin(), shader.vertexElements.end(),
            [&](const VertexElementUse& use) { return declaration.find(use.usage, use.usageIndex) != nullptr; });
        if (covers && !declaration.elements.empty()) {
            return &declaration;
        }
    }
    return nullptr;
}

std::optional<VertexDeclaration> Effect::inputDeclaration(const Shader& shader) const
{
    if (const VertexDeclaration* covering = declarationFor(shader)) {
        return *covering;
    }
    VertexDeclaration built;
    std::uint32_t offset = 0;
    for (const VertexElementUse& use : shader.vertexElements) {
        const VertexDeclElement* model = nullptr;
        for (const VertexDeclaration& declaration : declarations) {
            model = declaration.find(use.usage, use.usageIndex);
            if (model != nullptr) {
                break;
            }
        }
        if (model == nullptr) {
            return std::nullopt;
        }
        VertexDeclElement element = *model;
        element.stream = 0;
        element.offset = static_cast<std::uint16_t>(offset);
        offset += (element.byteSize() + 3) / 4 * 4;
        built.elements.push_back(std::move(element));
    }
    return built;
}

Effect parseEffect(ByteView data, const std::string& source)
{
    BeReader r(data, source);
    r.expect(kEffectVersion, "effect version");
    if (data.size() < kFileHeaderBytes) {
        r.fail("truncated header");
    }
    const std::uint32_t declarationCount = u32At(data, kDeclarationCountOffset, source);

    std::size_t magic = std::string_view::npos;
    for (std::size_t at = kFileHeaderBytes; at + kBlockHeaderBytes <= data.size(); at += 4) {
        if (loadBe32(data.data() + at) == kBlockMagic) {
            magic = at;
            break;
        }
    }
    if (magic == std::string_view::npos) {
        throw DataError("{}: no effect block (0x{:08x})", source, kBlockMagic);
    }
    const std::size_t base = magic + kBlockHeaderBytes;
    const std::uint32_t blockSize = u32At(data, magic + 4, source);
    if (base + blockSize > data.size()) {
        throw DataError("{}: effect block of 0x{:x} bytes at 0x{:x} is past the end", source, blockSize, base);
    }
    Effect effect;

    // Shaders are found by their magic: the section tables that list them are
    // only partly understood, and a blob is self-describing enough (size
    // prefix and consistent sizes) that a stray match is rejected.
    std::vector<std::size_t> blobOffsets;
    for (std::size_t at = base + kShaderObjectHeader; at + kShaderHeaderBytes <= base + blockSize; at += 4) {
        const std::uint32_t magicWord = loadBe32(data.data() + at);
        if (magicWord != kVertexShaderMagic && magicWord != kPixelShaderMagic) {
            continue;
        }
        const std::uint32_t size = loadBe32(data.data() + at - 4);
        const std::uint32_t virtualSize = loadBe32(data.data() + at + 4);
        const std::uint32_t physicalSize = loadBe32(data.data() + at + 8);
        if (std::uint64_t{virtualSize} + physicalSize != size || at + size > data.size()) {
            continue;
        }
        effect.shaders.push_back(parseShader(data.subspan(at, size), std::format("{} shader at 0x{:x}", source, at)));
        effect.shaders.back().fileOffset = at;
        blobOffsets.push_back(at);
        at += (size & ~std::size_t{3}) - 4;
    }

    auto shaderIndex = [&](std::uint32_t object) -> int {
        const std::size_t objectAt = base + object;
        const std::uint32_t size = u32At(data, objectAt + 4, source);
        if (size == 0) {
            return -1;
        }
        const auto it = std::find(blobOffsets.begin(), blobOffsets.end(), objectAt + kShaderObjectHeader);
        if (it == blobOffsets.end()) {
            throw DataError("{}: pass refers to a shader at 0x{:x} that is not one", source, objectAt);
        }
        return static_cast<int>(it - blobOffsets.begin());
    };

    const std::uint32_t techniqueArray = u32At(data, base + kTechniqueArrayOffset, source);
    const std::uint32_t techniqueCount = u32At(data, base + kTechniqueCountOffset, source);
    if (techniqueCount > kMaxCount) {
        throw DataError("{}: {} techniques", source, techniqueCount);
    }
    for (std::uint32_t t = 0; t < techniqueCount; ++t) {
        const std::size_t at = base + u32At(data, base + techniqueArray + std::size_t{t} * 4, source);
        Technique technique;
        technique.name = stringAt(data, base + u32At(data, at, source), source);
        const std::uint32_t passCount = u32At(data, at + 4, source);
        if (passCount > kMaxCount || at + kTechniqueHeaderBytes > data.size()) {
            throw DataError("{}: technique {} has {} passes", source, t, passCount);
        }
        for (std::uint32_t p = 0; p < passCount; ++p) {
            const std::size_t passAt = base + u32At(data, at + kTechniqueHeaderBytes + std::size_t{p} * 4, source);
            Pass pass;
            pass.name = stringAt(data, base + u32At(data, passAt, source), source);
            const std::size_t record = base + u32At(data, passAt + 8, source);
            if (u32At(data, record + kStateRecordShaders, source) != kStateRecordMarker) {
                throw DataError("{}: pass {}/{} state record at 0x{:x} lacks its marker", source, technique.name,
                    pass.name, record);
            }
            pass.vertexShader = shaderIndex(u32At(data, record + kStateRecordShaders + 4, source));
            pass.pixelShader = shaderIndex(u32At(data, record + kStateRecordShaders + 8, source));
            technique.passes.push_back(std::move(pass));
        }
        effect.techniques.push_back(std::move(technique));
    }

    if (declarationCount > kMaxCount) {
        throw DataError("{}: {} vertex declarations", source, declarationCount);
    }
    BeReader tail(data, source + " declarations");
    tail.seek(base + blockSize);
    for (std::uint32_t d = 0; d < declarationCount; ++d) {
        const std::size_t start = tail.pos();
        tail.need(kDeclarationHeaderBytes);
        tail.seek(start + kDeclarationCountField);
        const std::uint32_t count = tail.u32();
        if (count > kMaxCount) {
            tail.fail(std::format("declaration {} has {} elements", d, count));
        }
        tail.seek(start + kDeclarationHeaderBytes);
        VertexDeclaration declaration;
        for (std::uint32_t e = 0; e < count; ++e) {
            VertexDeclElement element;
            element.stream = tail.u16();
            element.offset = tail.u16();
            element.type = tail.u32();
            element.method = tail.u8();
            element.usage = tail.u8();
            element.usageIndex = tail.u8();
            tail.u8();
            declaration.elements.push_back(element);
        }
        tail.u32();
        effect.declarations.push_back(std::move(declaration));
    }
    for (VertexDeclaration& declaration : effect.declarations) {
        for (VertexDeclElement& element : declaration.elements) {
            element.name = tail.atEnd() ? std::string() : tail.cString();
        }
    }
    return effect;
}

} // namespace fh::xenos
