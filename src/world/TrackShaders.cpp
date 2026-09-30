#include "TrackShaders.h"

#include "Archive.h"

#include "base/Error.h"

#include <lzx/zip.h>

#include <bit>
#include <cstring>
#include <utility>

namespace fh::world {

namespace {

constexpr std::string_view kDefaultTechnique = "Default";
constexpr std::uint32_t kBlockMagic = 0xA3D70141;
constexpr std::size_t kBlockHeaderBytes = 12;
constexpr std::size_t kTechniqueArrayOffset = 0x200;
constexpr std::size_t kTechniqueCountOffset = 0x20C;
constexpr std::size_t kTechniquePasses = 16;
constexpr std::size_t kPassStateList = 12;
constexpr std::size_t kStateCount = 24;
constexpr std::size_t kStatePairs = 28;
constexpr std::uint32_t kMaxStates = 256;

enum StateId : std::uint32_t {
    kZWriteEnable = 48,
    kCullMode = 56,
    kAlphaBlendEnable = 60,
    kSourceBlend = 72,
    kDestinationBlend = 76,
    kSlopeScaleDepthBias = 204,
    kDepthBias = 208,
    kAlphaToMaskEnable = 336,
    kAlphaToMaskOffsets = 340,
};

std::uint16_t elementBytes(std::uint32_t type)
{
    switch (type) {
    case decltype360::kFloat3:
    case decltype360::kDec3N:
        return 12;
    case decltype360::kUShort2N:
        return 8;
    case decltype360::kColor:
        return 4;
    default:
        return 0;
    }
}

std::uint32_t renderType(std::uint32_t type)
{
    switch (type) {
    case decltype360::kFloat3:
    case decltype360::kDec3N:
        return decltype360::kFloat3Native;
    case decltype360::kUShort2N:
        return decltype360::kFloat2Native;
    default:
        return type;
    }
}

} // namespace

std::string TrackShaders::key(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    name = name.substr(0, name.find('.'));
    return toLower(name);
}

std::optional<xenos::VertexDeclaration> TrackShaders::renderDeclaration(const xenos::VertexDeclaration& file)
{
    xenos::VertexDeclaration out;
    std::uint16_t offset = 0;
    for (const xenos::VertexDeclElement& element : file.elements) {
        const std::uint16_t bytes = elementBytes(element.type);
        if (bytes == 0 || element.stream != 0) {
            return std::nullopt;
        }
        xenos::VertexDeclElement converted = element;
        converted.offset = offset;
        converted.type = renderType(element.type);
        out.elements.push_back(converted);
        offset = static_cast<std::uint16_t>(offset + bytes);
    }
    return out;
}

TrackShaders::RenderState TrackShaders::readRenderState(ByteView fxobj, std::string_view technique)
{
    BeReader r(fxobj, "effect render states");
    std::size_t base = 0;
    for (std::size_t at = 0; at + 4 <= fxobj.size(); at += 4) {
        if (loadBe32(fxobj.data() + at) == kBlockMagic) {
            base = at + kBlockHeaderBytes;
            break;
        }
    }
    if (base == 0) {
        throw DataError("effect has no data block");
    }
    const auto u32At = [&r](std::size_t offset) {
        r.seek(offset);
        return r.u32();
    };
    const std::uint32_t techniques = u32At(base + kTechniqueArrayOffset);
    const std::uint32_t count = u32At(base + kTechniqueCountOffset);
    for (std::uint32_t t = 0; t < count; ++t) {
        const std::size_t at = base + u32At(base + techniques + std::size_t{t} * 4);
        const std::size_t nameAt = base + u32At(at);
        if (nameAt >= fxobj.size()) {
            throw DataError("technique name past the end");
        }
        const auto* name = reinterpret_cast<const char*>(fxobj.data() + nameAt);
        if (std::string_view(name, strnlen(name, fxobj.size() - nameAt)) != technique) {
            continue;
        }
        const std::size_t pass = base + u32At(at + kTechniquePasses);
        const std::size_t list = base + u32At(pass + kPassStateList);
        const std::uint32_t states = u32At(list + kStateCount);
        if (states > kMaxStates) {
            throw DataError("implausible render state count {}", states);
        }
        RenderState state;
        for (std::uint32_t k = 0; k < states; ++k) {
            const std::uint32_t id = u32At(list + kStatePairs + std::size_t{k} * 8);
            const std::uint32_t value = r.u32();
            switch (id) {
            case kZWriteEnable:
                state.depthWrite = value != 0;
                break;
            case kCullMode:
                state.cull = value;
                state.cullGiven = true;
                break;
            case kAlphaBlendEnable:
                state.blend = value != 0;
                break;
            case kSourceBlend:
                state.sourceBlend = value;
                break;
            case kDestinationBlend:
                state.destinationBlend = value;
                break;
            case kSlopeScaleDepthBias:
                state.slopeScaleDepthBias = std::bit_cast<float>(value);
                break;
            case kDepthBias:
                state.depthBias = std::bit_cast<float>(value);
                break;
            case kAlphaToMaskEnable:
                state.alphaToMask = value != 0;
                break;
            case kAlphaToMaskOffsets:
                state.alphaToMaskOffsets = value;
                break;
            default:
                break;
            }
        }
        return state;
    }
    throw DataError("effect has no technique {}", technique);
}

TrackShaders TrackShaders::load(const lzx::ZipArchive& archive)
{
    const std::vector<std::size_t> files = distinctEntries(archive, ".fxobj");
    std::vector<std::optional<Entry>> parsed(files.size());
    parallelFor(files.size(), [&](std::size_t i) {
        const lzx::ZipEntry& zipEntry = archive.entries()[files[i]];
        try {
            Entry entry;
            entry.name = key(zipEntry.name);
            const Bytes data = readEntry(archive, zipEntry);
            entry.effect = xenos::parseEffect(data, zipEntry.name);
            entry.state = readRenderState(data, kDefaultTechnique);
            for (const auto& [technique, target] : {std::pair{"DepthOnly", &entry.depthOnlyState},
                     std::pair{"ShadowDepthOnly", &entry.shadowDepthOnlyState}}) {
                if (entry.effect.findTechnique(technique) != nullptr) {
                    *target = readRenderState(data, technique);
                }
            }
            const xenos::Technique* technique = entry.effect.findTechnique(kDefaultTechnique);
            if (technique == nullptr || technique->passes.empty() || technique->passes.front().vertexShader < 0) {
                return;
            }
            entry.technique = static_cast<std::size_t>(technique - entry.effect.techniques.data());
            const xenos::Shader& vertex
                = entry.effect.shaders[static_cast<std::size_t>(technique->passes.front().vertexShader)];
            std::optional<xenos::VertexDeclaration> declaration = entry.effect.inputDeclaration(vertex);
            if (!declaration) {
                return;
            }
            std::optional<xenos::VertexDeclaration> render = renderDeclaration(*declaration);
            if (!render) {
                return;
            }
            entry.fileDeclaration = std::move(*declaration);
            entry.renderDeclaration = std::move(*render);
            parsed[i] = std::move(entry);
        } catch (const DataError&) {
            parsed[i].reset();
        }
    });
    TrackShaders shaders;
    for (std::optional<Entry>& entry : parsed) {
        if (!entry) {
            ++shaders.m_failed;
            continue;
        }
        const auto index = static_cast<std::uint32_t>(shaders.m_entries.size());
        if (shaders.m_byKey.try_emplace(entry->name, index).second) {
            shaders.m_entries.push_back(std::move(*entry));
        }
    }
    return shaders;
}

std::optional<std::uint32_t> TrackShaders::find(std::string_view path) const
{
    const auto it = m_byKey.find(key(path));
    return it == m_byKey.end() ? std::nullopt : std::optional<std::uint32_t>(it->second);
}

} // namespace fh::world
