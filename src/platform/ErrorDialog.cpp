#include "ErrorDialog.h"

#include "base/Crash.h"
#include "base/Log.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_messagebox.h>

namespace openhorizon {

bool showErrorDialog(const std::string& title, const std::string& message, SDL_Window* parent)
{
    if (!errorDialogsAllowed()) {
        return false;
    }
    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), message.c_str(), parent)) {
        logWarning("error dialog: {}", SDL_GetError());
        return false;
    }
    return true;
}

} // namespace openhorizon
