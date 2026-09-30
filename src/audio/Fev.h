#pragma once

#include "base/Bytes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace openhorizon::audio {

/// A sound bank an FEV project plays from; `name` + ".fsb" next to the .fev.
struct FevBank {
    std::string name;
    /// 0x200 on banks loaded into memory, 0x80 on the one streamed bank
    /// (AMB_Quads_Stream); inferred from which banks carry which value.
    std::uint32_t flags = 0;
    std::uint32_t maxStreams = 0;
    /// The FSB header hash the bank was built with; 0 in version 0x37 files.
    std::uint64_t hash = 0;
};

struct FevCategory {
    std::string name;
    float volume = 1.0F;
    float pitch = 0.0F;
    std::vector<FevCategory> children;
};

/// A sound placed on a layer: plays while the layer's controlling parameter
/// lies in [start, start + length] of the parameter's range, as fractions.
struct FevSoundInstance {
    std::uint16_t soundDefinition = 0;
    float start = 0.0F;
    float length = 1.0F;
    /// Plays once rather than looping. Inferred: the flag is set on every
    /// UI click, voice line, impact and turbo blow-off and clear on every
    /// ambience bed, wind, tyre roll and engine loop.
    bool oneShot = false;
};

struct FevEnvelopePoint {
    /// Position along the controlling parameter, 0..1.
    float position = 0.0F;
    float value = 0.0F;
    std::uint32_t curveShape = 0;
};

/// A curve on a layer that drives a property from one of the event's
/// parameters. The property word always has bit 0x4 set (all 2439
/// envelopes on the disc); the bit above it says what the curve drives,
/// inferred from the curves: 0x8 volume (values 0..1: skid volume rises
/// with SlipAngle, engine layers fade in just above idle rpm), 0x10 pitch
/// (values around 0.5; the scale is unknown), none: a parameter of a DSP
/// effect (the first envelope of an effect names it, later ones point at it
/// through `parent`). 0x20, 0x80, 0x100 and 0x400 also occur, unidentified.
/// The controlling parameter index is verified to be in range on every
/// envelope and matches the curve shapes where they are telling.
struct FevEnvelope {
    bool isVolume() const { return (property & ~0x7U) == 0x8U; }
    bool isPitch() const { return (property & ~0x7U) == 0x10U; }
    bool isEffect() const { return (property & ~0x7U) == 0; }

    /// Index of the envelope that owns the DSP effect this one also drives,
    /// or -1.
    std::int32_t parent = -1;
    /// "FMOD Highpass" and similar; empty for event-level properties.
    std::string dspName;
    std::uint32_t dspParameter = 0;
    std::uint32_t property = 0;
    /// Index into FevEvent::parameters of the parameter the curve follows.
    std::uint32_t parameter = 0;
    std::vector<FevEnvelopePoint> points;

    /// The curve's value at `position` (0..1), linear between points and
    /// flat beyond the ends.
    float valueAt(float position) const;
};

struct FevLayer {
    /// A layer whose sounds do not follow any parameter (the VO projects).
    static constexpr std::uint16_t kNoParameter = 0xFFFF;

    std::uint16_t flags = 0;
    std::int16_t priority = -1;
    /// Index into FevEvent::parameters of the parameter sound instances are
    /// placed along, or kNoParameter.
    std::uint16_t controlParameter = 0;
    std::vector<FevSoundInstance> sounds;
    std::vector<FevEnvelope> envelopes;
};

struct FevParameter {
    std::string name;
    /// Change per second when the parameter runs by itself (a "Timer"
    /// parameter has 0.4).
    float velocity = 0.0F;
    float minimum = 0.0F;
    float maximum = 1.0F;
    std::uint32_t flags = 0;
    float seekSpeed = 0.0F;
    std::vector<float> sustainPoints;
};

/// Designer user properties: type 0 integer, 1 float (both seen), 2 string
/// (presumed from FMOD Designer; absent from the disc).
struct FevUserProperty {
    std::string name;
    std::variant<std::int32_t, float, std::string> value;
};

struct FevEvent {
    std::string name;
    /// Group path and name joined with '/', without the project name, e.g.
    /// "Transmissions/ShiftComplete/L0".
    std::string path;
    /// A Designer "simple event": one sound, no layers or parameters.
    bool simple = false;
    std::array<std::uint8_t, 16> guid{};
    float volume = 1.0F;
    /// In octaves (inferred: 0 on almost every event).
    float pitch = 0.0F;
    std::uint32_t priority = 128;
    std::uint32_t maxPlaybacks = 1;
    /// FMOD_MODE bits: 0x8 2D, 0x10 3D, 0x80000 world relative, 0x100000
    /// logarithmic rolloff, 0x4000000 custom rolloff.
    std::uint32_t mode = 0;
    float minDistance = 1.0F;
    float maxDistance = 10000.0F;
    std::vector<FevLayer> layers;
    std::vector<FevParameter> parameters;
    std::vector<FevUserProperty> userProperties;
    std::vector<std::string> categories;

    bool is3d() const { return (mode & 0x10U) != 0; }
    const FevParameter* findParameter(std::string_view parameterName) const;
};

/// An event group: a folder of events and subgroups. Its user properties
/// hold settings the game reads by name (Tires.fev's "Tires/Tires" group
/// carries the tyre parameter smoothing rates).
struct FevGroup {
    /// Parent groups and name joined with '/', without the project name,
    /// e.g. "Tires/LOD1/OnRoad".
    std::string path;
    std::vector<FevUserProperty> userProperties;

    const FevUserProperty* findUserProperty(std::string_view propertyName) const;
};

struct FevWaveform {
    enum class Type : std::uint32_t { Wavetable = 0, Oscillator = 1, DontPlay = 2, Programmer = 3 };
    Type type = Type::Wavetable;
    /// Relative chance of being picked among the definition's waveforms.
    std::uint32_t weight = 100;
    /// The source .wav path in the Designer project.
    std::string fileName;
    std::string bankName;
    std::uint32_t sampleIndex = 0;
    std::uint32_t lengthMs = 0;
};

struct FevSoundDefinition {
    /// Starts with '/', e.g. "/HornA_Short".
    std::string name;
    std::uint32_t propertyTemplate = 0;
    std::vector<FevWaveform> waveforms;
};

/// An FMOD Designer event project in the FEV1 layout, versions 0x00400000
/// (52 files on the disc) and 0x00370000 (Damage and ExhNoise).
///
/// The parser reads every one of the disc's 54 projects to exactly the end
/// of the file, and 6032 of the 6088 wavetable waveforms that name a bank on
/// the disc name the FSB sample at their index (the rest point at banks that
/// were rebuilt after the project, e.g. Damage.fsb). That is the verified
/// part. Field meanings below are inferred from values unless said
/// otherwise. All little-endian; a string is a u32 byte count including the
/// NUL, then the bytes (count 0: empty, no bytes).
///
///     char[4] "FEV1", u32 version, u32, u32 (unknown)
///     [0x40] u32 n, then n (u32 id, u32 value) pairs (memory use table)
///     string project name
///     u32 bank count, banks: u32 flags, u32 max streams,
///         [0x40] u8[8] FSB hash (verified equal to the bank's), string name
///     category tree: string name, f32 volume, f32 pitch, u32, u32,
///         u32 child count, children
///     u32 group count, groups: string name, user properties,
///         u32 subgroup count, u32 event count, subgroups, events
///     event: u32 type (0x10 simple, 0x8 complex), string name,
///         [0x40] u8[16] GUID, property block of 132 bytes (128 in 0x37,
///         which lacks the steal priority): f32 volume, f32 pitch,
///         f32 pitch and f32 volume randomisation, u32 priority,
///         u32 max playbacks, [0x40] u32 steal priority, u32 mode,
///         f32 min distance, f32 max distance, then cone, speaker level
///         and reverb settings that are not decoded
///       simple: u32 sound count, sound instances, u32 category count,
///         strings
///       complex: u32 layer count, layers: u16 flags, s16 priority,
///         u16 control parameter (0xFFFF none), u16 sound count,
///         u16 envelope count,
///         sound instances, envelopes; u32 parameter count, parameters,
///         user properties, u32 category count, strings
///     sound instance, 58 bytes: u16 sound definition, f32 start,
///         f32 length, u32, u32 one-shot flag, s32 (-1 everywhere), then
///         36 bytes not decoded
///     envelope: s32 parent, string DSP name, u32 DSP parameter,
///         u32 property, [0x40] u32 flags, u32 point count,
///         points (f32 position, f32 value, u32 shape),
///         u32 controlling parameter, u32
///     parameter: string name, f32 velocity, f32 min, f32 max, u32 flags,
///         f32 seek speed, u32, u32 sustain point count, f32 points
///     user properties: u32 count, then string name, u32 type, value
///         (u32/f32, or a string for type 2)
///     u32 sound definition template count, templates of 70 bytes (60 in
///         0x37), not decoded
///     u32 sound definition count, definitions: string name,
///         u32 template, u32 waveform count, waveforms: u32 type,
///         u32 weight, and for wavetables string file name, string bank
///         name, u32 sample index in the bank, u32 length in ms
///     u32 reverb definition count (0 on the disc), then the music data as
///         a u32 size and a chunk tree starting "comp" that runs to the end
///         of the file (not decoded)
class FevProject {
public:
    /// Throws DataError naming `source` if the file does not have the layout
    /// above.
    static FevProject parse(ByteView data, std::string source);

    const std::string& name() const { return m_name; }
    std::uint32_t version() const { return m_version; }
    const std::vector<FevBank>& banks() const { return m_banks; }
    const FevCategory& categories() const { return m_categories; }
    const std::vector<FevEvent>& events() const { return m_events; }
    /// Every event group, parents before their subgroups.
    const std::vector<FevGroup>& groups() const { return m_groups; }
    const std::vector<FevSoundDefinition>& soundDefinitions() const { return m_soundDefinitions; }

    /// The event at `path` ("Group/Subgroup/Event"), compared without case;
    /// a leading project name is accepted.
    const FevEvent* findEvent(std::string_view path) const;
    /// The group at `path`, compared without case; a leading project name
    /// is accepted.
    const FevGroup* findGroup(std::string_view path) const;
    const FevSoundDefinition& soundDefinition(std::size_t index) const;

private:
    std::string m_name;
    std::uint32_t m_version = 0;
    std::vector<FevBank> m_banks;
    FevCategory m_categories;
    std::vector<FevEvent> m_events;
    std::vector<FevGroup> m_groups;
    std::vector<FevSoundDefinition> m_soundDefinitions;
};

} // namespace openhorizon::audio
