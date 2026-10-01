#include "ParticleManager.h"

#include "ParticleEmitters.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace openhorizon::particles {
namespace {

bool sameName(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

/// 0x82007bf0: the factor 0x82dc5600 and 0x82dc2330 apply to a composite member's StartOffset and FinishOffset.
constexpr float kCompositeOffsetScale = 60.0F;
/// 0x82249254: the end 0x82dc5600 gives a composite member's emitter.
constexpr float kCompositeEnd = 900000.0F;

/// The file name an effects path ends in, without folder or extension.
std::string fileStem(const std::string& path)
{
    std::string name = path.substr(kEffectsFolder.size() + 1);
    name.resize(name.size() - 4);
    return name;
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
            definitions.push_back(parseEffect(*text, fileStem(path)));
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
    m_nextSeed = seed;
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
        if (s.system->size() > 0 || !s.system->emitters().empty()) {
            s.system->update(dt, m_rateScale);
            s.system->updateEmitters(dt, m_crt);
        }
    }
}

void ParticleManager::clear()
{
    for (System& s : m_systems) {
        s.system->clear();
    }
}

ParticleSystem* ParticleManager::acquire(const Vfs& vfs, std::string_view file)
{
    if (ParticleSystem* existing = find(file)) {
        return existing;
    }
    if (isCompositeName(file)) {
        return nullptr;
    }
    const std::string path = effectPath(file);
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        return nullptr;
    }
    try {
        m_definitions.push_back(parseEffect(*text, fileStem(path)));
    } catch (const DataError& e) {
        m_skipped.emplace_back(e.what());
        return nullptr;
    }
    m_systems.push_back({std::string(file), std::make_unique<ParticleSystem>(m_definitions.back(), m_nextSeed++)});
    return m_systems.back().system.get();
}

const CompositeSystems* ParticleManager::acquireComposite(const Vfs& vfs, std::string_view file)
{
    for (const CompositeSystems& c : m_composites) {
        if (sameName(c.definition.name, file)) {
            return &c;
        }
    }
    const std::optional<std::string> text = vfs.readText(effectPath(file));
    if (!text) {
        return nullptr;
    }
    CompositeSystems composite;
    try {
        composite.definition = parseComposite(*text, std::string(file));
    } catch (const DataError& e) {
        m_skipped.emplace_back(e.what());
        return nullptr;
    }
    for (const CompositeMember& member : composite.definition.members) {
        composite.members.push_back(acquire(vfs, member.fileName));
    }
    m_composites.push_back(std::move(composite));
    return &m_composites.back();
}

void ParticleManager::placeSimpleEmitters(const Vfs& vfs, const ParticleEmitterSet& set)
{
    for (const PlacedEmitter& placed : set.simpleEmitters()) {
        SimpleEmitterState state;
        state.gameplayId = placed.gameplayId;
        state.id = m_nextEmitterId++;
        state.position = placed.position;
        state.angles = placed.angles;
        if (placed.composite()) {
            state.composite = acquireComposite(vfs, placed.effect);
            // 0x82dc2298 and 0x82dc5600 look at the first member's system
            // unchecked; a composite without one could not run.
            if (state.composite != nullptr
                && (state.composite->members.empty() || state.composite->members.front() == nullptr)) {
                state.composite = nullptr;
            }
        } else {
            state.system = acquire(vfs, placed.effect);
        }
        if (state.system == nullptr && state.composite == nullptr) {
            m_skipped.push_back(placed.gameplayId + " (" + placed.effect + ")");
            continue;
        }
        if (m_simpleByGameplayId.contains(state.gameplayId)) {
            continue;
        }
        m_simpleByGameplayId.emplace(state.gameplayId, m_simpleEmitters.size());
        m_simpleEmitters.push_back(std::move(state));
    }
}

const SimpleEmitterState* ParticleManager::simpleEmitter(std::string_view gameplayId) const
{
    const auto it = m_simpleByGameplayId.find(gameplayId);
    return it == m_simpleByGameplayId.end() ? nullptr : &m_simpleEmitters[it->second];
}

bool ParticleManager::activateSimpleEmitter(std::string_view gameplayId)
{
    const auto it = m_simpleByGameplayId.find(gameplayId);
    if (it == m_simpleByGameplayId.end()) {
        return false;
    }
    SimpleEmitterState& emitter = m_simpleEmitters[it->second];
    emitter.wanted = true;
    start(emitter);
    return true;
}

bool ParticleManager::deactivateSimpleEmitter(std::string_view gameplayId)
{
    const auto it = m_simpleByGameplayId.find(gameplayId);
    if (it == m_simpleByGameplayId.end()) {
        return false;
    }
    SimpleEmitterState& emitter = m_simpleEmitters[it->second];
    emitter.wanted = false;
    if (emitter.running) {
        stop(emitter);
    }
    return true;
}

std::vector<const ParticleSystem*> ParticleManager::systemsOf(const SimpleEmitterState& emitter) const
{
    std::vector<const ParticleSystem*> systems;
    if (emitter.system != nullptr) {
        systems.push_back(emitter.system);
    } else if (emitter.composite != nullptr) {
        for (const ParticleSystem* member : emitter.composite->members) {
            if (member != nullptr) {
                systems.push_back(member);
            }
        }
    }
    return systems;
}

void ParticleManager::start(SimpleEmitterState& emitter)
{
    // 0x82dc5600.
    emitter.running = true;
    if (emitter.system != nullptr) {
        if (!emitter.system->hasEmitter(emitter.id)) {
            emitter.system->addEmitter(emitter.system->makeEmitter(emitter.id, emitter.position, emitter.angles));
        }
        return;
    }
    const CompositeSystems& composite = *emitter.composite;
    if (composite.members.front()->hasEmitter(emitter.id)) {
        return;
    }
    // PosOffset turned by the emitter's angles (0x82490c30); the VMX128 row-vector product is inferred.
    const glm::mat3 turn = rollPitchYaw(emitter.angles.x, emitter.angles.y, emitter.angles.z);
    for (std::size_t i = 0; i < composite.members.size(); ++i) {
        ParticleSystem* system = composite.members[i];
        if (system == nullptr) {
            continue;
        }
        const CompositeMember& member = composite.definition.members[i];
        const glm::vec3 position = emitter.position + transformRow(glm::vec3(member.positionOffset), turn);
        SystemEmitter e = system->makeEmitter(emitter.id, position, emitter.angles);
        e.start = member.startOffset * kCompositeOffsetScale;
        e.end = kCompositeEnd;
        system->addEmitter(e);
    }
}

void ParticleManager::stop(SimpleEmitterState& emitter)
{
    // 0x82dc2330.
    emitter.running = false;
    if (emitter.system != nullptr) {
        emitter.system->stopEmitters(emitter.id, std::nullopt);
        return;
    }
    const CompositeSystems& composite = *emitter.composite;
    for (std::size_t i = 0; i < composite.members.size(); ++i) {
        if (composite.members[i] != nullptr) {
            composite.members[i]->stopEmitters(
                emitter.id, composite.definition.members[i].finishOffset * kCompositeOffsetScale);
        }
    }
}

} // namespace openhorizon::particles
