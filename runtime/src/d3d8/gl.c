#include "../rt.h"
#include "gl.h"
#include <SDL3/SDL.h>

#define X(ret, name, args) PFN_gl##name p_gl##name;
GL_FUNCS(X)
#undef X

bool gl_load(void) {
    bool ok = true;
#define X(ret, name, args)                                                         \
    p_gl##name = (PFN_gl##name)SDL_GL_GetProcAddress("gl" #name);                  \
    if (!p_gl##name) {                                                             \
        LOGE("missing GL function gl" #name);                                      \
        ok = false;                                                                \
    }
    GL_FUNCS(X)
#undef X
    return ok;
}
