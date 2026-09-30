#include "PresentTarget.h"

#include <algorithm>

namespace openhorizon::render {

PresentTarget::~PresentTarget()
{
    release();
}

void PresentTarget::release()
{
    if (m_framebuffer != 0) {
        glDeleteFramebuffers(1, &m_framebuffer);
        glDeleteTextures(1, &m_colour);
        glDeleteTextures(1, &m_depth);
        m_framebuffer = 0;
        m_colour = 0;
        m_depth = 0;
    }
}

void PresentTarget::resize(int windowWidth, int windowHeight)
{
    windowWidth = std::max(windowWidth, 16);
    windowHeight = std::max(windowHeight, 9);
    if (windowWidth == m_windowWidth && windowHeight == m_windowHeight && m_framebuffer != 0) {
        return;
    }
    m_windowWidth = windowWidth;
    m_windowHeight = windowHeight;
    // The largest 16:9 picture the window holds, so it is copied pixel for
    // pixel with bars on the sides or above and below.
    m_width = std::min(windowWidth, windowHeight * 16 / 9);
    m_height = m_width * 9 / 16;
    release();
    // sRGB like the window's own framebuffer, so the tone map's encoding
    // and the HUD's raw writes land as they did there.
    glCreateTextures(GL_TEXTURE_2D, 1, &m_colour);
    glTextureStorage2D(m_colour, 1, GL_SRGB8_ALPHA8, m_width, m_height);
    glCreateTextures(GL_TEXTURE_2D, 1, &m_depth);
    glTextureStorage2D(m_depth, 1, GL_DEPTH24_STENCIL8, m_width, m_height);
    glCreateFramebuffers(1, &m_framebuffer);
    glNamedFramebufferTexture(m_framebuffer, GL_COLOR_ATTACHMENT0, m_colour, 0);
    glNamedFramebufferTexture(m_framebuffer, GL_DEPTH_STENCIL_ATTACHMENT, m_depth, 0);
}

void PresentTarget::bind() const
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glViewport(0, 0, m_width, m_height);
}

void PresentTarget::present() const
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_windowWidth, m_windowHeight);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    const int x = (m_windowWidth - m_width) / 2;
    const int y = (m_windowHeight - m_height) / 2;
    glBlitNamedFramebuffer(m_framebuffer, 0, 0, 0, m_width, m_height, x, y, x + m_width, y + m_height,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

} // namespace openhorizon::render
