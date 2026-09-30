#include "DrivingLineRenderer.h"

#include "base/Error.h"
#include "vfs/Vfs.h"
#include "world/XboxTexture.h"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace openhorizon::render {

namespace {

/// The texture the game's line object loads (0x820295b4).
constexpr const char* kTexture = "media/ui/textures/raceline.xds";
static_assert(sizeof(DrivingLine::Vertex) == 24, "the vertex is laid out as RaceLineDecl");

/// D3DRS_DEPTHBIAS as the draw sets it (0x82000c68), in depth units.
constexpr float kDepthBias = 2.0e-5F;

constexpr const char* kVertex = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColour;
layout(location = 2) in vec2 aUv;
uniform mat4 uViewProjection;
uniform float uDepthBias;
out vec4 vColour;
out vec2 vUv;
void main()
{
    gl_Position = uViewProjection * vec4(aPosition, 1.0);
    gl_Position.z += uDepthBias * gl_Position.w;
    vColour = aColour;
    vUv = aUv;
}
)";

constexpr const char* kFragment = R"(#version 450 core
in vec4 vColour;
in vec2 vUv;
layout(binding = 0) uniform sampler2D uRaceLine;
layout(location = 0) out vec4 fragColour;
void main()
{
    // The game's shader (0x8215C848) writes the colour as it is into its
    // square-root scene target, which this target also is.
    fragColour = vec4(vColour.rgb, vColour.a * texture(uRaceLine, vUv).a);
}
)";

Texture loadTexture(const Vfs& vfs)
{
    const std::optional<Bytes> data = vfs.read(kTexture);
    if (!data) {
        throw DataError("{} is missing", kTexture);
    }
    const world::RgbaImage image = world::decodeXboxTexture(*data);
    // The file's own mip levels are not read; the top level's are
    // generated instead.
    Texture texture = Texture::fromRgba(image.width, image.height, image.rgba, false, true);
    glTextureParameteri(texture.id(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texture.id(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

} // namespace

DrivingLineRenderer::DrivingLineRenderer(const Vfs& vfs)
    : m_shader("driving line", kVertex, kFragment)
    , m_texture(loadTexture(vfs))
{
    glCreateVertexArrays(1, &m_vao);
    glEnableVertexArrayAttrib(m_vao, 0);
    glVertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(DrivingLine::Vertex, position));
    glVertexArrayAttribBinding(m_vao, 0, 0);
    glEnableVertexArrayAttrib(m_vao, 1);
    glVertexArrayAttribFormat(m_vao, 1, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(DrivingLine::Vertex, colour));
    glVertexArrayAttribBinding(m_vao, 1, 0);
    glEnableVertexArrayAttrib(m_vao, 2);
    glVertexArrayAttribFormat(m_vao, 2, 2, GL_FLOAT, GL_FALSE, offsetof(DrivingLine::Vertex, uv));
    glVertexArrayAttribBinding(m_vao, 2, 0);
}

DrivingLineRenderer::~DrivingLineRenderer()
{
    if (m_vbo) {
        glDeleteBuffers(1, &m_vbo);
    }
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
    }
}

void DrivingLineRenderer::draw(const std::vector<DrivingLine::Vertex>& chevrons, const glm::mat4& viewProjection)
{
    if (chevrons.size() < 4) {
        return;
    }
    // The game draws quad lists; here each quad is two triangles.
    m_triangles.clear();
    for (std::size_t quad = 0; quad + 3 < chevrons.size(); quad += 4) {
        for (const std::size_t corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
            m_triangles.push_back(chevrons[quad + corner]);
        }
    }
    const std::size_t bytes = m_triangles.size() * sizeof(DrivingLine::Vertex);
    if (bytes > m_capacity) {
        if (m_vbo) {
            glDeleteBuffers(1, &m_vbo);
        }
        m_capacity = bytes * 2;
        glCreateBuffers(1, &m_vbo);
        glNamedBufferData(m_vbo, static_cast<GLsizeiptr>(m_capacity), nullptr, GL_STREAM_DRAW);
    }
    glNamedBufferSubData(m_vbo, 0, static_cast<GLsizeiptr>(bytes), m_triangles.data());
    glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, sizeof(DrivingLine::Vertex));

    m_shader.use();
    m_shader.set("uViewProjection", viewProjection);
    m_shader.set("uDepthBias", kDepthBias);
    m_texture.bind(0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_triangles.size()));
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

} // namespace openhorizon::render
