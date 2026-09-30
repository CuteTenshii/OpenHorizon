#include "AnarkFormats.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <numbers>
#include <tuple>

namespace openhorizon::ui {

namespace {

float bitsToFloat(std::uint32_t raw)
{
    return std::bit_cast<float>(raw);
}

std::string readSizedString(BeReader& in)
{
    const std::uint32_t length = in.u32();
    const ByteView raw = in.bytes(length);
    std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    const std::size_t nul = text.find('\0');
    if (nul != std::string::npos) {
        text.resize(nul);
    }
    return text;
}

Attribute readAttribute(BeReader& in)
{
    const std::uint32_t key = in.u32();
    Attribute attribute;
    attribute.hash = key & 0x07FFFFFFU;
    attribute.type = static_cast<std::uint8_t>(key >> 27);
    attribute.raw = in.u32();
    return attribute;
}

/// Reads a section's u32 size and checks the parser consumed exactly that.
class Section {
public:
    Section(BeReader& in, const char* name)
        : m_in(in)
        , m_name(name)
    {
        m_size = in.u32();
        m_start = in.pos();
        in.need(m_size);
    }

    std::size_t end() const { return m_start + m_size; }

    void finish() const
    {
        if (m_in.pos() != end()) {
            m_in.fail(std::format("{} section ends at 0x{:x}, expected 0x{:x}", m_name, m_in.pos(), end()));
        }
    }

private:
    BeReader& m_in;
    const char* m_name;
    std::uint32_t m_size = 0;
    std::size_t m_start = 0;
};

BlendFactor blendFactor(std::uint32_t xenosBlend)
{
    switch (xenosBlend) {
    case 0:
        return BlendFactor::Zero;
    case 1:
        return BlendFactor::One;
    case 6:
        return BlendFactor::SourceAlpha;
    case 7:
        return BlendFactor::InverseSourceAlpha;
    default:
        return BlendFactor::Other;
    }
}

TextureWrap textureWrap(std::uint32_t value)
{
    switch (value) {
    case 0:
        return TextureWrap::Repeat;
    case 1:
        return TextureWrap::Mirror;
    default:
        return TextureWrap::Clamp;
    }
}

void readText(BeReader& in, std::uint32_t version, FbfText& text)
{
    in.skip(4);
    text.text = readSizedString(in);
    text.stringId = readSizedString(in);
    in.skip(8);
    text.font = readSizedString(in);
    text.size = static_cast<int>(in.u32());
    text.horizontalAlign = static_cast<int>(in.u32());
    text.verticalAlign = static_cast<int>(in.u32());
    in.skip(8);
    text.tracking = in.f32();
    text.leading = in.f32();
    in.skip(4);
    in.skip(1);
    if (version >= 1008) {
        text.authoredWidth = in.f32();
    }
    if (version >= 1007 && in.u8() != 0) {
        FbfTextEffects effects;
        effects.firstEnabled = in.u8() != 0;
        for (int c = 0; c < 4; ++c) {
            effects.firstColor[c] = in.f32();
        }
        effects.firstValues.x = in.f32();
        effects.firstValues.y = in.f32();
        effects.secondEnabled = in.u8() != 0;
        for (int c = 0; c < 4; ++c) {
            effects.secondColor[c] = in.f32();
        }
        effects.secondValue = in.f32();
        effects.thirdEnabled = in.u8() != 0;
        for (int c = 0; c < 3; ++c) {
            effects.thirdValues[c] = in.f32();
        }
        text.effects = effects;
    }
}

void readMeshes(BeReader& in, FbfFile& file, const std::string& source)
{
    if (in.atEnd()) {
        return;
    }
    const std::uint32_t count = in.u32();
    const std::uint32_t extraSize = in.u32();
    const std::uint32_t bufferSize = in.u32();
    constexpr std::size_t kDescriptorSize = 96;
    const std::size_t descriptorsStart = in.pos();
    const std::size_t groupsStart = descriptorsStart + count * kDescriptorSize;
    in.need(count * kDescriptorSize + extraSize + bufferSize);
    const std::size_t bufferStart = groupsStart + extraSize;
    const auto addressBase = static_cast<std::int64_t>(bufferStart);
    const ByteView data = in.data();
    const auto at = [&](std::int64_t address, std::size_t bytes) -> const std::uint8_t* {
        const std::int64_t offset = addressBase + address;
        if (address < 0
            || offset + static_cast<std::int64_t>(bytes) > static_cast<std::int64_t>(bufferStart + bufferSize)) {
            throw DataError("{}: mesh data at 0x{:x} (+{}) lies outside the mesh buffer", source, address, bytes);
        }
        return data.data() + offset;
    };

    file.meshes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* d = data.data() + descriptorsStart + i * kDescriptorSize;
        Mesh mesh;
        mesh.boundsMin = {loadBeF32(d + 16), loadBeF32(d + 20), loadBeF32(d + 24)};
        mesh.boundsMax = {loadBeF32(d + 32), loadBeF32(d + 36), loadBeF32(d + 40)};
        const std::uint32_t groupCount = loadBe32(d + 48);
        const std::uint32_t vertexAddress = loadBe32(d + 72) & ~3U;
        const std::uint32_t vertexBytes = ((loadBe32(d + 76) >> 2) & 0xFFFFFFU) * 4;
        const std::uint32_t groupsOffset = loadBe32(d + 80);
        constexpr std::size_t kVertexSize = 32;
        const std::size_t vertexCount = vertexBytes / kVertexSize;
        const std::uint8_t* v = at(vertexAddress, vertexCount * kVertexSize);
        mesh.vertices.resize(vertexCount);
        for (std::size_t k = 0; k < vertexCount; ++k) {
            const std::uint8_t* p = v + k * kVertexSize;
            MeshVertex& vertex = mesh.vertices[k];
            vertex.position = {loadBeF32(p), loadBeF32(p + 4), loadBeF32(p + 8)};
            vertex.normal = {loadBeF32(p + 12), loadBeF32(p + 16), loadBeF32(p + 20)};
            vertex.uv = {loadBeF32(p + 24), loadBeF32(p + 28)};
        }
        if (groupsOffset + static_cast<std::size_t>(groupCount) * 36 > extraSize) {
            throw DataError("{}: mesh {} primitive groups exceed the group table", source, i);
        }
        // Rectangles come with a back face that repeats the front with the
        // opposite winding; a triangle whose corner positions were already
        // seen is that copy.
        std::vector<std::array<float, 9>> seen;
        for (std::uint32_t g = 0; g < groupCount; ++g) {
            const std::uint8_t* group = data.data() + groupsStart + groupsOffset + g * 36;
            const std::uint32_t indexAddress = loadBe32(group + 28);
            const std::uint32_t indexBytes = loadBe32(group + 32);
            const std::size_t indexCount = indexBytes / 2 / 3 * 3;
            const std::uint8_t* ib = at(indexAddress, indexCount * 2);
            for (std::size_t t = 0; t + 2 < indexCount; t += 3) {
                std::uint16_t tri[3];
                bool valid = true;
                for (int c = 0; c < 3; ++c) {
                    tri[c] = loadBe16(ib + (t + static_cast<std::size_t>(c)) * 2);
                    valid = valid && tri[c] < vertexCount;
                }
                if (!valid) {
                    throw DataError("{}: mesh {} index out of range", source, i);
                }
                std::array<glm::vec3, 3> corners{
                    mesh.vertices[tri[0]].position, mesh.vertices[tri[1]].position, mesh.vertices[tri[2]].position};
                std::sort(corners.begin(), corners.end(), [](const glm::vec3& a, const glm::vec3& b) {
                    return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
                });
                const std::array<float, 9> key{corners[0].x, corners[0].y, corners[0].z, corners[1].x, corners[1].y,
                    corners[1].z, corners[2].x, corners[2].y, corners[2].z};
                if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                    continue;
                }
                seen.push_back(key);
                mesh.indices.insert(mesh.indices.end(), {tri[0], tri[1], tri[2]});
            }
        }
        file.meshes.push_back(std::move(mesh));
    }
    in.seek(bufferStart + bufferSize);
}

} // namespace

namespace command {
const std::uint32_t GoToSlideName = anarkHash("COMMAND_GOTOSLIDENAME");
const std::uint32_t SetProperty = anarkHash("COMMAND_SETPROPERTY");
const std::uint32_t GoToTime = anarkHash("COMMAND_GOTOTIME");
const std::uint32_t Play = anarkHash("COMMAND_PLAY");
} // namespace command

std::uint32_t anarkHash(std::string_view name)
{
    std::uint32_t h = 0;
    for (const char c : name) {
        h = h * 0x1003FU + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<signed char>(c)));
    }
    h = h * 0x1003FU;
    return h & 0x07FFFFFFU;
}

std::uint16_t stringIdHash(std::string_view id)
{
    std::uint32_t h = 0xFFFF;
    for (char c : id) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        h ^= static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<signed char>(c)));
        h = ((h >> 9) | (h << 7)) & 0xFFFFU;
    }
    return static_cast<std::uint16_t>(h);
}

float Attribute::asFloat() const
{
    if (type == static_cast<std::uint8_t>(AttributeType::Float) || type == 19) {
        return bitsToFloat(raw);
    }
    return static_cast<float>(static_cast<std::int32_t>(raw));
}

float Track::evaluate(float timeMs) const
{
    if (keys.empty()) {
        return 0.0F;
    }
    if (timeMs <= keys.front().time) {
        return keys.front().value;
    }
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        const Keyframe& a = keys[i];
        const Keyframe& b = keys[i + 1];
        if (timeMs <= b.time) {
            const float span = b.time - a.time;
            const float s = span > 0.0F ? (timeMs - a.time) / span : 1.0F;
            const float r = 1.0F - s;
            return r * r * r * a.value + 3.0F * r * r * s * a.control1 + 3.0F * r * s * s * a.control2
                + s * s * s * b.value;
        }
    }
    return keys.back().value;
}

BgfFile BgfFile::parse(ByteView data, const std::string& source)
{
    BeReader in(data, source);
    BgfFile file;
    if (in.u8() != 1) {
        in.fail("not a big-endian presentation");
    }
    in.skip(1);
    const std::uint32_t headerSize = in.u32();
    const std::size_t headerStart = in.pos();
    if (in.fixedString(8) != "AnarkBGF") {
        in.fail("missing AnarkBGF signature");
    }
    const std::uint32_t version = in.u32();
    if (version != 3) {
        in.fail(std::format("unsupported presentation version {}", version));
    }
    in.skip(4);
    file.width = in.u16();
    file.height = in.u16();
    in.seek(headerStart + headerSize);

    if (in.u32() != 100) {
        in.fail("unexpected count table size");
    }
    std::uint32_t counts[25];
    for (std::uint32_t& count : counts) {
        count = in.u32();
    }
    const std::uint32_t elementCount = counts[0] + counts[1];
    const std::uint32_t stringCount = counts[6];

    {
        Section section(in, "string");
        file.strings.reserve(stringCount);
        for (std::uint32_t i = 0; i < stringCount; ++i) {
            file.strings.push_back(readSizedString(in));
        }
        section.finish();
    }
    {
        Section section(in, "element");
        file.elements.reserve(elementCount);
        for (std::uint32_t i = 0; i < elementCount; ++i) {
            BgfElement element;
            element.nameHash = in.u32() & 0x07FFFFFFU;
            element.parent = in.s32();
            element.id = in.u32();
            element.kind = static_cast<ElementKind>(in.u8());
            const std::uint8_t flags = in.u8();
            if (flags & 4) {
                in.skip(5);
            }
            const std::uint32_t count = in.u32();
            element.attributes.reserve(count);
            for (std::uint32_t k = 0; k < count; ++k) {
                element.attributes.push_back(readAttribute(in));
            }
            if (element.parent >= static_cast<std::int32_t>(elementCount)) {
                in.fail(std::format("element {} has parent {} of {}", i, element.parent, elementCount));
            }
            file.elements.push_back(std::move(element));
        }
        section.finish();
    }
    {
        Section contracts(in, "contract");
        in.seek(contracts.end());
    }
    {
        Section section(in, "animation");
        while (in.pos() < section.end()) {
            Track track;
            track.element = in.u32();
            track.attribute = in.u32() & 0x07FFFFFFU;
            in.skip(2);
            const std::uint16_t count = in.u16();
            track.keys.reserve(count);
            for (std::uint16_t k = 0; k < count; ++k) {
                Keyframe key;
                key.time = static_cast<float>(in.u32());
                key.value = in.f32();
                in.skip(4);
                key.control1 = in.f32();
                in.skip(4);
                key.control2 = in.f32();
                track.keys.push_back(key);
            }
            file.tracks.push_back(std::move(track));
        }
        section.finish();
    }
    {
        Section section(in, "logic");
        while (in.pos() < section.end()) {
            LogicOwner owner;
            owner.element = in.u32();
            const std::uint32_t handlers = in.u32();
            for (std::uint32_t h = 0; h < handlers; ++h) {
                LogicHandler handler;
                handler.event = in.u32() & 0x07FFFFFFU;
                const std::uint32_t actions = in.u32();
                in.need(static_cast<std::size_t>(actions) * 20);
                handler.actions.reserve(actions);
                for (std::uint32_t a = 0; a < actions; ++a) {
                    LogicAction action;
                    action.target = in.u32();
                    action.context = in.u32();
                    action.command = in.u32() & 0x07FFFFFFU;
                    action.key = in.u32();
                    action.value = in.u32();
                    if (action.target >= elementCount) {
                        in.fail(std::format("logic action names element {}", action.target));
                    }
                    handler.actions.push_back(action);
                }
                owner.handlers.push_back(std::move(handler));
            }
            if (owner.element >= elementCount) {
                in.fail(std::format("logic owner is element {}", owner.element));
            }
            file.logic.push_back(std::move(owner));
        }
        section.finish();
    }
    {
        Section parameters(in, "parameter");
        in.seek(parameters.end());
    }
    {
        Section section(in, "slide");
        while (in.pos() < section.end()) {
            Slide slide;
            slide.name = in.fixedString(16);
            slide.owner = in.u32();
            slide.flags = in.u8();
            slide.playCount = slide.flags & 3;
            slide.startsPaused = (slide.flags & 4) != 0;
            slide.pingPong = (slide.flags & 8) != 0;
            slide.nextAtEnd = (slide.flags & 16) != 0;
            in.skip(4);
            slide.durationMs = in.u32();
            const std::uint32_t count = in.u32();
            slide.entries.reserve(count);
            for (std::uint32_t e = 0; e < count; ++e) {
                SlideEntry entry;
                entry.element = in.u32();
                entry.active = in.u8() != 0;
                const std::uint32_t attributes = in.u32();
                for (std::uint32_t a = 0; a < attributes; ++a) {
                    entry.attributes.push_back(readAttribute(in));
                }
                const std::uint32_t tracks = in.u32();
                for (std::uint32_t t = 0; t < tracks; ++t) {
                    // Kind 0 names an animation track; kind 1 an action of
                    // the logic section (event handlers), which is not read.
                    const std::uint8_t kind = in.u8();
                    in.skip(1);
                    const std::uint16_t index = in.u16();
                    if (kind == 0) {
                        entry.tracks.push_back(index);
                    }
                }
                if (entry.element >= elementCount) {
                    in.fail(std::format("slide \"{}\" names element {}", slide.name, entry.element));
                }
                slide.entries.push_back(std::move(entry));
            }
            if (slide.owner >= elementCount) {
                in.fail(std::format("slide \"{}\" belongs to element {}", slide.name, slide.owner));
            }
            file.slides.push_back(std::move(slide));
        }
        section.finish();
    }
    for (const Slide& slide : file.slides) {
        for (const SlideEntry& entry : slide.entries) {
            for (const std::uint16_t track : entry.tracks) {
                if (track >= file.tracks.size()) {
                    in.fail(std::format("slide \"{}\" names track {} of {}", slide.name, track, file.tracks.size()));
                }
            }
        }
    }
    for (const Track& track : file.tracks) {
        if (track.element >= elementCount) {
            in.fail(std::format("animation track names element {}", track.element));
        }
    }
    return file;
}

BsgFile BsgFile::parse(ByteView data, const std::string& source)
{
    BeReader in(data, source);
    in.skip(8);
    if (in.fixedString(8) != "FGBkranA") {
        in.fail("missing FGBkranA signature");
    }
    in.seek(0x24);
    const std::uint32_t count = in.u32();
    in.skip(4);
    const std::uint32_t sectionSize = in.u32();
    constexpr std::uint32_t kNodeSize = 96;
    if (sectionSize != count * kNodeSize) {
        in.fail(std::format("{} nodes in a section of {} bytes", count, sectionSize));
    }
    BsgFile file;
    file.nodes.reserve(count);
    const auto vec3 = [&in]() {
        const float x = in.f32();
        const float y = in.f32();
        const float z = in.f32();
        return glm::vec3(x, y, z);
    };
    for (std::uint32_t i = 0; i < count; ++i) {
        BsgNode node;
        node.parent = in.s32();
        node.id = in.u32();
        node.type = static_cast<BsgNodeType>(in.u8());
        in.skip(3);
        node.position = vec3();
        node.rotation = vec3();
        node.scale = vec3();
        node.pivot = vec3();
        node.boundsMin = vec3();
        node.boundsMax = vec3();
        node.opacity = in.f32();
        node.layer = in.u8();
        in.skip(7);
        if (node.parent >= static_cast<std::int32_t>(count)) {
            in.fail(std::format("node {} has parent {}", i, node.parent));
        }
        file.nodes.push_back(node);
    }
    return file;
}

void UvTransform::matrix(glm::vec3& row0, glm::vec3& row1) const
{
    const float angle = rotationDegrees * std::numbers::pi_v<float> / 180.0F;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    // Texture u, then texture v before the flip.
    const glm::vec3 u(scale.x * c, -scale.x * s, scale.x * (c * position.x - s * position.y) + pivot.x);
    const glm::vec3 v(scale.y * s, scale.y * c, scale.y * (s * position.x + c * position.y) + pivot.y);
    row0 = u;
    row1 = glm::vec3(-v.x, -v.y, 1.0F - v.z);
}

FbfFile FbfFile::parse(ByteView data, const std::string& source)
{
    BeReader in(data, source);
    FbfFile file;
    file.version = in.u32();
    if (file.version < 1006 || file.version > 1008) {
        in.fail(std::format("unsupported render data version {}", file.version));
    }
    const std::uint32_t count = in.u32();
    file.records.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        FbfRecord record;
        const std::uint32_t kind = in.u32();
        record.id = in.u32();
        record.name = readSizedString(in);
        switch (kind) {
        case 0:
        case 8:
        case 9:
        case 10:
            record.kind = FbfKind::Group;
            break;
        case 11:
            // A custom renderer node (such as a map); its u32 names the
            // renderer the game plugs in.
            record.kind = FbfKind::Group;
            in.skip(4);
            break;
        case 1:
            record.kind = FbfKind::Text;
            readText(in, file.version, record.text);
            break;
        case 2:
            record.kind = FbfKind::Camera;
            in.skip(64);
            break;
        case 3:
            record.kind = FbfKind::Layer;
            in.skip(24);
            break;
        case 4:
            record.kind = FbfKind::Model;
            record.mesh = in.u32();
            in.skip(4);
            break;
        case 5: {
            record.kind = FbfKind::Material;
            for (int c = 0; c < 4; ++c) {
                record.material.diffuse[c] = in.f32();
            }
            in.skip(13 * 4);
            in.skip(4 * 5);
            record.material.sourceBlend = blendFactor(in.u32());
            record.material.destinationBlend = blendFactor(in.u32());
            in.skip(8);
            break;
        }
        case 6:
            record.kind = FbfKind::Light;
            in.skip(104);
            break;
        case 7: {
            record.kind = FbfKind::Image;
            record.image.material = in.u32();
            record.image.path = readSizedString(in);
            for (float& value : record.image.storedMatrix) {
                value = in.f32();
            }
            record.image.uv.rotationDegrees = in.f32();
            record.image.uv.position.x = in.f32();
            record.image.uv.position.y = in.f32();
            record.image.uv.scale.x = in.f32();
            record.image.uv.scale.y = in.f32();
            record.image.uv.pivot.x = in.f32();
            record.image.uv.pivot.y = in.f32();
            record.image.mappingMode = in.u32();
            in.skip(12);
            record.image.wrapU = textureWrap(in.u32());
            record.image.wrapV = textureWrap(in.u32());
            break;
        }
        default:
            in.fail(std::format("record {} (\"{}\") has unknown kind {}", i, record.name, kind));
        }
        file.records.push_back(std::move(record));
    }
    readMeshes(in, file, source);
    for (const FbfRecord& record : file.records) {
        if (record.kind == FbfKind::Model && record.mesh >= file.meshes.size()) {
            throw DataError(
                "{}: model \"{}\" uses mesh {} of {}", source, record.name, record.mesh, file.meshes.size());
        }
    }
    return file;
}

SceneFiles SceneFiles::load(const Vfs& vfs, std::string_view path)
{
    const std::string base(path);
    SceneFiles scene;
    scene.bgf = BgfFile::parse(vfs.readOrThrow(base + ".bgf"), base + ".bgf");
    scene.bsg = BsgFile::parse(vfs.readOrThrow(base + ".bsg"), base + ".bsg");
    scene.fbf = FbfFile::parse(vfs.readOrThrow(base + ".fbf"), base + ".fbf");
    return scene;
}

std::string texturePathForScene(std::string_view scenePath)
{
    std::string path = Vfs::normalize(scenePath);
    const std::size_t dot = path.rfind('.');
    const std::size_t slash = path.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        path.resize(dot);
    }
    return path + ".xds";
}

} // namespace openhorizon::ui
