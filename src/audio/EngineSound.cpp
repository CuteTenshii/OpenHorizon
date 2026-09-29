#include "EngineSound.h"

#include "AudioDevice.h"
#include "AudioLibrary.h"
#include "EventPlayer.h"
#include "TyreSound.h"

#include "base/Log.h"
#include "vfs/Vfs.h"

#include <algorithm>
#include <cmath>

namespace fh::audio {

namespace {

constexpr const char* kTransmissionsProject = "media/audio/cars/transmissions/transmissions.fev";
constexpr const char* kShiftEvent = "ShiftComplete/L0";
/// Fuel-cut rhythm of the rev limiter: one 35 ms cut every 90 ms.
constexpr float kLimiterPeriod = 0.09F;
constexpr float kLimiterCut = 0.035F;

std::string_view groupName(EmissionGroup group)
{
    switch (group) {
    case EmissionGroup::Intake:
        return "intake";
    case EmissionGroup::Ambient:
        return "ambient";
    case EmissionGroup::Exhaust:
        return "exhaust";
    }
    return "";
}

} // namespace

EngineSound::EngineSound(AudioDevice& device, const Vfs& vfs, const std::string& carMedia, EngineSoundOptions options)
    : EngineSound(device, std::make_shared<AudioLibrary>(vfs), carMedia, std::move(options))
{
}

EngineSound::EngineSound(AudioDevice& device, std::shared_ptr<AudioLibrary> library, const std::string& carMedia,
    EngineSoundOptions options)
    : m_mixer(device.mixer())
    , m_library(std::move(library))
    , m_options(std::move(options))
    , m_random(m_options.seed)
{
    m_tuning = CarAudioTuning::load(m_library->vfs(), carMedia, m_options.intakeUpgrade, m_options.exhaustUpgrade);
    try {
        build();
        startEvents();
    } catch (...) {
        for (const Emitter& emitter : m_emitters) {
            m_mixer.destroyBus(emitter.bus);
        }
        throw;
    }
}

EngineSound::~EngineSound()
{
    m_tyres.reset();
    m_shift.reset();
    for (const Emitter& emitter : m_emitters) {
        m_mixer.destroyBus(emitter.bus);
    }
}

std::vector<float> EngineSound::loopWeights(std::span<const EngineLoop> loops, float rpm)
{
    const std::size_t n = loops.size();
    std::vector<float> weights(n, 0.0F);
    if (n == 0) {
        return weights;
    }
    float sumSquares = 0.0F;
    for (std::size_t i = 0; i < n; ++i) {
        const EngineLoop& loop = loops[i];
        if (rpm < loop.rpmMin || rpm > loop.rpmMax) {
            continue;
        }
        float weight = 1.0F;
        if (i > 0 && loops[i - 1].rpmMax > loop.rpmMin && rpm < loops[i - 1].rpmMax) {
            weight *= (rpm - loop.rpmMin) / (loops[i - 1].rpmMax - loop.rpmMin);
        }
        if (i + 1 < n && loop.rpmMax > loops[i + 1].rpmMin && rpm > loops[i + 1].rpmMin) {
            weight *= (loop.rpmMax - rpm) / (loop.rpmMax - loops[i + 1].rpmMin);
        }
        weights[i] = std::max(weight, 0.0F);
        sumSquares += weights[i] * weights[i];
    }
    if (sumSquares <= 1e-12F) {
        std::size_t nearest = 0;
        float best = std::abs(rpm - loops[0].rpmSample);
        for (std::size_t i = 1; i < n; ++i) {
            const float distance = std::abs(rpm - loops[i].rpmSample);
            if (distance < best) {
                best = distance;
                nearest = i;
            }
        }
        std::fill(weights.begin(), weights.end(), 0.0F);
        weights[nearest] = 1.0F;
        return weights;
    }
    const float scale = 1.0F / std::sqrt(sumSquares);
    for (float& weight : weights) {
        weight *= scale;
    }
    return weights;
}

float EngineSound::emitterVolume(const std::string& name) const
{
    const CarModelTuning::View* view = m_tuning.model.view(m_options.view);
    if (!view) {
        return 1.0F;
    }
    const auto it = view->emitterVolumes.find(name);
    return it == view->emitterVolumes.end() ? 1.0F : it->second;
}

float EngineSound::otherVolume(const std::string& name) const
{
    const CarModelTuning::View* view = m_tuning.model.view(m_options.view);
    if (!view) {
        return 1.0F;
    }
    const auto it = view->otherVolumes.find(name);
    return it == view->otherVolumes.end() ? 1.0F : it->second;
}

void EngineSound::addEmitter(EmissionGroup group, const std::string& name)
{
    const std::size_t g = static_cast<std::size_t>(group);
    const std::optional<HarmonicTuning>& harmonic = m_tuning.harmonics[g];
    if (!harmonic) {
        return;
    }
    const auto loops = harmonic->loops.find(name);
    if (loops == harmonic->loops.end()) {
        return;
    }
    const std::string bankPath = engineBankPath(m_library->vfs(), harmonic->bank);
    if (bankPath.empty()) {
        logWarning("audio: {} names bank {}, which is missing", m_tuning.harmonicFiles[g], harmonic->bank);
        return;
    }

    Emitter emitter;
    emitter.name = name;
    emitter.group = group;
    emitter.dsp = m_tuning.engine.groupDsp[g] ? &*m_tuning.engine.groupDsp[g] : &harmonic->dsp;
    emitter.viewVolume = emitterVolume(name);
    if (const CarModelTuning::View* view = m_tuning.model.view(m_options.view)) {
        emitter.pan = view->stereoPan(name);
    }
    if (const auto position = m_tuning.model.emitterPositions.find(name);
        position != m_tuning.model.emitterPositions.end()) {
        emitter.localPosition = position->second;
    }
    if (const auto rolloff = m_tuning.rolloffs.find(name); rolloff != m_tuning.rolloffs.end()) {
        emitter.rolloff = rolloff->second;
    }
    const std::shared_ptr<const FsbBank> bank = m_library->bank(bankPath);
    // Each side of a split exhaust plays one channel; anything else plays
    // whole samples, in stereo where the recording is.
    const bool splitExhaust = harmonic->loops.contains("ExhaustL") && harmonic->loops.contains("ExhaustR");
    emitter.bus = m_mixer.createBus();
    m_emitters.push_back(emitter);
    Emitter& added = m_emitters.back();
    const std::size_t samples = bank->size();
    for (const EngineLoop& loop : loops->second) {
        const std::size_t candidate = loop.sampleIndex < samples ? loop.sampleIndex : loop.sampleIndex - samples;
        const int channels = candidate < samples ? bank->sample(candidate).channels : 0;
        const std::optional<LoopSource> source = resolveLoopIndex(loop.sampleIndex, samples, channels);
        if (!source) {
            logWarning("audio: {} {} loop index {} is past the {} samples of {}", m_tuning.harmonicFiles[g], name,
                loop.sampleIndex, samples, bankPath);
            continue;
        }
        const int channel = splitExhaust || loop.sampleIndex >= samples ? source->channel : -1;
        const std::shared_ptr<const PcmBuffer> pcm = m_library->sampleChannel(bankPath, source->sample, channel);
        VoiceSettings voice;
        voice.volume = 0.0F;
        voice.loop = true;
        voice.bus = added.bus;
        voice.pan = m_options.spatial ? 0.0F : added.pan;
        voice.spatial = m_options.spatial;
        voice.position = m_position + m_basis * added.localPosition;
        voice.rolloff = added.rolloff;
        // Random start points keep two sides that play the same mono
        // recording from phasing against each other.
        std::uniform_int_distribution<std::size_t> start(0, pcm->frames() - 1);
        voice.startFrame = start(m_random);
        added.loops.push_back(LoopVoice{loop, pcm, voice, kNoVoice});
        added.loopTable.push_back(loop);
    }
}

void EngineSound::build()
{
    addEmitter(EmissionGroup::Intake, "EngineIntake");
    addEmitter(EmissionGroup::Ambient, "EngineAmbient");
    addEmitter(EmissionGroup::Exhaust, "ExhaustL");
    addEmitter(EmissionGroup::Exhaust, "ExhaustR");
    if (m_emitters.empty()) {
        throw DataError("{}: the car's harmonic tunings have no loops", m_tuning.media);
    }
    const auto exhausts = std::count_if(m_emitters.begin(), m_emitters.end(), [](const Emitter& e) {
        return e.group == EmissionGroup::Exhaust;
    });
    if (exhausts == 1 && !m_options.spatial) {
        // A single recorded exhaust stands for both pipes, so it plays centred.
        for (Emitter& emitter : m_emitters) {
            if (emitter.group == EmissionGroup::Exhaust) {
                emitter.pan = 0.0F;
                for (LoopVoice& loop : emitter.loops) {
                    loop.settings.pan = 0.0F;
                    if (loop.voice != kNoVoice) {
                        m_mixer.setPan(loop.voice, 0.0F);
                    }
                }
            }
        }
    }

    const std::optional<HarmonicTuning>& exhaust = m_tuning.harmonics[static_cast<std::size_t>(EmissionGroup::Exhaust)];
    if (exhaust && exhaust->burble) {
        m_burbleBank = engineBankPath(m_library->vfs(), exhaust->burble->bank);
        if (m_burbleBank.empty()) {
            logWarning("audio: burble bank {} is missing", exhaust->burble->bank);
        } else {
            m_burble = &*exhaust->burble;
            const std::shared_ptr<const FsbBank> bank = m_library->bank(m_burbleBank);
            const std::uint32_t count = std::min<std::uint32_t>(m_burble->burbles + m_burble->backfires,
                static_cast<std::uint32_t>(bank->size()));
            for (std::uint32_t i = 0; i < count; ++i) {
                m_library->sample(m_burbleBank, i);
            }
        }
    }
    std::size_t loopCount = 0;
    for (const Emitter& emitter : m_emitters) {
        loopCount += emitter.loops.size();
    }
    std::string banks;
    for (std::size_t g = 0; g < m_tuning.harmonics.size(); ++g) {
        if (m_tuning.harmonics[g]) {
            banks += std::format("{}{}={}", banks.empty() ? "" : ", ", groupName(static_cast<EmissionGroup>(g)),
                m_tuning.harmonics[g]->bank);
        }
    }
    logInfo("audio: {} engine: {} emitters, {} loops ({})", m_tuning.media, m_emitters.size(), loopCount, banks);
}

void EngineSound::startEvents()
{
    const auto prepare = [this](const char* projectPath, const char* eventPath, float volume)
        -> std::unique_ptr<EventInstance> {
        try {
            const std::shared_ptr<const FevProject> project = m_library->project(projectPath);
            const FevEvent* event = project->findEvent(eventPath);
            if (!event) {
                logWarning("audio: {} has no event {}", projectPath, eventPath);
                return nullptr;
            }
            // Decode everything the event can play now rather than on the
            // game thread when it first sounds.
            for (const FevLayer& layer : event->layers) {
                for (const FevSoundInstance& sound : layer.sounds) {
                    for (const FevWaveform& waveform : project->soundDefinition(sound.soundDefinition).waveforms) {
                        if (waveform.type == FevWaveform::Type::Wavetable) {
                            m_library->waveform(projectPath, waveform);
                        }
                    }
                }
            }
            EventSettings settings;
            settings.volume = volume;
            settings.seed = m_random();
            settings.spatial = m_options.spatial;
            settings.position = m_position;
            return std::make_unique<EventInstance>(m_mixer, *m_library, projectPath, project, *event, settings);
        } catch (const DataError& e) {
            logWarning("audio: {} {} unavailable: {}", projectPath, eventPath, e.what());
            return nullptr;
        }
    };
    if (m_options.tyres) {
        TyreSoundOptions tyres;
        // The game gives the car in focus a roll and a skid per wheel and
        // other cars one pair; the player's car is the one heard unplaced.
        tyres.perWheel = !m_options.spatial;
        tyres.spatial = m_options.spatial;
        tyres.volume = otherVolume("TireVolume") * m_volume;
        tyres.seed = m_random();
        try {
            m_tyres = std::make_unique<TyreSound>(m_mixer, m_library, tyres);
        } catch (const DataError& e) {
            logWarning("audio: tyre sounds unavailable: {}", e.what());
        }
    }
    if (m_options.transmission) {
        m_shift = prepare(kTransmissionsProject, kShiftEvent,
            m_tuning.engine.shiftVolume * otherVolume("TransmissionVolume") * m_volume);
        if (m_shift) {
            m_shift->setParameter("IsFocus", 1.0F);
        }
    }
}

EngineInputs EngineSound::inputs(float rpm, float throttle) const
{
    EngineInputs in;
    const float idle = m_tuning.engine.idleRpm;
    in.rpm = std::clamp((rpm - idle) / (m_tuning.engine.redlineRpm - idle), 0.0F, 1.0F);
    in.throttle = throttle;
    // No torque reaches the audio from physics, so load is estimated: the
    // throttle opening drives, and a closed throttle brakes in proportion to
    // engine speed.
    in.posTorque = throttle;
    in.negTorque = (1.0F - throttle) * in.rpm;
    return in;
}

void EngineSound::update(float rpm, float throttle, float dt)
{
    dt = std::max(dt, 0.0F);
    const float smoothing = 1.0F - std::exp(-dt / 0.04F);
    m_throttle += (std::clamp(throttle, 0.0F, 1.0F) - m_throttle) * smoothing;

    m_limiter = rpm >= m_tuning.engine.redlineRpm * 0.985F && throttle > 0.5F;
    float effectiveThrottle = m_throttle;
    if (m_limiter) {
        m_limiterClock = std::fmod(m_limiterClock + dt, kLimiterPeriod);
        if (m_limiterClock < kLimiterCut) {
            effectiveThrottle = 0.0F;
        }
    } else {
        m_limiterClock = 0.0F;
    }
    const EngineInputs in = inputs(rpm, effectiveThrottle);

    m_shiftBoost = std::max(0.0F, m_shiftBoost - m_shiftBoostRate * dt);
    const bool running = rpm >= m_tuning.engine.idleRpm * 0.3F;
    const float global = running
        ? m_tuning.engine.engineVolume * m_tuning.engine.globalVolume.evaluate(in) * m_volume * (1.0F + m_shiftBoost)
        : 0.0F;
    for (Emitter& emitter : m_emitters) {
        const std::vector<float> weights = loopWeights(emitter.loopTable, rpm);
        for (std::size_t i = 0; i < emitter.loops.size(); ++i) {
            LoopVoice& loop = emitter.loops[i];
            const float volume = weights[i] * loop.loop.volume;
            const float pitch = std::max(rpm, 1.0F) / loop.loop.rpmSample * loop.loop.pitch;
            if (volume <= 0.0F) {
                if (loop.voice != kNoVoice) {
                    m_mixer.stop(loop.voice, 0.05F);
                    loop.voice = kNoVoice;
                }
                continue;
            }
            if (loop.voice == kNoVoice) {
                // A fresh random start point, so a loop that comes back does
                // not always restart at the same place.
                std::uniform_int_distribution<std::size_t> start(0, loop.pcm->frames() - 1);
                loop.settings.startFrame = start(m_random);
                loop.settings.volume = volume;
                loop.settings.pitch = pitch;
                loop.voice = m_mixer.play(loop.pcm, loop.settings);
                continue;
            }
            m_mixer.setPitch(loop.voice, pitch);
            m_mixer.setVolume(loop.voice, volume);
        }
        m_mixer.setBusVolume(emitter.bus, global * emitter.dsp->volume.evaluate(in) * emitter.viewVolume);
        const std::vector<FilterSpec> filters = emitter.dsp->filters(in);
        m_mixer.setBusFilters(emitter.bus, filters);
    }
    updateBurbles(in, rpm, dt);

    if (m_shift) {
        m_shift->setParameter("TransmissionRPM", rpm);
    }
}

void EngineSound::playBurble(std::uint32_t index, float volume)
{
    if (m_burbleBank.empty()) {
        return;
    }
    std::shared_ptr<const PcmBuffer> pcm;
    try {
        pcm = m_library->sample(m_burbleBank, index);
    } catch (const DataError& e) {
        logWarning("audio: burble {} unavailable: {}", index, e.what());
        return;
    }
    VoiceSettings voice;
    voice.volume = volume * m_volume;
    voice.spatial = m_options.spatial;
    glm::vec3 local{0.0F};
    for (const Emitter& emitter : m_emitters) {
        if (emitter.group == EmissionGroup::Exhaust) {
            local = emitter.localPosition;
            break;
        }
    }
    voice.position = m_position + m_basis * glm::vec3(0.0F, local.y, local.z);
    const bool backfire = index >= m_burble->burbles;
    if (const auto rolloff = m_tuning.rolloffs.find(backfire ? "Backfire" : "Burble");
        rolloff != m_tuning.rolloffs.end()) {
        voice.rolloff = rolloff->second;
    }
    m_mixer.play(std::move(pcm), voice);
}

void EngineSound::updateBurbles(const EngineInputs& in, float rpm, float dt)
{
    if (!m_burble) {
        return;
    }
    std::uniform_real_distribution<float> unit(0.0F, 1.0F);
    const float exhaustVolume = emitterVolume("ExhaustL");
    const auto tick = [&](OneShotTimer& timer, bool window, float minTime, float maxTime, int silence,
                          float volume, std::uint32_t first, std::uint32_t count) {
        if (!window || count == 0 || maxTime <= 0.0F) {
            timer.armed = false;
            return;
        }
        const auto delay = [&] { return (minTime + (maxTime - minTime) * unit(m_random)) / 1000.0F; };
        if (!timer.armed) {
            timer.armed = true;
            timer.remaining = delay();
            return;
        }
        timer.remaining -= dt;
        if (timer.remaining > 0.0F) {
            return;
        }
        timer.remaining = delay();
        if (unit(m_random) * 100.0F < static_cast<float>(silence)) {
            return;
        }
        std::uniform_int_distribution<std::uint32_t> pick(first, first + count - 1);
        playBurble(pick(m_random), volume);
    };

    const bool overrun = in.throttle < 0.1F;
    const float burbleVolume = m_tuning.engine.burbleVolume * m_burble->volume.evaluate(in) * exhaustVolume
        * m_tuning.engine.engineVolume;
    tick(m_burbleTimer, overrun && rpm >= m_burble->minRpm && rpm <= m_burble->maxRpm && burbleVolume > 0.0F,
        m_burble->minTime, m_burble->maxTime, m_burble->silenceWeighting, burbleVolume, 0, m_burble->burbles);

    const auto level = static_cast<std::size_t>(std::clamp(m_options.exhaustUpgrade, 0, 3));
    if (level < m_tuning.backfires.size()) {
        const BackfireLevel& backfire = m_tuning.backfires[level];
        const float volume = m_tuning.engine.backfireVolume * backfire.volume.evaluate(in) * exhaustVolume;
        const bool window = overrun && in.rpm >= backfire.minRpm && in.rpm <= backfire.maxRpm;
        tick(m_backfireTimer, window, backfire.minTime, backfire.maxTime, backfire.silenceWeighting, volume,
            m_burble->burbles, m_burble->backfires);
        if (m_backfireQueued && backfire.maxTime > 0.0F && m_burble->backfires > 0 && volume > 0.0F) {
            std::uniform_int_distribution<std::uint32_t> pick(m_burble->burbles,
                m_burble->burbles + m_burble->backfires - 1);
            if (unit(m_random) * 100.0F >= static_cast<float>(backfire.silenceWeighting)) {
                playBurble(pick(m_random), volume);
            }
        }
    }
    m_backfireQueued = false;
}

void EngineSound::shiftGear(int fromGear, int toGear)
{
    const bool up = toGear > fromGear;
    const float boost = (up ? m_tuning.engine.shiftBoostUp : m_tuning.engine.shiftBoostDown) / 100.0F;
    const float time = up ? m_tuning.engine.shiftBoostUpTime : m_tuning.engine.shiftBoostDownTime;
    m_shiftBoost = boost;
    m_shiftBoostRate = time > 0.0F ? boost / time : boost;
    if (m_shift) {
        m_shift->stop(0.0F);
        m_shift->setVolume(m_tuning.engine.shiftVolume * otherVolume("TransmissionVolume") * m_volume);
        m_shift->start();
    }
    m_backfireQueued = up;
}

void EngineSound::updateTyres(std::span<const TyreInput, 4> wheels, const TyreCarInput& car, float dt)
{
    if (m_tyres) {
        m_tyres->update(wheels, car, dt);
    }
}

void EngineSound::setTransform(const glm::vec3& position, const glm::mat3& basis)
{
    m_position = position;
    m_basis = basis;
    if (!m_options.spatial) {
        return;
    }
    for (Emitter& emitter : m_emitters) {
        const glm::vec3 world = position + basis * emitter.localPosition;
        for (LoopVoice& loop : emitter.loops) {
            loop.settings.position = world;
            if (loop.voice != kNoVoice) {
                m_mixer.setPosition(loop.voice, world);
            }
        }
    }
    if (m_shift) {
        m_shift->setPosition(position);
    }
}

void EngineSound::setVolume(float volume)
{
    m_volume = std::max(volume, 0.0F);
    if (m_tyres) {
        m_tyres->setVolume(otherVolume("TireVolume") * m_volume);
    }
}

} // namespace fh::audio
