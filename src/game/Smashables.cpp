#include "Smashables.h"

#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"
#include "world/WorldIndex.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace fh {

namespace {

constexpr float kGravity = 9.80665F;
/// Placements and model placements agree to the millimetre; this snaps both
/// to the same key.
constexpr float kMatchCell = 0.05F;
constexpr float kBodyCell = 16.0F;
/// Mass per cubic metre of bounding box: props are mostly air (a sign is a
/// thin board on a post), so far below any solid material.
constexpr float kDensity = 60.0F;
constexpr float kMinMass = 3.0F;
constexpr float kMaxMass = 250.0F;
constexpr float kRestitution = 0.25F;
constexpr float kFriction = 0.6F;

std::uint64_t positionKey(const glm::vec3& p)
{
    const auto q = [](float v) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(v / kMatchCell)) & 0x1FFFFF); };
    return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
}

/// "CO_SignG_001.2.rmb" gives "CO_SignG_001".
std::string baseType(std::string_view physicsType)
{
    std::string name(physicsType);
    for (const char* suffix : {".rmb"}) {
        if (endsWithNoCase(name, suffix)) {
            name.resize(name.size() - std::string_view(suffix).size());
        }
    }
    const std::size_t dot = name.rfind('.');
    if (dot != std::string::npos
        && std::all_of(name.begin() + static_cast<std::ptrdiff_t>(dot) + 1, name.end(),
            [](char c) { return c >= '0' && c <= '9'; })) {
        name.resize(dot);
    }
    return name;
}

glm::mat3 worldInverseInertia(const Smashables::Body& body)
{
    const glm::mat3 r = glm::mat3_cast(body.orientation);
    const glm::mat3 local(glm::vec3(body.inverseInertia.x, 0.0F, 0.0F), glm::vec3(0.0F, body.inverseInertia.y, 0.0F),
        glm::vec3(0.0F, 0.0F, body.inverseInertia.z));
    return r * local * glm::transpose(r);
}

float bodyResponse(const Smashables::Body& body, const glm::vec3& point, const glm::vec3& direction)
{
    const glm::vec3 r = point - body.position;
    const glm::vec3 angular = glm::cross(worldInverseInertia(body) * glm::cross(r, direction), r);
    return 1.0F / body.mass + glm::dot(direction, angular);
}

glm::vec3 bodyPointVelocity(const Smashables::Body& body, const glm::vec3& point)
{
    return body.velocity + glm::cross(body.angularVelocity, point - body.position);
}

void applyBodyImpulse(Smashables::Body& body, const glm::vec3& impulse, const glm::vec3& point)
{
    body.velocity += impulse / body.mass;
    body.angularVelocity += worldInverseInertia(body) * glm::cross(point - body.position, impulse);
}

glm::vec3 sphereCentre(const Smashables::Body& body, const glm::vec4& sphere)
{
    if (!body.loose) {
        return body.restCentre + glm::vec3(sphere);
    }
    return body.position + body.orientation * glm::vec3(sphere);
}

} // namespace

glm::mat4 Smashables::Body::displacement() const
{
    if (!loose) {
        return glm::mat4(1.0F);
    }
    glm::mat4 current = glm::mat4_cast(orientation);
    current[3] = glm::vec4(position, 1.0F);
    glm::mat4 rest(1.0F);
    rest[3] = glm::vec4(-restCentre, 1.0F);
    return current * rest;
}

std::uint64_t Smashables::cellKey(int x, int z)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
}

Smashables Smashables::load(const Vfs& vfs, std::string_view track, const world::WorldIndex& index)
{
    const std::string path = std::format("media/tracks/{}/Ribbon_00/CollObjs.xml", track);
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        throw DataError("{} not found", path);
    }
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(text->data(), text->size());
    if (!parsed) {
        throw DataError("{}: {}", path, parsed.description());
    }

    // Placements grouped by position: the pieces of one object share it.
    struct Group {
        std::string type;
        std::vector<std::uint32_t> chunks;
    };
    std::unordered_map<std::uint64_t, std::size_t> groupByKey;
    std::vector<Group> groups;
    std::size_t placements = 0;
    for (const pugi::xml_node node : doc.child("CollObjs").children()) {
        const pugi::xml_node pos = node.child("Pos");
        const glm::vec3 position(
            pos.attribute("x").as_float(), pos.attribute("y").as_float(), pos.attribute("z").as_float());
        ++placements;
        const auto [it, inserted] = groupByKey.try_emplace(positionKey(position), groups.size());
        if (inserted) {
            groups.push_back({baseType(node.attribute("PhysicsType").as_string()), {}});
        }
    }

    Smashables result;
    const auto& chunks = index.chunks();
    result.m_chunkMask.assign(chunks.size(), false);
    for (std::uint32_t i = 0; i < chunks.size(); ++i) {
        const world::WorldChunk& chunk = chunks[i];
        if (!chunk.placed || chunk.scattered) {
            continue;
        }
        const auto it = groupByKey.find(positionKey(chunk.placement.position));
        if (it != groupByKey.end()) {
            groups[it->second].chunks.push_back(i);
        }
    }

    for (Group& group : groups) {
        if (group.chunks.empty()) {
            continue;
        }
        Body body;
        body.type = group.type;
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        float finestBand = std::numeric_limits<float>::max();
        for (std::uint32_t c : group.chunks) {
            lo = glm::min(lo, chunks[c].boundsMin);
            hi = glm::max(hi, chunks[c].boundsMax);
            finestBand = std::min(finestBand, chunks[c].bandStart);
            result.m_chunkMask[c] = true;
        }
        for (std::uint32_t c : group.chunks) {
            if (chunks[c].bandStart <= finestBand) {
                body.finestChunks.push_back(c);
            }
        }
        body.chunks = std::move(group.chunks);
        body.restCentre = (lo + hi) * 0.5F;
        body.halfExtents = glm::max((hi - lo) * 0.5F, glm::vec3(0.05F));
        const glm::vec3 size = body.halfExtents * 2.0F;
        body.mass = std::clamp(size.x * size.y * size.z * kDensity, kMinMass, kMaxMass);
        body.inverseInertia = glm::vec3(12.0F / (body.mass * (size.y * size.y + size.z * size.z)),
            12.0F / (body.mass * (size.x * size.x + size.z * size.z)),
            12.0F / (body.mass * (size.x * size.x + size.y * size.y)));

        // A row of spheres along the longest side, as thick as the middle
        // extent: a pole becomes a stack, a fence a line.
        const glm::vec3 h = body.halfExtents;
        int longAxis = 0;
        if (h.y > h[longAxis]) {
            longAxis = 1;
        }
        if (h.z > h[longAxis]) {
            longAxis = 2;
        }
        float sorted[3] = {h.x, h.y, h.z};
        std::sort(sorted, sorted + 3);
        const float radius = std::clamp(sorted[1], 0.08F, 0.6F);
        const float length = h[longAxis];
        const int count = std::clamp(static_cast<int>(std::ceil(length / radius)), 1, 8);
        for (int k = 0; k < count; ++k) {
            glm::vec3 centre(0.0F);
            const float t = count == 1 ? 0.0F : static_cast<float>(k) / static_cast<float>(count - 1);
            centre[longAxis] = -length + radius + t * std::max(2.0F * (length - radius), 0.0F);
            body.spheres.emplace_back(centre, radius);
        }
        const auto cx = static_cast<int>(std::floor(body.restCentre.x / kBodyCell));
        const auto cz = static_cast<int>(std::floor(body.restCentre.z / kBodyCell));
        result.m_cells[cellKey(cx, cz)].push_back(result.m_bodies.size());
        result.m_bodies.push_back(std::move(body));
    }
    logInfo("smashables: {} objects from {} placements", result.m_bodies.size(), placements);
    return result;
}

std::vector<std::size_t> Smashables::nearby(const glm::vec3& point, float radius) const
{
    std::vector<std::size_t> found;
    const int x0 = static_cast<int>(std::floor((point.x - radius) / kBodyCell));
    const int x1 = static_cast<int>(std::floor((point.x + radius) / kBodyCell));
    const int z0 = static_cast<int>(std::floor((point.z - radius) / kBodyCell));
    const int z1 = static_cast<int>(std::floor((point.z + radius) / kBodyCell));
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const auto it = m_cells.find(cellKey(x, z));
            if (it != m_cells.end()) {
                found.insert(found.end(), it->second.begin(), it->second.end());
            }
        }
    }
    // Loose bodies may have travelled away from their rest cell.
    for (std::size_t i : m_loose) {
        if (glm::length(m_bodies[i].position - point) < radius + 10.0F) {
            found.push_back(i);
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

void Smashables::knockLoose(std::size_t index)
{
    Body& body = m_bodies[index];
    body.loose = true;
    body.sleeping = false;
    body.sleepTimer = 0.0F;
    body.position = body.restCentre;
    body.orientation = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    body.velocity = glm::vec3(0.0F);
    body.angularVelocity = glm::vec3(0.0F);
    m_loose.push_back(index);
}

std::vector<std::size_t> Smashables::collideCar(CarPhysics& car, const std::vector<glm::vec4>& carSpheres)
{
    std::vector<std::size_t> knocked;
    const std::vector<std::size_t> candidates = nearby(car.position(), 8.0F);
    for (std::size_t index : candidates) {
        Body& body = m_bodies[index];
        for (const glm::vec4& carSphere : carSpheres) {
            const glm::vec3 carCentre(carSphere);
            for (const glm::vec4& sphere : body.spheres) {
                const glm::vec3 centre = sphereCentre(body, sphere);
                const glm::vec3 offset = carCentre - centre;
                const float distance = glm::length(offset);
                const float reach = carSphere.w + sphere.w;
                if (distance >= reach || distance < 1e-4F) {
                    continue;
                }
                if (!body.loose) {
                    knockLoose(index);
                    knocked.push_back(index);
                }
                body.sleeping = false;
                body.sleepTimer = 0.0F;
                const glm::vec3 normal = offset / distance;
                const glm::vec3 point = centre + normal * sphere.w;
                const float approach = glm::dot(car.worldPointVelocity(point) - bodyPointVelocity(body, point), normal);
                if (approach < 0.0F) {
                    const float response = car.impulseResponse(point, normal) + bodyResponse(body, point, normal);
                    const float impulse = -(1.0F + kRestitution) * approach / response;
                    car.applyImpulse(normal * impulse, point);
                    applyBodyImpulse(body, -normal * impulse, point);
                }
                // The light object gives way, not the car.
                body.position -= normal * (reach - distance) * 0.9F;
            }
        }
    }
    return knocked;
}

void Smashables::step(float dt, const GroundQuery& ground)
{
    for (std::size_t index : m_loose) {
        Body& body = m_bodies[index];
        if (body.sleeping) {
            continue;
        }
        body.velocity.y -= kGravity * dt;
        // Air and rolling losses, so tumbling debris settles.
        body.velocity *= 1.0F - 0.1F * dt;
        body.angularVelocity *= 1.0F - 0.8F * dt;
        body.position += body.velocity * dt;
        const glm::quat spin(0.0F, body.angularVelocity.x, body.angularVelocity.y, body.angularVelocity.z);
        body.orientation = glm::normalize(body.orientation + spin * body.orientation * (0.5F * dt));

        for (const glm::vec4& sphere : body.spheres) {
            const glm::vec3 centre = sphereCentre(body, sphere);
            GroundHit hit;
            const glm::vec3 origin = centre + glm::vec3(0.0F, sphere.w + 0.5F, 0.0F);
            if (!ground.raycast(origin, glm::vec3(0.0F, -1.0F, 0.0F), sphere.w * 2.0F + 1.0F, hit)) {
                continue;
            }
            const float depth = sphere.w - glm::dot(centre - hit.point, hit.normal);
            if (depth <= 0.0F) {
                continue;
            }
            const glm::vec3 n = hit.normal;
            const glm::vec3 point = centre - n * sphere.w;
            const glm::vec3 velocity = bodyPointVelocity(body, point);
            const float approach = glm::dot(velocity, n);
            if (approach < 0.0F) {
                const float bounce = approach < -1.5F ? kRestitution : 0.0F;
                const float normalImpulse = -(1.0F + bounce) * approach / bodyResponse(body, point, n);
                applyBodyImpulse(body, n * normalImpulse, point);
                const glm::vec3 after = bodyPointVelocity(body, point);
                glm::vec3 tangent = after - n * glm::dot(after, n);
                const float slide = glm::length(tangent);
                if (slide > 1e-4F) {
                    tangent /= slide;
                    const float frictionImpulse
                        = std::min(slide / bodyResponse(body, point, tangent), kFriction * normalImpulse);
                    applyBodyImpulse(body, -tangent * frictionImpulse, point);
                }
            }
            body.position += n * depth * 0.8F;
        }

        const bool still = glm::length(body.velocity) < 0.15F && glm::length(body.angularVelocity) < 0.3F;
        body.sleepTimer = still ? body.sleepTimer + dt : 0.0F;
        if (body.sleepTimer > 0.8F || body.position.y < -600.0F) {
            body.sleeping = true;
            body.velocity = glm::vec3(0.0F);
            body.angularVelocity = glm::vec3(0.0F);
        }
    }
}

std::vector<std::size_t> Smashables::reset()
{
    std::vector<std::size_t> was = m_loose;
    for (std::size_t index : m_loose) {
        Body& body = m_bodies[index];
        body.loose = false;
        body.sleeping = false;
        body.velocity = glm::vec3(0.0F);
        body.angularVelocity = glm::vec3(0.0F);
    }
    m_loose.clear();
    return was;
}

} // namespace fh
