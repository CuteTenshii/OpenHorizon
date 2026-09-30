#pragma once

namespace openhorizon {
class Window;
} // namespace openhorizon

namespace openhorizon::test {

/// A hidden 64x64 window whose OpenGL context is current, made once per
/// process, for the cases that draw. Skips the test case that asks when
/// the machine can make none, as on a CI runner without a GPU.
Window& requireGlContext();

} // namespace openhorizon::test
