#pragma once

#include <string>

struct SDL_Window;

namespace openhorizon {

/// Shows a blocking error box over `parent` (keeps it above a fullscreen
/// game); false when errorDialogsAllowed() says no or it fails. Main
/// thread only; works before SDL is initialised.
bool showErrorDialog(const std::string& title, const std::string& message, SDL_Window* parent = nullptr);

} // namespace openhorizon
