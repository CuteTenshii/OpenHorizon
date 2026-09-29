#pragma once

#include "render/Gl.h"

namespace fh::render {

/// The picture the game draws into: 16:9 whatever the window's shape, the
/// largest that fits it, as the console always output 16:9. Everything of a
/// frame (world, HUD, menus) draws here as it would into the window, and
/// present() copies it to the middle of the window, black around it.
class PresentTarget {
public:
    PresentTarget() = default;
    ~PresentTarget();
    PresentTarget(const PresentTarget&) = delete;
    PresentTarget& operator=(const PresentTarget&) = delete;

    /// Sizes the picture for a window, recreating it when that changes.
    void resize(int windowWidth, int windowHeight);
    /// Binds the picture as the framebuffer to draw into.
    void bind() const;
    GLuint framebuffer() const { return m_framebuffer; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    /// Copies the picture to the window's framebuffer, centred, and clears
    /// the bars around it.
    void present() const;

private:
    void release();

    GLuint m_framebuffer = 0;
    GLuint m_colour = 0;
    GLuint m_depth = 0;
    int m_width = 0;
    int m_height = 0;
    int m_windowWidth = 0;
    int m_windowHeight = 0;
};

} // namespace fh::render
