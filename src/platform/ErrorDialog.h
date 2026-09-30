#pragma once

#include <string>

struct SDL_Window;

namespace openhorizon {

/// Shows `message` in the system's error box titled `title`, over `parent`
/// when given (so it stays in front of a fullscreen game), and waits for
/// the player to close it. Returns false without showing anything when
/// errorDialogsAllowed() says no one could answer it, or when the system
/// cannot show one. Callable before SDL is initialised; call it from the
/// main thread.
bool showErrorDialog(const std::string& title, const std::string& message, SDL_Window* parent = nullptr);

} // namespace openhorizon
