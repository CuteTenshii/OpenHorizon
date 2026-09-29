#pragma once

// OpenGL 4.5 core entry points, loaded at run time through the windowing
// layer. Only the functions the renderer uses are listed, so a missing one
// is reported by name when the context is created.

// glcorearb.h declares prototypes when GL_GLEXT_PROTOTYPES is defined at
// all, which would clash with the loaded pointers of the same names.
#undef GL_GLEXT_PROTOTYPES
#include <GL/glcorearb.h>

#define FH_GL_FUNCTIONS(X)                                                                                             \
    X(PFNGLGETSTRINGPROC, glGetString)                                                                                 \
    X(PFNGLGETSTRINGIPROC, glGetStringi)                                                                               \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)                                                                             \
    X(PFNGLGETERRORPROC, glGetError)                                                                                   \
    X(PFNGLENABLEPROC, glEnable)                                                                                       \
    X(PFNGLDISABLEPROC, glDisable)                                                                                     \
    X(PFNGLENABLEIPROC, glEnablei)                                                                                     \
    X(PFNGLDISABLEIPROC, glDisablei)                                                                                   \
    X(PFNGLBLENDFUNCPROC, glBlendFunc)                                                                                 \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                                                                 \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation)                                                                         \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                                                                 \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                                                                 \
    X(PFNGLCOLORMASKPROC, glColorMask)                                                                                 \
    X(PFNGLCULLFACEPROC, glCullFace)                                                                                   \
    X(PFNGLFRONTFACEPROC, glFrontFace)                                                                                 \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset)                                                                         \
    X(PFNGLPOLYGONMODEPROC, glPolygonMode)                                                                             \
    X(PFNGLVIEWPORTPROC, glViewport)                                                                                   \
    X(PFNGLSCISSORPROC, glScissor)                                                                                     \
    X(PFNGLCLEARPROC, glClear)                                                                                         \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                                                               \
    X(PFNGLCLEARDEPTHPROC, glClearDepth)                                                                               \
    X(PFNGLCLIPCONTROLPROC, glClipControl)                                                                             \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                                                             \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                                                               \
    X(PFNGLFINISHPROC, glFinish)                                                                                       \
    X(PFNGLBEGINQUERYPROC, glBeginQuery)                                                                               \
    X(PFNGLENDQUERYPROC, glEndQuery)                                                                                   \
    X(PFNGLGETQUERYOBJECTIVPROC, glGetQueryObjectiv)                                                                   \
    X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v)                                                             \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback)                                                           \
    X(PFNGLDEBUGMESSAGECONTROLPROC, glDebugMessageControl)                                                             \
    X(PFNGLCREATEBUFFERSPROC, glCreateBuffers)                                                                         \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                                         \
    X(PFNGLNAMEDBUFFERSTORAGEPROC, glNamedBufferStorage)                                                               \
    X(PFNGLNAMEDBUFFERDATAPROC, glNamedBufferData)                                                                     \
    X(PFNGLNAMEDBUFFERSUBDATAPROC, glNamedBufferSubData)                                                               \
    X(PFNGLMAPNAMEDBUFFERRANGEPROC, glMapNamedBufferRange)                                                             \
    X(PFNGLUNMAPNAMEDBUFFERPROC, glUnmapNamedBuffer)                                                                   \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                                                                       \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange)                                                                     \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                                               \
    X(PFNGLCREATEVERTEXARRAYSPROC, glCreateVertexArrays)                                                               \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                                                               \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                                     \
    X(PFNGLVERTEXARRAYVERTEXBUFFERPROC, glVertexArrayVertexBuffer)                                                     \
    X(PFNGLVERTEXARRAYELEMENTBUFFERPROC, glVertexArrayElementBuffer)                                                   \
    X(PFNGLENABLEVERTEXARRAYATTRIBPROC, glEnableVertexArrayAttrib)                                                     \
    X(PFNGLVERTEXARRAYATTRIBFORMATPROC, glVertexArrayAttribFormat)                                                     \
    X(PFNGLVERTEXARRAYATTRIBIFORMATPROC, glVertexArrayAttribIFormat)                                                   \
    X(PFNGLVERTEXARRAYATTRIBBINDINGPROC, glVertexArrayAttribBinding)                                                   \
    X(PFNGLVERTEXARRAYBINDINGDIVISORPROC, glVertexArrayBindingDivisor)                                                 \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                                               \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements)                                                                           \
    X(PFNGLDRAWELEMENTSBASEVERTEXPROC, glDrawElementsBaseVertex)                                                       \
    X(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced)                                                         \
    X(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced)                                                             \
    X(PFNGLMULTIDRAWELEMENTSINDIRECTPROC, glMultiDrawElementsIndirect)                                                 \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                                           \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                                           \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                                           \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                                         \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                                             \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                                                   \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                                         \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                                                         \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                                           \
    X(PFNGLDETACHSHADERPROC, glDetachShader)                                                                           \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                                             \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                                           \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                                                 \
    X(PFNGLPROGRAMPARAMETERIPROC, glProgramParameteri)                                                                 \
    X(PFNGLGETPROGRAMBINARYPROC, glGetProgramBinary)                                                                   \
    X(PFNGLPROGRAMBINARYPROC, glProgramBinary)                                                                         \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                                               \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                                               \
    X(PFNGLPROGRAMUNIFORM1IPROC, glProgramUniform1i)                                                                   \
    X(PFNGLPROGRAMUNIFORM1FPROC, glProgramUniform1f)                                                                   \
    X(PFNGLPROGRAMUNIFORM2FPROC, glProgramUniform2f)                                                                   \
    X(PFNGLPROGRAMUNIFORM3FPROC, glProgramUniform3f)                                                                   \
    X(PFNGLPROGRAMUNIFORM4FPROC, glProgramUniform4f)                                                                   \
    X(PFNGLPROGRAMUNIFORM3FVPROC, glProgramUniform3fv)                                                                 \
    X(PFNGLPROGRAMUNIFORM4FVPROC, glProgramUniform4fv)                                                                 \
    X(PFNGLPROGRAMUNIFORMMATRIX4FVPROC, glProgramUniformMatrix4fv)                                                     \
    X(PFNGLPROGRAMUNIFORMMATRIX3FVPROC, glProgramUniformMatrix3fv)                                                     \
    X(PFNGLCREATETEXTURESPROC, glCreateTextures)                                                                       \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                                                       \
    X(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D)                                                                   \
    X(PFNGLTEXTURESTORAGE3DPROC, glTextureStorage3D)                                                                   \
    X(PFNGLTEXTURESUBIMAGE2DPROC, glTextureSubImage2D)                                                                 \
    X(PFNGLTEXTURESUBIMAGE3DPROC, glTextureSubImage3D)                                                                 \
    X(PFNGLCOMPRESSEDTEXTURESUBIMAGE2DPROC, glCompressedTextureSubImage2D)                                             \
    X(PFNGLTEXTUREPARAMETERIPROC, glTextureParameteri)                                                                 \
    X(PFNGLTEXTUREPARAMETERFPROC, glTextureParameterf)                                                                 \
    X(PFNGLTEXTUREPARAMETERFVPROC, glTextureParameterfv)                                                               \
    X(PFNGLGENERATETEXTUREMIPMAPPROC, glGenerateTextureMipmap)                                                         \
    X(PFNGLBINDTEXTUREUNITPROC, glBindTextureUnit)                                                                     \
    X(PFNGLCREATESAMPLERSPROC, glCreateSamplers)                                                                       \
    X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers)                                                                       \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)                                                                 \
    X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf)                                                                 \
    X(PFNGLSAMPLERPARAMETERFVPROC, glSamplerParameterfv)                                                               \
    X(PFNGLBINDSAMPLERPROC, glBindSampler)                                                                             \
    X(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers)                                                               \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                                                               \
    X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture)                                                     \
    X(PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC, glNamedFramebufferTextureLayer)                                           \
    X(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERSPROC, glNamedFramebufferDrawBuffers)                                             \
    X(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERPROC, glNamedFramebufferDrawBuffer)                                               \
    X(PFNGLNAMEDFRAMEBUFFERREADBUFFERPROC, glNamedFramebufferReadBuffer)                                               \
    X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, glCheckNamedFramebufferStatus)                                             \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                                     \
    X(PFNGLBLITNAMEDFRAMEBUFFERPROC, glBlitNamedFramebuffer)                                                           \
    X(PFNGLCLEARNAMEDFRAMEBUFFERFVPROC, glClearNamedFramebufferfv)                                                     \
    X(PFNGLCLEARNAMEDFRAMEBUFFERFIPROC, glClearNamedFramebufferfi)                                                     \
    X(PFNGLGENQUERIESPROC, glGenQueries)                                                                               \
    X(PFNGLDELETEQUERIESPROC, glDeleteQueries)                                                                         \
    X(PFNGLFENCESYNCPROC, glFenceSync)                                                                                 \
    X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync)                                                                       \
    X(PFNGLDELETESYNCPROC, glDeleteSync)

// Extension entry points the renderer uses when the driver has them; they
// stay null otherwise, so callers check before calling.
#define FH_GL_OPTIONAL_FUNCTIONS(X)                                                                                    \
    X(PFNGLMAXSHADERCOMPILERTHREADSKHRPROC, glMaxShaderCompilerThreadsKHR)                                             \
    X(PFNGLMAXSHADERCOMPILERTHREADSARBPROC, glMaxShaderCompilerThreadsARB)

namespace fh::gl {

#define FH_GL_DECLARE(type, name) extern type name;
FH_GL_FUNCTIONS(FH_GL_DECLARE)
FH_GL_OPTIONAL_FUNCTIONS(FH_GL_DECLARE)
#undef FH_GL_DECLARE

using ProcLoader = void* (*)(const char* name);

/// Loads every listed entry point. Returns the name of the first required
/// one the driver lacks, or nullptr when all were found; optional ones it
/// lacks stay null.
const char* load(ProcLoader loader);

} // namespace fh::gl

// Renderer code calls the unqualified names.
using namespace fh::gl;
