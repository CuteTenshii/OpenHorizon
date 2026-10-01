#include "Granny.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace openhorizon::animated::granny {

namespace {

/// Granny's member types (granny_member_type), as the type definitions
/// store them.
enum MemberType : std::uint32_t {
    kEnd = 0,
    kInline = 1,
    kReference = 2,
    kReferenceToArray = 3,
    kArrayOfReferences = 4,
    kVariantReference = 5,
    kReferenceToVariantArray = 7,
    kString = 8,
    kTransform = 9,
    kReal32 = 10,
    kInt8 = 11,
    kUInt8 = 12,
    kBinormalInt8 = 13,
    kNormalUInt8 = 14,
    kInt16 = 15,
    kUInt16 = 16,
    kBinormalInt16 = 17,
    kNormalUInt16 = 18,
    kInt32 = 19,
    kUInt32 = 20,
    kReal16 = 21,
    kEmptyReference = 22,
};

constexpr std::array<std::uint32_t, 4> kMagic{0xB595110E, 0x4BB5A56A, 0x502828EB, 0x04B37825};
constexpr std::size_t kHeaderSizeOffset = 0x10;
constexpr std::size_t kFormatOffset = 0x14;
constexpr std::size_t kInfoOffset = 0x20;
constexpr std::uint32_t kVersion = 7;
constexpr std::size_t kSectionBytes = 44;
constexpr std::size_t kFixupBytes = 12;
constexpr std::size_t kMemberBytes = 32;
constexpr std::size_t kTransformBytes = 68;
constexpr std::uint32_t kMaxSections = 64;
constexpr std::uint32_t kMaxCount = 1U << 20;
constexpr int kMaxDepth = 32;

struct Section {
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct MemberDef {
    std::uint32_t type = kEnd;
    std::string name;
    /// Address of the member's own type, when it has one.
    std::optional<std::size_t> definition;
    std::uint32_t width = 0;
};

/// An object in the file: its type's address and its own.
struct Object {
    std::size_t type = 0;
    std::size_t address = 0;
};

/// A member of an object: its definition and where its value lies.
struct Field {
    const MemberDef* def = nullptr;
    std::size_t address = 0;
};

class File {
public:
    File(ByteView data, const std::string& source)
        : m_data(data)
        , m_source(source)
    {
        if (data.size() < kInfoOffset + 44) {
            fail("too short for a Granny file");
        }
        for (std::size_t i = 0; i < kMagic.size(); ++i) {
            if (u32(i * 4) != kMagic[i]) {
                fail("not a big-endian 32-bit Granny file");
            }
        }
        if (u32(kFormatOffset) != 0) {
            fail("compressed Granny header");
        }
        (void)u32(kHeaderSizeOffset);
        if (u32(kInfoOffset) != kVersion) {
            fail("Granny file version {} is not 7", u32(kInfoOffset));
        }
        const std::size_t table = kInfoOffset + u32(kInfoOffset + 12);
        const std::uint32_t count = u32(kInfoOffset + 16);
        if (count == 0 || count > kMaxSections) {
            fail("implausible section count {}", count);
        }
        m_sections.resize(count);
        for (std::uint32_t s = 0; s < count; ++s) {
            const std::size_t entry = table + static_cast<std::size_t>(s) * kSectionBytes;
            if (u32(entry) != 0) {
                fail("section {} is compressed", s);
            }
            const std::uint32_t stored = u32(entry + 8);
            const std::uint32_t size = u32(entry + 12);
            if (stored != size) {
                fail("section {} stores {} bytes of {}", s, stored, size);
            }
            m_sections[s] = {u32(entry + 4), size};
            need(m_sections[s].offset, m_sections[s].size);
        }
        for (std::uint32_t s = 0; s < count; ++s) {
            const std::size_t entry = table + static_cast<std::size_t>(s) * kSectionBytes;
            const std::size_t fixups = u32(entry + 28);
            const std::uint32_t fixupCount = u32(entry + 32);
            if (fixupCount > kMaxCount) {
                fail("implausible fixup count");
            }
            for (std::uint32_t f = 0; f < fixupCount; ++f) {
                const std::size_t fixup = fixups + static_cast<std::size_t>(f) * kFixupBytes;
                const std::uint32_t from = u32(fixup);
                m_pointers[address(s, from)] = address(u32(fixup + 4), u32(fixup + 8));
            }
        }
        m_root = {address(u32(kInfoOffset + 20), u32(kInfoOffset + 24)),
            address(u32(kInfoOffset + 28), u32(kInfoOffset + 32))};
    }

    Object root() const { return m_root; }

    template <typename... Args> [[noreturn]] void fail(std::format_string<Args...> format, Args&&... args) const
    {
        throw DataError("{}: Granny file: {}", m_source, std::format(format, std::forward<Args>(args)...));
    }

    void need(std::size_t offset, std::size_t bytes) const
    {
        if (offset > m_data.size() || bytes > m_data.size() - offset) {
            fail("truncated at 0x{:x}", offset);
        }
    }
    std::uint32_t u32(std::size_t offset) const
    {
        need(offset, 4);
        return loadBe32(m_data.data() + offset);
    }
    std::uint16_t u16(std::size_t offset) const
    {
        need(offset, 2);
        return loadBe16(m_data.data() + offset);
    }
    std::uint8_t u8(std::size_t offset) const
    {
        need(offset, 1);
        return m_data[offset];
    }
    float f32(std::size_t offset) const { return std::bit_cast<float>(u32(offset)); }

    /// Where a pointer field points, or nothing for a null pointer.
    std::optional<std::size_t> pointer(std::size_t field) const
    {
        const auto it = m_pointers.find(field);
        if (it == m_pointers.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::string cString(std::size_t offset) const
    {
        std::string text;
        for (std::size_t at = offset;; ++at) {
            const std::uint8_t c = u8(at);
            if (c == 0) {
                return text;
            }
            text.push_back(static_cast<char>(c));
        }
    }

    const std::vector<MemberDef>& members(std::size_t type) const
    {
        const auto cached = m_members.find(type);
        if (cached != m_members.end()) {
            return cached->second;
        }
        std::vector<MemberDef> list;
        for (std::size_t at = type;; at += kMemberBytes) {
            MemberDef def;
            def.type = u32(at);
            if (def.type == kEnd) {
                break;
            }
            if (const auto name = pointer(at + 4)) {
                def.name = cString(*name);
            }
            def.definition = pointer(at + 8);
            def.width = u32(at + 12);
            if (def.width > kMaxCount) {
                fail("member {} has an implausible width", def.name);
            }
            list.push_back(std::move(def));
            if (list.size() > kMaxCount) {
                fail("a type does not end");
            }
        }
        return m_members.emplace(type, std::move(list)).first->second;
    }

    std::size_t memberSize(const MemberDef& def, int depth = 0) const
    {
        std::size_t size = 0;
        switch (def.type) {
        case kInline:
            if (!def.definition) {
                fail("inline member {} has no type", def.name);
            }
            size = typeSize(*def.definition, depth + 1);
            break;
        case kReference:
        case kString:
        case kEmptyReference:
            size = 4;
            break;
        case kReferenceToArray:
        case kArrayOfReferences:
        case kVariantReference:
            size = 8;
            break;
        case kReferenceToVariantArray:
            size = 12;
            break;
        case kTransform:
            size = kTransformBytes;
            break;
        case kReal32:
        case kInt32:
        case kUInt32:
            size = 4;
            break;
        case kInt16:
        case kUInt16:
        case kBinormalInt16:
        case kNormalUInt16:
        case kReal16:
            size = 2;
            break;
        case kInt8:
        case kUInt8:
        case kBinormalInt8:
        case kNormalUInt8:
            size = 1;
            break;
        default:
            fail("member {} has unknown type {}", def.name, def.type);
        }
        return size * std::max<std::uint32_t>(def.width, 1);
    }

    std::size_t typeSize(std::size_t type, int depth = 0) const
    {
        if (depth > kMaxDepth) {
            fail("types nest too deeply");
        }
        const auto cached = m_sizes.find(type);
        if (cached != m_sizes.end()) {
            return cached->second;
        }
        std::size_t size = 0;
        for (const MemberDef& def : members(type)) {
            size += memberSize(def, depth);
        }
        m_sizes.emplace(type, size);
        return size;
    }

    std::optional<Field> find(const Object& object, std::string_view name) const
    {
        std::size_t at = object.address;
        for (const MemberDef& def : members(object.type)) {
            if (def.name == name) {
                return Field{&def, at};
            }
            at += memberSize(def);
        }
        return std::nullopt;
    }

    Field field(const Object& object, std::string_view name) const
    {
        const std::optional<Field> found = find(object, name);
        if (!found) {
            fail("no member {}", name);
        }
        return *found;
    }

    Field field(const Object& object, std::string_view name, std::uint32_t type) const
    {
        const Field found = field(object, name);
        if (found.def->type != type) {
            fail("member {} has type {}, not {}", name, found.def->type, type);
        }
        return found;
    }

    std::string string(const Object& object, std::string_view name) const
    {
        const Field f = field(object, name, kString);
        const auto text = pointer(f.address);
        return text ? cString(*text) : std::string();
    }

    /// A number member of any primitive type, as a float (integers keep
    /// their value).
    float number(const Field& f, std::size_t index = 0) const
    {
        const std::size_t size = memberSize(*f.def) / std::max<std::uint32_t>(f.def->width, 1);
        const std::size_t at = f.address + index * size;
        switch (f.def->type) {
        case kReal32:
            return f32(at);
        case kInt32:
            return static_cast<float>(static_cast<std::int32_t>(u32(at)));
        case kUInt32:
            return static_cast<float>(u32(at));
        case kInt16:
        case kBinormalInt16:
            return static_cast<float>(static_cast<std::int16_t>(u16(at)));
        case kUInt16:
        case kNormalUInt16:
            return static_cast<float>(u16(at));
        case kReal16:
            return halfToFloat(u16(at));
        case kInt8:
        case kBinormalInt8:
            return static_cast<float>(static_cast<std::int8_t>(u8(at)));
        case kUInt8:
        case kNormalUInt8:
            return static_cast<float>(u8(at));
        default:
            fail("member {} is not a number", f.def->name);
        }
    }
    float number(const Object& object, std::string_view name) const { return number(field(object, name)); }
    std::int32_t integer(const Object& object, std::string_view name) const
    {
        const Field f = field(object, name);
        if (f.def->type == kInt32 || f.def->type == kUInt32) {
            return static_cast<std::int32_t>(u32(f.address));
        }
        return static_cast<std::int32_t>(number(f));
    }

    /// The numbers of a member that is an inline array of numbers, or a
    /// reference to an array of objects of one number member each.
    std::vector<float> numbers(const Object& object, std::string_view name) const
    {
        const Field f = field(object, name);
        std::vector<float> values;
        if (f.def->type == kReferenceToArray) {
            const std::uint32_t count = u32(f.address);
            const auto first = pointer(f.address + 4);
            if (count == 0 || !first) {
                return values;
            }
            if (count > kMaxCount || !f.def->definition) {
                fail("member {} is not an array of numbers", name);
            }
            const std::vector<MemberDef>& element = members(*f.def->definition);
            if (element.size() != 1) {
                fail("member {} is not an array of numbers", name);
            }
            const std::size_t stride = typeSize(*f.def->definition);
            values.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                values.push_back(number(Field{element.data(), *first + i * stride}));
            }
            return values;
        }
        const std::uint32_t width = std::max<std::uint32_t>(f.def->width, 1);
        values.reserve(width);
        for (std::uint32_t i = 0; i < width; ++i) {
            values.push_back(number(f, i));
        }
        return values;
    }

    std::optional<Object> reference(const Object& object, std::string_view name) const
    {
        const Field f = field(object, name, kReference);
        const auto target = pointer(f.address);
        if (!target || !f.def->definition) {
            return std::nullopt;
        }
        return Object{*f.def->definition, *target};
    }

    Object inlined(const Object& object, std::string_view name) const
    {
        const Field f = field(object, name, kInline);
        return Object{*f.def->definition, f.address};
    }

    std::optional<Object> variant(const Object& object, std::string_view name) const
    {
        const Field f = field(object, name, kVariantReference);
        const auto type = pointer(f.address);
        const auto target = pointer(f.address + 4);
        if (!type || !target) {
            return std::nullopt;
        }
        return Object{*type, *target};
    }

    std::vector<Object> array(const Object& object, std::string_view name) const
    {
        const Field f = field(object, name);
        std::vector<Object> objects;
        const std::uint32_t count = u32(f.address);
        const auto first = pointer(f.address + 4);
        if (count == 0 || !first) {
            return objects;
        }
        if (count > kMaxCount || !f.def->definition) {
            fail("member {} is not an array", name);
        }
        objects.reserve(count);
        if (f.def->type == kReferenceToArray) {
            const std::size_t stride = typeSize(*f.def->definition);
            for (std::uint32_t i = 0; i < count; ++i) {
                objects.push_back({*f.def->definition, *first + i * stride});
            }
        } else if (f.def->type == kArrayOfReferences) {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (const auto target = pointer(*first + i * 4)) {
                    objects.push_back({*f.def->definition, *target});
                }
            }
        } else {
            fail("member {} is not an array", name);
        }
        return objects;
    }

    Transform transform(const Object& object, std::string_view name) const
    {
        const Field f = field(object, name, kTransform);
        Transform t;
        t.flags = u32(f.address);
        t.position = {f32(f.address + 4), f32(f.address + 8), f32(f.address + 12)};
        t.orientation = glm::quat(f32(f.address + 28), f32(f.address + 16), f32(f.address + 20), f32(f.address + 24));
        // Stored row by row for column vectors (SS p); glm indexes
        // [column][row].
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                t.scaleShear[column][row] = f32(f.address + 32 + static_cast<std::size_t>(row * 3 + column) * 4);
            }
        }
        return t;
    }

    /// The name of the first member of a type, which Granny's curve data
    /// types use to carry their format's name.
    std::string firstMemberName(std::size_t type) const
    {
        const std::vector<MemberDef>& list = members(type);
        return list.empty() ? std::string() : list.front().name;
    }

private:
    std::size_t address(std::uint32_t section, std::uint32_t offset) const
    {
        if (section >= m_sections.size() || offset > m_sections[section].size) {
            fail("reference to section {} offset 0x{:x} outside the file", section, offset);
        }
        return m_sections[section].offset + offset;
    }

    ByteView m_data;
    const std::string& m_source;
    std::vector<Section> m_sections;
    std::unordered_map<std::size_t, std::size_t> m_pointers;
    mutable std::unordered_map<std::size_t, std::vector<MemberDef>> m_members;
    mutable std::unordered_map<std::size_t, std::size_t> m_sizes;
    Object m_root;
};

/// The name each format's curve data type carries in its first member.
constexpr std::array<std::string_view, 19> kFormatNames{"DaKeyframes32f", "DaK32fC32f", "DaIdentity", "DaConstant32f",
    "D3Constant32f", "D4Constant32f", "DaK16uC16u", "DaK8uC8u", "D4nK16uC15u", "D4nK8uC7u", "D3K16uC16u", "D3K8uC8u",
    "D9I1K16uC16u", "D9I3K16uC16u", "D9I1K8uC8u", "D9I3K8uC8u", "D3I1K32fC32f", "D3I1K16uC16u", "D3I1K8uC8u"};

/// The (scale, offset) pairs the quaternion formats pick their three
/// stored components' ranges from (0x820D5960), by four bits each of
/// ScaleOffsetTableEntries.
constexpr std::array<std::array<float, 2>, 16> kQuaternionScaleOffsets{{
    {1.414214F, -0.707107F},
    {0.707107F, -0.353553F},
    {0.353553F, -0.53033F},
    {0.353553F, -0.176777F},
    {0.353553F, 0.176777F},
    {0.176777F, -0.176777F},
    {0.176777F, -0.088388F},
    {0.176777F, 0.0F},
    {-1.414214F, 0.707107F},
    {-0.707107F, 0.353553F},
    {-0.353553F, 0.53033F},
    {-0.353553F, 0.176777F},
    {-0.353553F, -0.176777F},
    {-0.176777F, 0.176777F},
    {-0.176777F, 0.088388F},
    {-0.176777F, -0.0F},
}};

/// A knot scale stored as the top half of a float (the `Trunc` formats).
float truncatedFloat(float stored)
{
    return std::bit_cast<float>(static_cast<std::uint32_t>(stored) << 16);
}

/// Splits `values` (n knots, then n controls of `per` values) into knots
/// scaled by `knotScale`.
std::size_t knotCount(const File& file, std::size_t values, std::size_t per)
{
    if (values % (per + 1) != 0) {
        file.fail("a curve's knots and controls do not divide");
    }
    return values / (per + 1);
}

Curve readCurve(const File& file, const Object& curve2)
{
    Curve curve;
    const std::optional<Object> data = file.variant(curve2, "CurveData");
    if (!data) {
        return curve;
    }
    const std::string headerName = file.firstMemberName(data->type);
    const Object header = file.inlined(*data, headerName);
    const auto format = static_cast<std::size_t>(file.number(header, "Format"));
    if (format >= kFormatNames.size() || headerName != std::string("CurveDataHeader_") + kFormatNames[format].data()) {
        file.fail("curve data {} does not match format {}", headerName, format);
    }
    curve.format = static_cast<CurveFormat>(format);
    curve.degree = static_cast<int>(file.number(header, "Degree"));
    if (curve.degree > 3) {
        file.fail("curve degree {}", curve.degree);
    }
    switch (curve.format) {
    case CurveFormat::DaIdentity:
        curve.kind = Curve::Kind::Identity;
        curve.dimension = file.integer(*data, "Dimension");
        return curve;
    case CurveFormat::DaKeyframes32f:
        curve.kind = Curve::Kind::Keyframes;
        curve.dimension = file.integer(*data, "Dimension");
        curve.controls = file.numbers(*data, "Controls");
        return curve;
    case CurveFormat::DaConstant32f:
    case CurveFormat::D3Constant32f:
    case CurveFormat::D4Constant32f:
        curve.kind = Curve::Kind::Constant;
        curve.controls = file.numbers(*data, "Controls");
        curve.dimension = static_cast<int>(curve.controls.size());
        return curve;
    case CurveFormat::DaK32fC32f: {
        curve.kind = Curve::Kind::Spline;
        curve.knots = file.numbers(*data, "Knots");
        curve.controls = file.numbers(*data, "Controls");
        if (curve.knots.empty() || curve.controls.size() % curve.knots.size() != 0) {
            file.fail("a DaK32fC32f curve's controls do not match its knots");
        }
        curve.dimension = static_cast<int>(curve.controls.size() / curve.knots.size());
        return curve;
    }
    case CurveFormat::D4nK16uC15u:
    case CurveFormat::D4nK8uC7u: {
        // Verified for the 16-bit format (0x82A98600); the 8-bit one is
        // taken to work alike with 7-bit values.
        curve.kind = Curve::Kind::Spline;
        curve.dimension = 4;
        const bool wide = curve.format == CurveFormat::D4nK16uC15u;
        const std::uint32_t top = wide ? 0x8000U : 0x80U;
        const std::uint32_t mask = top - 1;
        const float unit = 1.0F / static_cast<float>(mask);
        const auto entries = static_cast<std::uint32_t>(file.number(*data, "ScaleOffsetTableEntries"));
        const float inverseKnotScale = file.number(*data, "OneOverKnotScale");
        const std::vector<float> values = file.numbers(*data, "KnotsControls");
        const std::size_t n = knotCount(file, values.size(), 3);
        std::array<float, 4> scales{};
        std::array<float, 4> offsets{};
        for (std::size_t k = 0; k < 4; ++k) {
            const auto& pair = kQuaternionScaleOffsets[(entries >> (4 * k)) & 15U];
            scales[k] = pair[0] * unit;
            offsets[k] = pair[1];
        }
        for (std::size_t i = 0; i < n; ++i) {
            curve.knots.push_back(values[i] / inverseKnotScale);
        }
        for (std::size_t i = 0; i < n; ++i) {
            const auto a = static_cast<std::uint32_t>(values[n + 3 * i]);
            const auto b = static_cast<std::uint32_t>(values[n + 3 * i + 1]);
            const auto c = static_cast<std::uint32_t>(values[n + 3 * i + 2]);
            // The component left out is named by the top bits of the
            // second and third values; the others follow it in turn.
            const std::uint32_t gift = (((b & top) != 0 ? 1U : 0U) << 1) | ((c & top) != 0 ? 1U : 0U);
            const std::uint32_t s1 = (gift + 1) & 3U;
            const std::uint32_t s2 = (s1 + 1) & 3U;
            const std::uint32_t s3 = (s2 + 1) & 3U;
            std::array<float, 4> q{};
            q[s1] = static_cast<float>(a & mask) * scales[s1] + offsets[s1];
            q[s2] = static_cast<float>(b & mask) * scales[s2] + offsets[s2];
            q[s3] = static_cast<float>(c & mask) * scales[s3] + offsets[s3];
            const float w = std::sqrt(std::max(0.0F, 1.0F - (q[s1] * q[s1] + q[s2] * q[s2] + q[s3] * q[s3])));
            q[gift] = (a & top) != 0 ? -w : w;
            curve.controls.insert(curve.controls.end(), q.begin(), q.end());
        }
        return curve;
    }
    default:
        break;
    }

    // Knots, then controls through a per-component scale and offset. Verified
    // for D3K16uC16u (0x82A7A308) and the D9I1 diagonal (0x82A7B900); the
    // 8-bit, D3I1 and D9I3 variants are assumed alike.
    curve.kind = Curve::Kind::Spline;
    const std::vector<float> values = file.numbers(*data, "KnotsControls");
    float knotScale = 1.0F;
    if (curve.format != CurveFormat::D3I1K32fC32f) {
        knotScale = 1.0F / truncatedFloat(file.number(*data, "OneOverKnotScaleTrunc"));
    }
    std::vector<float> scales;
    std::vector<float> offsets;
    std::size_t per = 0;
    switch (curve.format) {
    case CurveFormat::DaK16uC16u:
    case CurveFormat::DaK8uC8u: {
        const std::vector<float> scaleOffsets = file.numbers(*data, "ControlScaleOffsets");
        per = scaleOffsets.size() / 2;
        scales.assign(scaleOffsets.begin(), scaleOffsets.begin() + static_cast<std::ptrdiff_t>(per));
        offsets.assign(scaleOffsets.begin() + static_cast<std::ptrdiff_t>(per), scaleOffsets.end());
        curve.dimension = static_cast<int>(per);
        break;
    }
    case CurveFormat::D9I1K16uC16u:
    case CurveFormat::D9I1K8uC8u:
        scales = {file.number(*data, "ControlScale")};
        offsets = {file.number(*data, "ControlOffset")};
        per = 1;
        curve.dimension = 9;
        break;
    case CurveFormat::D9I3K16uC16u:
    case CurveFormat::D9I3K8uC8u:
        scales = file.numbers(*data, "ControlScales");
        offsets = file.numbers(*data, "ControlOffsets");
        per = 3;
        curve.dimension = 9;
        break;
    case CurveFormat::D3I1K32fC32f:
    case CurveFormat::D3I1K16uC16u:
    case CurveFormat::D3I1K8uC8u:
        scales = file.numbers(*data, "ControlScales");
        offsets = file.numbers(*data, "ControlOffsets");
        per = 1;
        curve.dimension = 3;
        break;
    default:
        scales = file.numbers(*data, "ControlScales");
        offsets = file.numbers(*data, "ControlOffsets");
        per = 3;
        curve.dimension = 3;
        break;
    }
    if (per == 0 || scales.size() < std::min<std::size_t>(per, 3) || offsets.size() != scales.size()) {
        file.fail("curve format {} has scales that do not fit", kFormatNames[format]);
    }
    const std::size_t n = knotCount(file, values.size(), per);
    for (std::size_t i = 0; i < n; ++i) {
        curve.knots.push_back(values[i] * knotScale);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const float* v = values.data() + n + per * i;
        switch (curve.format) {
        case CurveFormat::D3I1K32fC32f:
        case CurveFormat::D3I1K16uC16u:
        case CurveFormat::D3I1K8uC8u:
            for (std::size_t j = 0; j < 3; ++j) {
                curve.controls.push_back(v[0] * scales[j] + offsets[j]);
            }
            break;
        case CurveFormat::D9I1K16uC16u:
        case CurveFormat::D9I1K8uC8u:
        case CurveFormat::D9I3K16uC16u:
        case CurveFormat::D9I3K8uC8u: {
            std::array<float, 9> m{};
            for (std::size_t j = 0; j < 3; ++j) {
                const std::size_t source = per == 1 ? 0 : j;
                m[j * 4] = v[source] * scales[source] + offsets[source];
            }
            curve.controls.insert(curve.controls.end(), m.begin(), m.end());
            break;
        }
        default:
            for (std::size_t j = 0; j < per; ++j) {
                curve.controls.push_back(v[j] * scales[j] + offsets[j]);
            }
            break;
        }
    }
    if (curve.knots.empty()) {
        file.fail("a curve has no knots");
    }
    return curve;
}

Transform readTransform(const File& file, const Object& object, std::string_view name)
{
    return file.transform(object, name);
}

} // namespace

glm::mat4 Transform::matrix() const
{
    glm::mat3 linear(1.0F);
    if ((flags & kHasScaleShear) != 0) {
        linear = scaleShear;
    }
    if ((flags & kHasOrientation) != 0) {
        linear = glm::mat3_cast(orientation) * linear;
    }
    glm::mat4 m(linear);
    if ((flags & kHasPosition) != 0) {
        m[3] = glm::vec4(position, 1.0F);
    }
    return m;
}

int Model::findBone(std::string_view boneName) const
{
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == boneName) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Curve::sample(float time, float duration, bool loop, float* out) const
{
    const auto dim = static_cast<std::size_t>(dimension);
    if (kind == Kind::Identity || kind == Kind::Keyframes || dim == 0) {
        return false;
    }
    if (kind == Kind::Constant) {
        std::copy_n(controls.begin(), std::min(dim, controls.size()), out);
        return true;
    }
    const auto n = static_cast<std::ptrdiff_t>(knots.size());
    if (n == 0 || controls.size() < static_cast<std::size_t>(n) * dim) {
        return false;
    }
    // FindKnot: the first knot after the time, clamped to the last.
    std::ptrdiff_t i = std::upper_bound(knots.begin(), knots.end(), time) - knots.begin();
    i = std::min(i, n - 1);
    // A looping curve's last knot is its first one a duration later, so
    // indices past either end wrap over the other knots.
    const std::ptrdiff_t period = n - 1;
    const bool wraps = loop && period > 0;
    const auto index = [&](std::ptrdiff_t j, float& shift) {
        shift = 0.0F;
        if (!wraps) {
            return std::clamp<std::ptrdiff_t>(j, 0, n - 1);
        }
        if (j >= 0 && j < period) {
            return j;
        }
        const std::ptrdiff_t turns = j >= 0 ? j / period : -((-j + period - 1) / period);
        shift = static_cast<float>(turns) * duration;
        return j - turns * period;
    };
    const auto knot = [&](std::ptrdiff_t j) {
        float shift = 0.0F;
        const std::ptrdiff_t k = index(j, shift);
        return knots[static_cast<std::size_t>(k)] + shift;
    };
    const auto control = [&](std::ptrdiff_t j) {
        float shift = 0.0F;
        return controls.data() + static_cast<std::size_t>(index(j, shift)) * dim;
    };
    const auto ratio = [](float a, float b) { return b != 0.0F ? a / b : 0.0F; };
    switch (degree) {
    case 0:
        std::copy_n(control(i), dim, out);
        break;
    case 1: {
        const float k0 = knot(i - 1);
        const float u = ratio(time - k0, knot(i) - k0);
        const float* p0 = control(i - 1);
        const float* p1 = control(i);
        for (std::size_t d = 0; d < dim; ++d) {
            out[d] = p0[d] * (1.0F - u) + p1[d] * u;
        }
        break;
    }
    case 2: {
        const float km2 = knot(i - 2);
        const float km1 = knot(i - 1);
        const float k0 = knot(i);
        const float k1 = knot(i + 1);
        const float a = ratio(time - km2, k0 - km2);
        const float b = ratio(time - km1, k0 - km1);
        const float c = ratio(time - km1, k1 - km1);
        const float w0 = (1.0F - a) * (1.0F - b);
        const float w1 = a + b - a * b - b * c;
        const float w2 = b * c;
        const float* p0 = control(i - 2);
        const float* p1 = control(i - 1);
        const float* p2 = control(i);
        for (std::size_t d = 0; d < dim; ++d) {
            out[d] = w0 * p0[d] + w1 * p1[d] + w2 * p2[d];
        }
        break;
    }
    default: {
        const float km3 = knot(i - 3);
        const float km2 = knot(i - 2);
        const float km1 = knot(i - 1);
        const float k0 = knot(i);
        const float k1 = knot(i + 1);
        const float k2 = knot(i + 2);
        const float a = ratio(time - km1, k0 - km1);
        const float b = ratio(time - km2, k0 - km2);
        const float c = ratio(time - km1, k1 - km1);
        const float d3 = ratio(time - km3, k0 - km3);
        const float e = ratio(time - km2, k1 - km2);
        const float f = ratio(time - km1, k2 - km1);
        const float w0 = (1.0F - d3) * (1.0F - b) * (1.0F - a);
        const float w1 = (1.0F - e) * ((1.0F - c) * a + (1.0F - a) * b) + d3 * (1.0F - a) * (1.0F - b);
        const float w2 = (1.0F - f) * c * a + e * ((1.0F - c) * a + (1.0F - a) * b);
        const float w3 = c * a * f;
        const float* p0 = control(i - 3);
        const float* p1 = control(i - 2);
        const float* p2 = control(i - 1);
        const float* p3 = control(i);
        for (std::size_t d = 0; d < dim; ++d) {
            out[d] = w0 * p0[d] + w1 * p1[d] + w2 * p2[d] + w3 * p3[d];
        }
        break;
    }
    }
    return true;
}

FileInfo readFileInfo(ByteView data, const std::string& source)
{
    const File file(data, source);
    const Object root = file.root();
    FileInfo info;
    for (const Object& model : file.array(root, "Models")) {
        Model out;
        out.name = file.string(model, "Name");
        out.initialPlacement = readTransform(file, model, "InitialPlacement");
        if (const std::optional<Object> skeleton = file.reference(model, "Skeleton")) {
            out.skeletonName = file.string(*skeleton, "Name");
            for (const Object& bone : file.array(*skeleton, "Bones")) {
                Bone b;
                b.name = file.string(bone, "Name");
                b.parent = file.integer(bone, "ParentIndex");
                b.local = readTransform(file, bone, "Transform");
                const std::vector<float> inverse = file.numbers(bone, "InverseWorldTransform");
                if (inverse.size() != 16) {
                    file.fail("bone {} has no 4x4 inverse world matrix", b.name);
                }
                // Stored for row vectors with the translation in the last
                // row: read column-major, that is the column-vector matrix.
                b.inverseWorld = glm::make_mat4(inverse.data());
                if (b.parent >= static_cast<int>(out.bones.size())) {
                    file.fail("bone {} comes before its parent", b.name);
                }
                out.bones.push_back(std::move(b));
            }
        }
        info.models.push_back(std::move(out));
    }
    for (const Object& animation : file.array(root, "Animations")) {
        Animation out;
        out.name = file.string(animation, "Name");
        out.duration = file.number(animation, "Duration");
        out.timeStep = file.number(animation, "TimeStep");
        out.oversampling = file.number(animation, "Oversampling");
        for (const Object& group : file.array(animation, "TrackGroups")) {
            TrackGroup g;
            g.name = file.string(group, "Name");
            for (const Object& track : file.array(group, "TransformTracks")) {
                TransformTrack t;
                t.name = file.string(track, "Name");
                t.position = readCurve(file, file.inlined(track, "PositionCurve"));
                t.orientation = readCurve(file, file.inlined(track, "OrientationCurve"));
                t.scaleShear = readCurve(file, file.inlined(track, "ScaleShearCurve"));
                g.tracks.push_back(std::move(t));
            }
            out.groups.push_back(std::move(g));
        }
        info.animations.push_back(std::move(out));
    }
    return info;
}

void worldPose(const Model& model, const std::vector<Transform>& locals, std::vector<glm::mat4>& world)
{
    world.resize(model.bones.size());
    const glm::mat4 offset = model.initialPlacement.matrix();
    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        const Bone& bone = model.bones[i];
        const glm::mat4 local = (i < locals.size() ? locals[i] : bone.local).matrix();
        world[i] = (bone.parent >= 0 ? world[static_cast<std::size_t>(bone.parent)] : offset) * local;
    }
}

} // namespace openhorizon::animated::granny
