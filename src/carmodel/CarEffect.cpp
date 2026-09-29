#include "CarEffect.h"

#include "base/Error.h"
#include "vfs/Vfs.h"
#include "xenos/Microcode.h"

#include <cstring>
#include <format>
#include <utility>

namespace fh::carmodel {

namespace {

std::mutex g_cacheMutex;
std::map<std::string, std::weak_ptr<const CarEffect>> g_cache;

/// The effect data block and the offsets of its technique table, as
/// xenos::parseEffect reads them.
constexpr std::uint32_t kBlockMagic = 0xA3D70141;
constexpr std::size_t kBlockHeaderBytes = 12;
constexpr std::size_t kTechniqueArrayOffset = 0x200;
constexpr std::size_t kTechniqueCountOffset = 0x20C;
constexpr std::size_t kTechniquePasses = 16;
/// A pass's state list and, in it, the count and the (state, value) pairs;
/// the layout world::TrackShaders::readRenderState reads for track effects,
/// checked here on the car effect's detail_glass passes (multiply, then
/// add) and their Cleanup pass (back to source alpha blending).
constexpr std::size_t kPassStateList = 12;
constexpr std::size_t kStateCount = 24;
constexpr std::size_t kStatePairs = 28;
constexpr std::uint32_t kMaxStates = 256;
constexpr std::uint32_t kAlphaBlendEnable = 60;
constexpr std::uint32_t kSourceBlend = 72;
constexpr std::uint32_t kDestinationBlend = 76;

/// The blend states of pass `pass` of technique `technique`, or nothing when
/// the pass sets none.
std::optional<CarPass::Blend> readBlend(ByteView data, std::string_view technique, std::size_t pass)
{
    BeReader r(data, "car effect render states");
    std::size_t base = 0;
    for (std::size_t at = 0; at + 4 <= data.size(); at += 4) {
        if (loadBe32(data.data() + at) == kBlockMagic) {
            base = at + kBlockHeaderBytes;
            break;
        }
    }
    if (base == 0) {
        return std::nullopt;
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
        if (nameAt >= data.size()) {
            return std::nullopt;
        }
        const auto* name = reinterpret_cast<const char*>(data.data() + nameAt);
        if (std::string_view(name, strnlen(name, data.size() - nameAt)) != technique) {
            continue;
        }
        const std::size_t passAt = base + u32At(at + kTechniquePasses + pass * 4);
        const std::size_t list = base + u32At(passAt + kPassStateList);
        const std::uint32_t states = u32At(list + kStateCount);
        if (states > kMaxStates) {
            return std::nullopt;
        }
        std::optional<CarPass::Blend> blend;
        for (std::uint32_t k = 0; k < states; ++k) {
            const std::uint32_t id = u32At(list + kStatePairs + std::size_t{k} * 8);
            const std::uint32_t value = r.u32();
            if (id == kAlphaBlendEnable || id == kSourceBlend || id == kDestinationBlend) {
                if (!blend) {
                    blend.emplace();
                }
                if (id == kAlphaBlendEnable) {
                    blend->enable = value != 0;
                } else if (id == kSourceBlend) {
                    blend->source = value;
                } else {
                    blend->destination = value;
                }
            }
        }
        return blend;
    }
    return std::nullopt;
}

/// Whether the pixel shader exports the square root of red, green and blue
/// to colour target 0.
bool writesSquareRoot(const xenos::Shader& pixel)
{
    const xenos::Program program = xenos::decodeProgram(xenos::ShaderStage::Pixel, pixel.microcode);
    std::uint32_t mask = 0;
    for (const xenos::CfInstruction& cf : program.cf) {
        if (!cf.isExec()) {
            continue;
        }
        for (std::uint32_t i = 0; i < cf.count; ++i) {
            if (cf.slotIsFetch(i)) {
                continue;
            }
            const std::array<std::uint32_t, 3> words = program.slot(cf.address + i);
            const xenos::AluInstruction alu = xenos::decodeAlu(words[0], words[1], words[2]);
            if (alu.exportData && alu.vectorDest == 0
                && alu.scalarOpcode == static_cast<std::uint32_t>(xenos::ScalarOpcode::Sqrt)) {
                mask |= alu.scalarResultMask();
            }
        }
    }
    return (mask & 7U) == 7U;
}

} // namespace

CarEffect::CarEffect(std::string path, Bytes data, xenos::Effect effect)
    : m_path(std::move(path))
    , m_data(std::move(data))
    , m_effect(std::move(effect))
{
}

std::shared_ptr<const CarEffect> CarEffect::load(const Vfs& vfs, const std::string& path)
{
    const std::string key = Vfs::normalize(path);
    const std::lock_guard lock(g_cacheMutex);
    if (const std::shared_ptr<const CarEffect> known = g_cache[key].lock()) {
        return known;
    }
    Bytes data = vfs.readOrThrow(key);
    xenos::Effect effect = xenos::parseEffect(data, key);
    auto loaded = std::make_shared<const CarEffect>(key, std::move(data), std::move(effect));
    g_cache[key] = loaded;
    return loaded;
}

bool CarEffect::hasTechnique(std::string_view technique) const
{
    return m_effect.findTechnique(technique) != nullptr;
}

std::vector<std::shared_ptr<const CarPass>> CarEffect::passes(std::string_view technique) const
{
    const std::lock_guard lock(m_mutex);
    if (const auto known = m_translated.find(technique); known != m_translated.end()) {
        return known->second.passes;
    }
    Entry entry;
    const xenos::Technique* found = m_effect.findTechnique(technique);
    if (found == nullptr) {
        entry.error = std::format("{} has no technique {}", m_path, technique);
    } else {
        try {
            for (std::size_t p = 0; p < found->passes.size(); ++p) {
                const xenos::Pass& pass = found->passes[p];
                if (pass.vertexShader < 0) {
                    continue;
                }
                auto translated = std::make_shared<CarPass>();
                translated->program = xenos::buildProgram(m_effect, pass);
                if (pass.pixelShader >= 0) {
                    translated->squareRootOutput
                        = writesSquareRoot(m_effect.shaders[static_cast<std::size_t>(pass.pixelShader)]);
                }
                translated->blend = readBlend(m_data, technique, p);
                entry.passes.push_back(std::move(translated));
            }
        } catch (const DataError& e) {
            entry.passes.clear();
            entry.error = std::format("{} technique {}: {}", m_path, technique, e.what());
        }
    }
    auto passes = entry.passes;
    m_translated.emplace(std::string(technique), std::move(entry));
    return passes;
}

std::string CarEffect::error(std::string_view technique) const
{
    const std::lock_guard lock(m_mutex);
    const auto known = m_translated.find(technique);
    return known == m_translated.end() ? std::string() : known->second.error;
}

} // namespace fh::carmodel
