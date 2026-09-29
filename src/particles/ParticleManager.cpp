#include "ParticleManager.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <algorithm>
#include <cctype>

namespace fh::particles {
namespace {

bool sameName(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

} // namespace

ParticleManager ParticleManager::load(const Vfs& vfs)
{
    const std::optional<std::string> list = vfs.readText(kDefaultTrackPath);
    if (!list) {
        throw DataError("{} is missing", kDefaultTrackPath);
    }
    const std::vector<SystemEntry> entries = parseSystemList(*list);

    std::vector<EffectDefinition> definitions;
    std::vector<std::string> skipped;
    for (const SystemEntry& entry : entries) {
        if (isCompositeName(entry.file)) {
            skipped.push_back(entry.file + " (composite)");
            continue;
        }
        const std::string path = effectPath(entry.file);
        const std::optional<std::string> text = vfs.readText(path);
        if (!text) {
            skipped.push_back(entry.file + " (missing)");
            continue;
        }
        try {
            std::string name = path.substr(kEffectsFolder.size() + 1);
            name.resize(name.size() - 4);
            definitions.push_back(parseEffect(*text, std::move(name)));
        } catch (const DataError& e) {
            skipped.emplace_back(e.what());
        }
    }
    ParticleManager manager(std::move(definitions), entries);
    manager.m_skipped.insert(manager.m_skipped.end(), skipped.begin(), skipped.end());
    return manager;
}

ParticleManager::ParticleManager(std::vector<EffectDefinition> definitions, const std::vector<SystemEntry>& entries)
{
    for (EffectDefinition& d : definitions) {
        m_definitions.push_back(std::move(d));
    }
    std::uint32_t seed = 1;
    for (const SystemEntry& entry : entries) {
        const auto it = std::ranges::find_if(
            m_definitions, [&entry](const EffectDefinition& d) { return sameName(d.name, entry.file); });
        if (it == m_definitions.end()) {
            continue;
        }
        m_systems.push_back({entry.systemName, std::make_unique<ParticleSystem>(*it, seed++)});
    }
}

ParticleSystem* ParticleManager::find(std::string_view systemName)
{
    for (System& s : m_systems) {
        if (sameName(s.name, systemName)) {
            return s.system.get();
        }
    }
    return nullptr;
}

const ParticleSystem* ParticleManager::find(std::string_view systemName) const
{
    for (const System& s : m_systems) {
        if (sameName(s.name, systemName)) {
            return s.system.get();
        }
    }
    return nullptr;
}

void ParticleManager::emit(ParticleSystem& system, const EmitRequest& request)
{
    system.emit(request, m_frameDt, m_crt);
}

void ParticleManager::update(float dt)
{
    m_frameDt = dt;
    if (dt <= 0.0F) {
        return;
    }
    for (System& s : m_systems) {
        if (s.system->size() > 0) {
            s.system->update(dt, m_rateScale);
        }
    }
}

void ParticleManager::clear()
{
    for (System& s : m_systems) {
        s.system->clear();
    }
}

} // namespace fh::particles
