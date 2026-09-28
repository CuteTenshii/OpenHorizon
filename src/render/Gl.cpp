#include "Gl.h"

namespace fh::gl {

#define FH_GL_DEFINE(type, name) type name = nullptr;
FH_GL_FUNCTIONS(FH_GL_DEFINE)
#undef FH_GL_DEFINE

const char* load(ProcLoader loader)
{
// `type` is a type in a cast, which parentheses would break.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define FH_GL_LOAD(type, name)                                                                                         \
    name = reinterpret_cast<type>(loader(#name));                                                                      \
    if (name == nullptr) {                                                                                             \
        return #name;                                                                                                  \
    }
    // NOLINTEND(bugprone-macro-parentheses)
    FH_GL_FUNCTIONS(FH_GL_LOAD)
#undef FH_GL_LOAD
    return nullptr;
}

} // namespace fh::gl
