#include "Fev.h"

#include "LeReader.h"

namespace openhorizon::audio {

namespace {

constexpr std::uint32_t kVersion40 = 0x00400000;
constexpr std::uint32_t kSimpleEvent = 0x10;
constexpr std::uint32_t kComplexEvent = 0x08;
/// A sanity bound on counts so that a misread count fails with a clear
/// message instead of an enormous allocation.
constexpr std::uint32_t kMaxCount = 1U << 20;

struct ParsedProject {
    std::string name;
    std::uint32_t version = 0;
    std::vector<FevBank> banks;
    FevCategory categories;
    std::vector<FevEvent> events;
    std::vector<FevGroup> groups;
    std::vector<FevSoundDefinition> soundDefinitions;
};

class FevReader {
public:
    FevReader(ByteView data, std::string source)
        : m_in(data, std::move(source))
    {
    }

    ParsedProject read();

private:
    std::uint32_t count(const char* what)
    {
        const std::uint32_t n = m_in.u32();
        if (n > kMaxCount) {
            m_in.fail(std::format("implausible {} count {}", what, n));
        }
        return n;
    }

    FevCategory readCategory(int depth);
    void readGroup(const std::string& prefix, ParsedProject& contents, int depth);
    FevEvent readEvent(const std::string& prefix);
    FevSoundInstance readSoundInstance();
    FevEnvelope readEnvelope();
    FevParameter readParameter();
    std::vector<FevUserProperty> readUserProperties();
    FevSoundDefinition readSoundDefinition(std::uint32_t templateCount);

    LeReader m_in;
    std::uint32_t m_version = 0;
};

FevCategory FevReader::readCategory(int depth)
{
    if (depth > 64) {
        m_in.fail("category tree nested more than 64 levels");
    }
    FevCategory category;
    category.name = m_in.countedString();
    category.volume = m_in.f32();
    category.pitch = m_in.f32();
    m_in.skip(8);
    const std::uint32_t children = count("subcategory");
    category.children.reserve(children);
    for (std::uint32_t i = 0; i < children; ++i) {
        category.children.push_back(readCategory(depth + 1));
    }
    return category;
}

std::vector<FevUserProperty> FevReader::readUserProperties()
{
    const std::uint32_t n = count("user property");
    std::vector<FevUserProperty> properties;
    properties.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        FevUserProperty property;
        property.name = m_in.countedString();
        const std::uint32_t type = m_in.u32();
        switch (type) {
        case 0:
            property.value = m_in.s32();
            break;
        case 1:
            property.value = m_in.f32();
            break;
        case 2:
            property.value = m_in.countedString();
            break;
        default:
            m_in.fail(std::format("user property '{}' has unknown type {}", property.name, type));
        }
        properties.push_back(std::move(property));
    }
    return properties;
}

void FevReader::readGroup(const std::string& prefix, ParsedProject& contents, int depth)
{
    if (depth > 64) {
        m_in.fail("event groups nested more than 64 levels");
    }
    FevGroup group;
    group.path = prefix + m_in.countedString();
    group.userProperties = readUserProperties();
    const std::string path = group.path + "/";
    contents.groups.push_back(std::move(group));
    const std::uint32_t subgroups = count("subgroup");
    const std::uint32_t groupEvents = count("event");
    for (std::uint32_t i = 0; i < subgroups; ++i) {
        readGroup(path, contents, depth + 1);
    }
    for (std::uint32_t i = 0; i < groupEvents; ++i) {
        contents.events.push_back(readEvent(path));
    }
}

FevSoundInstance FevReader::readSoundInstance()
{
    FevSoundInstance sound;
    sound.soundDefinition = m_in.u16();
    sound.start = m_in.f32();
    sound.length = m_in.f32();
    m_in.skip(4);
    sound.oneShot = m_in.u32() != 0;
    m_in.skip(40);
    return sound;
}

FevEnvelope FevReader::readEnvelope()
{
    FevEnvelope envelope;
    envelope.parent = m_in.s32();
    envelope.dspName = m_in.countedString();
    envelope.dspParameter = m_in.u32();
    envelope.property = m_in.u32();
    if (m_version >= kVersion40) {
        m_in.skip(4);
    }
    const std::uint32_t points = count("envelope point");
    envelope.points.reserve(points);
    for (std::uint32_t i = 0; i < points; ++i) {
        FevEnvelopePoint point;
        point.position = m_in.f32();
        point.value = m_in.f32();
        point.curveShape = m_in.u32();
        envelope.points.push_back(point);
    }
    envelope.parameter = m_in.u32();
    m_in.skip(4);
    return envelope;
}

FevParameter FevReader::readParameter()
{
    FevParameter parameter;
    parameter.name = m_in.countedString();
    parameter.velocity = m_in.f32();
    parameter.minimum = m_in.f32();
    parameter.maximum = m_in.f32();
    parameter.flags = m_in.u32();
    parameter.seekSpeed = m_in.f32();
    m_in.skip(4);
    const std::uint32_t sustainPoints = count("sustain point");
    parameter.sustainPoints.reserve(sustainPoints);
    for (std::uint32_t i = 0; i < sustainPoints; ++i) {
        parameter.sustainPoints.push_back(m_in.f32());
    }
    return parameter;
}

FevEvent FevReader::readEvent(const std::string& prefix)
{
    const std::size_t start = m_in.pos();
    const std::uint32_t type = m_in.u32();
    if (type != kSimpleEvent && type != kComplexEvent) {
        m_in.seek(start);
        m_in.fail(std::format("event of unknown type 0x{:x}", type));
    }
    FevEvent event;
    event.simple = type == kSimpleEvent;
    event.name = m_in.countedString();
    event.path = prefix + event.name;
    if (m_version >= kVersion40) {
        const ByteView guid = m_in.bytes(event.guid.size());
        std::copy(guid.begin(), guid.end(), event.guid.begin());
    }
    const std::size_t properties = m_in.pos();
    event.volume = m_in.f32();
    event.pitch = m_in.f32();
    m_in.skip(8);
    event.priority = m_in.u32();
    event.maxPlaybacks = m_in.u32();
    if (m_version >= kVersion40) {
        m_in.skip(4);
    }
    event.mode = m_in.u32();
    event.minDistance = m_in.f32();
    event.maxDistance = m_in.f32();
    m_in.seek(properties);
    m_in.skip(m_version >= kVersion40 ? 132 : 128);

    if (event.simple) {
        const std::uint32_t sounds = count("sound");
        FevLayer layer;
        for (std::uint32_t i = 0; i < sounds; ++i) {
            layer.sounds.push_back(readSoundInstance());
        }
        event.layers.push_back(std::move(layer));
    } else {
        const std::uint32_t layers = count("layer");
        for (std::uint32_t l = 0; l < layers; ++l) {
            FevLayer layer;
            layer.flags = m_in.u16();
            layer.priority = m_in.s16();
            layer.controlParameter = m_in.u16();
            const std::uint16_t sounds = m_in.u16();
            const std::uint16_t envelopes = m_in.u16();
            for (std::uint16_t i = 0; i < sounds; ++i) {
                layer.sounds.push_back(readSoundInstance());
            }
            for (std::uint16_t i = 0; i < envelopes; ++i) {
                layer.envelopes.push_back(readEnvelope());
            }
            event.layers.push_back(std::move(layer));
        }
        const std::uint32_t parameters = count("parameter");
        for (std::uint32_t i = 0; i < parameters; ++i) {
            event.parameters.push_back(readParameter());
        }
        event.userProperties = readUserProperties();
    }
    const std::uint32_t categories = count("category");
    for (std::uint32_t i = 0; i < categories; ++i) {
        event.categories.push_back(m_in.countedString());
    }
    return event;
}

FevSoundDefinition FevReader::readSoundDefinition(std::uint32_t templateCount)
{
    FevSoundDefinition definition;
    definition.name = m_in.countedString();
    definition.propertyTemplate = m_in.u32();
    if (definition.propertyTemplate >= templateCount) {
        m_in.fail(std::format("sound definition '{}' uses template {} of {}", definition.name,
            definition.propertyTemplate, templateCount));
    }
    const std::uint32_t waveforms = count("waveform");
    for (std::uint32_t i = 0; i < waveforms; ++i) {
        FevWaveform waveform;
        const std::uint32_t type = m_in.u32();
        waveform.weight = m_in.u32();
        switch (type) {
        case 0:
            waveform.type = FevWaveform::Type::Wavetable;
            waveform.fileName = m_in.countedString();
            waveform.bankName = m_in.countedString();
            waveform.sampleIndex = m_in.u32();
            waveform.lengthMs = m_in.u32();
            break;
        case 2:
            waveform.type = FevWaveform::Type::DontPlay;
            break;
        case 3:
            waveform.type = FevWaveform::Type::Programmer;
            break;
        default:
            // Oscillators (type 1) carry settings whose layout no file on
            // the disc shows, so they cannot be skipped reliably.
            m_in.fail(std::format("sound definition '{}' has a waveform of unsupported type {}", definition.name,
                type));
        }
        definition.waveforms.push_back(std::move(waveform));
    }
    return definition;
}

ParsedProject FevReader::read()
{
    ParsedProject contents;
    if (m_in.fixedString(4) != "FEV1") {
        m_in.seek(0);
        m_in.fail("not an FEV1 event project");
    }
    m_version = m_in.u32();
    if (m_version != kVersion40 && m_version != 0x00370000) {
        m_in.fail(std::format("unsupported FEV version 0x{:08x}", m_version));
    }
    contents.version = m_version;
    m_in.skip(8);
    if (m_version >= kVersion40) {
        const std::uint32_t pairs = count("memory table entry");
        m_in.skip(static_cast<std::size_t>(pairs) * 8);
    }
    contents.name = m_in.countedString();

    const std::uint32_t banks = count("bank");
    for (std::uint32_t i = 0; i < banks; ++i) {
        FevBank bank;
        bank.flags = m_in.u32();
        bank.maxStreams = m_in.u32();
        if (m_version >= kVersion40) {
            bank.hash = m_in.u64();
        }
        bank.name = m_in.countedString();
        contents.banks.push_back(std::move(bank));
    }
    contents.categories = readCategory(0);

    const std::uint32_t groups = count("event group");
    for (std::uint32_t i = 0; i < groups; ++i) {
        readGroup({}, contents, 0);
    }

    const std::uint32_t templates = count("sound definition template");
    m_in.skip(static_cast<std::size_t>(templates) * (m_version >= kVersion40 ? 70 : 60));
    const std::uint32_t definitions = count("sound definition");
    for (std::uint32_t i = 0; i < definitions; ++i) {
        contents.soundDefinitions.push_back(readSoundDefinition(templates));
    }

    for (const FevEvent& event : contents.events) {
        for (const FevLayer& layer : event.layers) {
            for (const FevSoundInstance& sound : layer.sounds) {
                if (sound.soundDefinition >= definitions) {
                    m_in.fail(std::format("event '{}' uses sound definition {} of {}", event.path,
                        sound.soundDefinition, definitions));
                }
            }
            if (!event.simple && !layer.sounds.empty() && layer.controlParameter != FevLayer::kNoParameter
                && layer.controlParameter >= event.parameters.size()) {
                m_in.fail(std::format("event '{}' has a layer controlled by parameter {} of {}", event.path,
                    layer.controlParameter, event.parameters.size()));
            }
            for (const FevEnvelope& envelope : layer.envelopes) {
                if (envelope.parameter >= event.parameters.size()) {
                    m_in.fail(std::format("event '{}' has an envelope following parameter {} of {}", event.path,
                        envelope.parameter, event.parameters.size()));
                }
            }
        }
    }

    const std::uint32_t reverbs = m_in.u32();
    if (reverbs != 0) {
        m_in.fail(std::format("{} reverb definitions; their layout is unknown", reverbs));
    }
    const std::size_t musicStart = m_in.pos();
    const std::uint32_t musicSize = m_in.u32();
    if (m_in.fixedString(4) != "comp" || musicStart + musicSize != m_in.size()) {
        m_in.seek(musicStart);
        m_in.fail("the music data chunk does not end the file; the project was misread");
    }
    return contents;
}

} // namespace

FevProject FevProject::parse(ByteView data, std::string source)
{
    FevReader reader(data, std::move(source));
    ParsedProject contents = reader.read();
    FevProject project;
    project.m_name = std::move(contents.name);
    project.m_version = contents.version;
    project.m_banks = std::move(contents.banks);
    project.m_categories = std::move(contents.categories);
    project.m_events = std::move(contents.events);
    project.m_groups = std::move(contents.groups);
    project.m_soundDefinitions = std::move(contents.soundDefinitions);
    return project;
}

const FevEvent* FevProject::findEvent(std::string_view path) const
{
    for (const FevEvent& event : m_events) {
        if (iequals(event.path, path)) {
            return &event;
        }
    }
    if (startsWithNoCase(path, m_name) && path.size() > m_name.size() && path[m_name.size()] == '/') {
        return findEvent(path.substr(m_name.size() + 1));
    }
    return nullptr;
}

const FevGroup* FevProject::findGroup(std::string_view path) const
{
    for (const FevGroup& group : m_groups) {
        if (iequals(group.path, path)) {
            return &group;
        }
    }
    if (startsWithNoCase(path, m_name) && path.size() > m_name.size() && path[m_name.size()] == '/') {
        return findGroup(path.substr(m_name.size() + 1));
    }
    return nullptr;
}

const FevUserProperty* FevGroup::findUserProperty(std::string_view propertyName) const
{
    for (const FevUserProperty& property : userProperties) {
        if (iequals(property.name, propertyName)) {
            return &property;
        }
    }
    return nullptr;
}

const FevSoundDefinition& FevProject::soundDefinition(std::size_t index) const
{
    if (index >= m_soundDefinitions.size()) {
        throw DataError("FEV project {}: sound definition {} out of range ({})", m_name, index,
            m_soundDefinitions.size());
    }
    return m_soundDefinitions[index];
}

float FevEnvelope::valueAt(float position) const
{
    if (points.empty()) {
        return 1.0F;
    }
    if (position <= points.front().position) {
        return points.front().value;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (position <= points[i].position) {
            const FevEnvelopePoint& a = points[i - 1];
            const FevEnvelopePoint& b = points[i];
            const float span = b.position - a.position;
            const float t = span > 0.0F ? (position - a.position) / span : 1.0F;
            return a.value + (b.value - a.value) * t;
        }
    }
    return points.back().value;
}

const FevParameter* FevEvent::findParameter(std::string_view parameterName) const
{
    for (const FevParameter& parameter : parameters) {
        if (iequals(parameter.name, parameterName)) {
            return &parameter;
        }
    }
    return nullptr;
}

} // namespace openhorizon::audio
