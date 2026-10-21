#pragma once
// OpenGL ES 3.x entry points. On Android these come from libGLESv3; on the Linux headless
// harness from Mesa's libGLESv2 (which exports the GLES 3 API).
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

namespace gvr {
// Returns false and logs if a GL error is pending. `where` names the call site.
bool checkGl(const char* where);
}  // namespace gvr
