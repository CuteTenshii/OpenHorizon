#pragma once

namespace openhorizon {
class Window;
} // namespace openhorizon

namespace openhorizon::test {

/// A hidden 64x64 window with a current OpenGL context, made once per
/// process. Skips the test case when none can be made (no GPU).
Window& requireGlContext();

} // namespace openhorizon::test
