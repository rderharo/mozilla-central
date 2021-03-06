/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 *   Mozilla Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2009
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Vladimir Vukicevic <vladimir@pobox.com> (original author)
 *   Mark Steele <mwsteele@gmail.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "WebGLContext.h"

#include "mozilla/Preferences.h"

#include "CheckedInt.h"

#include "jstypedarray.h"

#if defined(USE_ANGLE)
#include "angle/ShaderLang.h"
#endif

#include <algorithm>

using namespace mozilla;

/*
 * Pull data out of the program, post-linking
 */
bool
WebGLProgram::UpdateInfo()
{
    mIdentifierMap = nsnull;
    mIdentifierReverseMap = nsnull;
    mUniformInfoMap = nsnull;

    mAttribMaxNameLength = 0;

    for (size_t i = 0; i < mAttachedShaders.Length(); i++)
        mAttribMaxNameLength = NS_MAX(mAttribMaxNameLength, mAttachedShaders[i]->mAttribMaxNameLength);

    GLint attribCount;
    mContext->gl->fGetProgramiv(mGLName, LOCAL_GL_ACTIVE_ATTRIBUTES, &attribCount);

    mAttribsInUse.resize(mContext->mGLMaxVertexAttribs);
    std::fill(mAttribsInUse.begin(), mAttribsInUse.end(), false);

    nsAutoArrayPtr<char> nameBuf(new char[mAttribMaxNameLength]);

    for (int i = 0; i < attribCount; ++i) {
        GLint attrnamelen;
        GLint attrsize;
        GLenum attrtype;
        mContext->gl->fGetActiveAttrib(mGLName, i, mAttribMaxNameLength, &attrnamelen, &attrsize, &attrtype, nameBuf);
        if (attrnamelen > 0) {
            GLint loc = mContext->gl->fGetAttribLocation(mGLName, nameBuf);
            NS_ABORT_IF_FALSE(loc >= 0, "major oops in managing the attributes of a WebGL program");
            if (loc < mContext->mGLMaxVertexAttribs) {
                mAttribsInUse[loc] = true;
            } else {
                mContext->ErrorInvalidOperation("program exceeds MAX_VERTEX_ATTRIBS");
                return false;
            }
        }
    }

    return true;
}

/*
 * Verify that state is consistent for drawing, and compute max number of elements (maxAllowedCount)
 * that will be legal to be read from bound VBOs.
 */

bool
WebGLContext::ValidateBuffers(PRInt32 *maxAllowedCount, const char *info)
{
#ifdef DEBUG
    GLint currentProgram = 0;
    MakeContextCurrent();
    gl->fGetIntegerv(LOCAL_GL_CURRENT_PROGRAM, &currentProgram);
    NS_ASSERTION(GLuint(currentProgram) == mCurrentProgram->GLName(),
                 "WebGL: current program doesn't agree with GL state");
    if (GLuint(currentProgram) != mCurrentProgram->GLName())
        return false;
#endif

    *maxAllowedCount = -1;

    PRUint32 attribs = mAttribBuffers.Length();
    for (PRUint32 i = 0; i < attribs; ++i) {
        const WebGLVertexAttribData& vd = mAttribBuffers[i];

        // If the attrib array isn't enabled, there's nothing to check;
        // it's a static value.
        if (!vd.enabled)
            continue;

        if (vd.buf == nsnull) {
            ErrorInvalidOperation("%s: no VBO bound to enabled vertex attrib index %d!", info, i);
            return false;
        }

        // If the attrib is not in use, then we don't have to validate
        // it, just need to make sure that the binding is non-null.
        if (!mCurrentProgram->IsAttribInUse(i))
            continue;

        // the base offset
        CheckedInt32 checked_byteLength
          = CheckedInt32(vd.buf->ByteLength()) - vd.byteOffset;
        CheckedInt32 checked_sizeOfLastElement
          = CheckedInt32(vd.componentSize()) * vd.size;

        if (!checked_byteLength.valid() ||
            !checked_sizeOfLastElement.valid())
        {
          ErrorInvalidOperation("%s: integer overflow occured while checking vertex attrib %d", info, i);
          return false;
        }

        if (checked_byteLength.value() < checked_sizeOfLastElement.value()) {
          *maxAllowedCount = 0;
        } else {
          CheckedInt32 checked_maxAllowedCount
            = ((checked_byteLength - checked_sizeOfLastElement) / vd.actualStride()) + 1;

          if (!checked_maxAllowedCount.valid()) {
            ErrorInvalidOperation("%s: integer overflow occured while checking vertex attrib %d", info, i);
            return false;
          }

          if (*maxAllowedCount == -1 || *maxAllowedCount > checked_maxAllowedCount.value())
              *maxAllowedCount = checked_maxAllowedCount.value();
        }
    }

    return true;
}

bool WebGLContext::ValidateCapabilityEnum(WebGLenum cap, const char *info)
{
    switch (cap) {
        case LOCAL_GL_BLEND:
        case LOCAL_GL_CULL_FACE:
        case LOCAL_GL_DEPTH_TEST:
        case LOCAL_GL_DITHER:
        case LOCAL_GL_POLYGON_OFFSET_FILL:
        case LOCAL_GL_SAMPLE_ALPHA_TO_COVERAGE:
        case LOCAL_GL_SAMPLE_COVERAGE:
        case LOCAL_GL_SCISSOR_TEST:
        case LOCAL_GL_STENCIL_TEST:
            return true;
        default:
            ErrorInvalidEnumInfo(info, cap);
            return false;
    }
}

bool WebGLContext::ValidateBlendEquationEnum(WebGLenum mode, const char *info)
{
    switch (mode) {
        case LOCAL_GL_FUNC_ADD:
        case LOCAL_GL_FUNC_SUBTRACT:
        case LOCAL_GL_FUNC_REVERSE_SUBTRACT:
            return true;
        default:
            ErrorInvalidEnumInfo(info, mode);
            return false;
    }
}

bool WebGLContext::ValidateBlendFuncDstEnum(WebGLenum factor, const char *info)
{
    switch (factor) {
        case LOCAL_GL_ZERO:
        case LOCAL_GL_ONE:
        case LOCAL_GL_SRC_COLOR:
        case LOCAL_GL_ONE_MINUS_SRC_COLOR:
        case LOCAL_GL_DST_COLOR:
        case LOCAL_GL_ONE_MINUS_DST_COLOR:
        case LOCAL_GL_SRC_ALPHA:
        case LOCAL_GL_ONE_MINUS_SRC_ALPHA:
        case LOCAL_GL_DST_ALPHA:
        case LOCAL_GL_ONE_MINUS_DST_ALPHA:
        case LOCAL_GL_CONSTANT_COLOR:
        case LOCAL_GL_ONE_MINUS_CONSTANT_COLOR:
        case LOCAL_GL_CONSTANT_ALPHA:
        case LOCAL_GL_ONE_MINUS_CONSTANT_ALPHA:
            return true;
        default:
            ErrorInvalidEnumInfo(info, factor);
            return false;
    }
}

bool WebGLContext::ValidateBlendFuncSrcEnum(WebGLenum factor, const char *info)
{
    if (factor == LOCAL_GL_SRC_ALPHA_SATURATE)
        return true;
    else
        return ValidateBlendFuncDstEnum(factor, info);
}

bool WebGLContext::ValidateBlendFuncEnumsCompatibility(WebGLenum sfactor, WebGLenum dfactor, const char *info)
{
    bool sfactorIsConstantColor = sfactor == LOCAL_GL_CONSTANT_COLOR ||
                                    sfactor == LOCAL_GL_ONE_MINUS_CONSTANT_COLOR;
    bool sfactorIsConstantAlpha = sfactor == LOCAL_GL_CONSTANT_ALPHA ||
                                    sfactor == LOCAL_GL_ONE_MINUS_CONSTANT_ALPHA;
    bool dfactorIsConstantColor = dfactor == LOCAL_GL_CONSTANT_COLOR ||
                                    dfactor == LOCAL_GL_ONE_MINUS_CONSTANT_COLOR;
    bool dfactorIsConstantAlpha = dfactor == LOCAL_GL_CONSTANT_ALPHA ||
                                    dfactor == LOCAL_GL_ONE_MINUS_CONSTANT_ALPHA;
    if ( (sfactorIsConstantColor && dfactorIsConstantAlpha) ||
         (dfactorIsConstantColor && sfactorIsConstantAlpha) ) {
        ErrorInvalidOperation("%s are mutually incompatible, see section 6.8 in the WebGL 1.0 spec", info);
        return false;
    } else {
        return true;
    }
}

bool WebGLContext::ValidateTextureTargetEnum(WebGLenum target, const char *info)
{
    switch (target) {
        case LOCAL_GL_TEXTURE_2D:
        case LOCAL_GL_TEXTURE_CUBE_MAP:
            return true;
        default:
            ErrorInvalidEnumInfo(info, target);
            return false;
    }
}

bool WebGLContext::ValidateComparisonEnum(WebGLenum target, const char *info)
{
    switch (target) {
        case LOCAL_GL_NEVER:
        case LOCAL_GL_LESS:
        case LOCAL_GL_LEQUAL:
        case LOCAL_GL_GREATER:
        case LOCAL_GL_GEQUAL:
        case LOCAL_GL_EQUAL:
        case LOCAL_GL_NOTEQUAL:
        case LOCAL_GL_ALWAYS:
            return true;
        default:
            ErrorInvalidEnumInfo(info, target);
            return false;
    }
}

bool WebGLContext::ValidateStencilOpEnum(WebGLenum action, const char *info)
{
    switch (action) {
        case LOCAL_GL_KEEP:
        case LOCAL_GL_ZERO:
        case LOCAL_GL_REPLACE:
        case LOCAL_GL_INCR:
        case LOCAL_GL_INCR_WRAP:
        case LOCAL_GL_DECR:
        case LOCAL_GL_DECR_WRAP:
        case LOCAL_GL_INVERT:
            return true;
        default:
            ErrorInvalidEnumInfo(info, action);
            return false;
    }
}

bool WebGLContext::ValidateFaceEnum(WebGLenum face, const char *info)
{
    switch (face) {
        case LOCAL_GL_FRONT:
        case LOCAL_GL_BACK:
        case LOCAL_GL_FRONT_AND_BACK:
            return true;
        default:
            ErrorInvalidEnumInfo(info, face);
            return false;
    }
}

bool WebGLContext::ValidateBufferUsageEnum(WebGLenum target, const char *info)
{
    switch (target) {
        case LOCAL_GL_STREAM_DRAW:
        case LOCAL_GL_STATIC_DRAW:
        case LOCAL_GL_DYNAMIC_DRAW:
            return true;
        default:
            ErrorInvalidEnumInfo(info, target);
            return false;
    }
}

bool WebGLContext::ValidateDrawModeEnum(WebGLenum mode, const char *info)
{
    switch (mode) {
        case LOCAL_GL_TRIANGLES:
        case LOCAL_GL_TRIANGLE_STRIP:
        case LOCAL_GL_TRIANGLE_FAN:
        case LOCAL_GL_POINTS:
        case LOCAL_GL_LINE_STRIP:
        case LOCAL_GL_LINE_LOOP:
        case LOCAL_GL_LINES:
            return true;
        default:
            ErrorInvalidEnumInfo(info, mode);
            return false;
    }
}

bool WebGLContext::ValidateGLSLVariableName(const nsAString& name, const char *info)
{
    const PRUint32 maxSize = 256;
    if (name.Length() > maxSize) {
        ErrorInvalidValue("%s: identifier is %d characters long, exceeds the maximum allowed length of %d characters",
                          info, name.Length(), maxSize);
        return false;
    }

    if (!ValidateGLSLString(name, info)) {
        return false;
    }

    return true;
}

bool WebGLContext::ValidateGLSLString(const nsAString& string, const char *info)
{
    for (PRUint32 i = 0; i < string.Length(); ++i) {
        if (!ValidateGLSLCharacter(string.CharAt(i))) {
             ErrorInvalidValue("%s: string contains the illegal character '%d'", info, string.CharAt(i));
             return false;
        }
    }

    return true;
}

PRUint32 WebGLContext::GetTexelSize(WebGLenum format, WebGLenum type)
{
    if (type == LOCAL_GL_UNSIGNED_BYTE || type == LOCAL_GL_FLOAT) {
        int multiplier = type == LOCAL_GL_FLOAT ? 4 : 1;
        switch (format) {
            case LOCAL_GL_ALPHA:
            case LOCAL_GL_LUMINANCE:
                return 1 * multiplier;
            case LOCAL_GL_LUMINANCE_ALPHA:
                return 2 * multiplier;
            case LOCAL_GL_RGB:
                return 3 * multiplier;
            case LOCAL_GL_RGBA:
                return 4 * multiplier;
            default:
                break;
        }
    } else if (type == LOCAL_GL_UNSIGNED_SHORT_4_4_4_4 ||
               type == LOCAL_GL_UNSIGNED_SHORT_5_5_5_1 ||
               type == LOCAL_GL_UNSIGNED_SHORT_5_6_5)
    {
        return 2;
    }

    NS_ABORT();
    return 0;
}

bool WebGLContext::ValidateTexFormatAndType(WebGLenum format, WebGLenum type, int jsArrayType,
                                              PRUint32 *texelSize, const char *info)
{
    if (type == LOCAL_GL_UNSIGNED_BYTE ||
        (IsExtensionEnabled(WebGL_OES_texture_float) && type == LOCAL_GL_FLOAT))
    {
        if (jsArrayType != -1) {
            if ((type == LOCAL_GL_UNSIGNED_BYTE && jsArrayType != js::TypedArray::TYPE_UINT8) ||
                (type == LOCAL_GL_FLOAT && jsArrayType != js::TypedArray::TYPE_FLOAT32))
            {
                ErrorInvalidOperation("%s: invalid typed array type for given format", info);
                return false;
            }
        }

        int texMultiplier = type == LOCAL_GL_FLOAT ? 4 : 1;
        switch (format) {
            case LOCAL_GL_ALPHA:
            case LOCAL_GL_LUMINANCE:
                *texelSize = 1 * texMultiplier;
                return true;
            case LOCAL_GL_LUMINANCE_ALPHA:
                *texelSize = 2 * texMultiplier;
                return true;
            case LOCAL_GL_RGB:
                *texelSize = 3 * texMultiplier;
                return true;
            case LOCAL_GL_RGBA:
                *texelSize = 4 * texMultiplier;
                return true;
            default:
                break;
        }

        ErrorInvalidEnum("%s: invalid format 0x%x", info, format);
        return false;
    }

    switch (type) {
        case LOCAL_GL_UNSIGNED_SHORT_4_4_4_4:
        case LOCAL_GL_UNSIGNED_SHORT_5_5_5_1:
            if (jsArrayType != -1 && jsArrayType != js::TypedArray::TYPE_UINT16) {
                ErrorInvalidOperation("%s: invalid typed array type for given format", info);
                return false;
            }

            if (format == LOCAL_GL_RGBA) {
                *texelSize = 2;
                return true;
            }
            ErrorInvalidOperation("%s: mutually incompatible format and type", info);
            return false;

        case LOCAL_GL_UNSIGNED_SHORT_5_6_5:
            if (jsArrayType != -1 && jsArrayType != js::TypedArray::TYPE_UINT16) {
                ErrorInvalidOperation("%s: invalid typed array type for given format", info);
                return false;
            }

            if (format == LOCAL_GL_RGB) {
                *texelSize = 2;
                return true;
            }
            ErrorInvalidOperation("%s: mutually incompatible format and type", info);
            return false;

        default:
            break;
        }

    ErrorInvalidEnum("%s: invalid type 0x%x", info, type);
    return false;
}

bool WebGLContext::ValidateAttribIndex(WebGLuint index, const char *info)
{
    if (index >= mAttribBuffers.Length()) {
        if (index == WebGLuint(-1)) {
             ErrorInvalidValue("%s: index -1 is invalid. That probably comes from a getAttribLocation() call, "
                               "where this return value -1 means that the passed name didn't correspond to an active attribute in "
                               "the specified program.", info);
        } else {
             ErrorInvalidValue("%s: index %d is out of range", info, index);
        }
        return false;
    } else {
        return true;
    }
}

bool WebGLContext::ValidateStencilParamsForDrawCall()
{
  const char *msg = "%s set different front and back stencil %s. Drawing in this configuration is not allowed.";
  if (mStencilRefFront != mStencilRefBack) {
      ErrorInvalidOperation(msg, "stencilFuncSeparate", "reference values");
      return false;
  }
  if (mStencilValueMaskFront != mStencilValueMaskBack) {
      ErrorInvalidOperation(msg, "stencilFuncSeparate", "value masks");
      return false;
  }
  if (mStencilWriteMaskFront != mStencilWriteMaskBack) {
      ErrorInvalidOperation(msg, "stencilMaskSeparate", "write masks");
      return false;
  }
  return true;
}

bool
WebGLContext::InitAndValidateGL()
{
    if (!gl) return false;

    GLenum error = gl->fGetError();
    if (error != LOCAL_GL_NO_ERROR) {
        LogMessage("GL error 0x%x occurred during OpenGL context initialization, before WebGL initialization!", error);
        return false;
    }

    mMinCapability = Preferences::GetBool("webgl.min_capability_mode", false);
    mDisableExtensions = Preferences::GetBool("webgl.disable-extensions", false);

    mActiveTexture = 0;
    mWebGLError = LOCAL_GL_NO_ERROR;

    mAttribBuffers.Clear();

    mBound2DTextures.Clear();
    mBoundCubeMapTextures.Clear();

    mBoundArrayBuffer = nsnull;
    mBoundElementArrayBuffer = nsnull;
    mCurrentProgram = nsnull;

    mBoundFramebuffer = nsnull;
    mBoundRenderbuffer = nsnull;

    MakeContextCurrent();

    // on desktop OpenGL, we always keep vertex attrib 0 array enabled
    if (!gl->IsGLES2()) {
        gl->fEnableVertexAttribArray(0);
    }

    if (MinCapabilityMode()) {
        mGLMaxVertexAttribs = MINVALUE_GL_MAX_VERTEX_ATTRIBS;
    } else {
        gl->fGetIntegerv(LOCAL_GL_MAX_VERTEX_ATTRIBS, &mGLMaxVertexAttribs);
    }
    if (mGLMaxVertexAttribs < 8) {
        LogMessage("GL_MAX_VERTEX_ATTRIBS: %d is < 8!", mGLMaxVertexAttribs);
        return false;
    }

    mAttribBuffers.SetLength(mGLMaxVertexAttribs);

    // Note: GL_MAX_TEXTURE_UNITS is fixed at 4 for most desktop hardware,
    // even though the hardware supports much more.  The
    // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS value is the accurate value.
    if (MinCapabilityMode()) {
        mGLMaxTextureUnits = MINVALUE_GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS;
    } else {
        gl->fGetIntegerv(LOCAL_GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &mGLMaxTextureUnits);
    }
    if (mGLMaxTextureUnits < 8) {
        LogMessage("GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: %d is < 8!", mGLMaxTextureUnits);
        return false;
    }

    mBound2DTextures.SetLength(mGLMaxTextureUnits);
    mBoundCubeMapTextures.SetLength(mGLMaxTextureUnits);

    if (MinCapabilityMode()) {
        mGLMaxTextureSize = MINVALUE_GL_MAX_TEXTURE_SIZE;
        mGLMaxCubeMapTextureSize = MINVALUE_GL_MAX_CUBE_MAP_TEXTURE_SIZE;
        mGLMaxTextureImageUnits = MINVALUE_GL_MAX_TEXTURE_IMAGE_UNITS;
        mGLMaxVertexTextureImageUnits = MINVALUE_GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS;
    } else {
        gl->fGetIntegerv(LOCAL_GL_MAX_TEXTURE_SIZE, &mGLMaxTextureSize);
        gl->fGetIntegerv(LOCAL_GL_MAX_CUBE_MAP_TEXTURE_SIZE, &mGLMaxCubeMapTextureSize);
        gl->fGetIntegerv(LOCAL_GL_MAX_TEXTURE_IMAGE_UNITS, &mGLMaxTextureImageUnits);
        gl->fGetIntegerv(LOCAL_GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &mGLMaxVertexTextureImageUnits);
    }

    if (MinCapabilityMode()) {
        mGLMaxFragmentUniformVectors = MINVALUE_GL_MAX_FRAGMENT_UNIFORM_VECTORS;
        mGLMaxVertexUniformVectors = MINVALUE_GL_MAX_VERTEX_UNIFORM_VECTORS;
        mGLMaxVaryingVectors = MINVALUE_GL_MAX_VARYING_VECTORS;
    } else {
        if (gl->HasES2Compatibility()) {
            gl->fGetIntegerv(LOCAL_GL_MAX_FRAGMENT_UNIFORM_VECTORS, &mGLMaxFragmentUniformVectors);
            gl->fGetIntegerv(LOCAL_GL_MAX_VERTEX_UNIFORM_VECTORS, &mGLMaxVertexUniformVectors);
            gl->fGetIntegerv(LOCAL_GL_MAX_VARYING_VECTORS, &mGLMaxVaryingVectors);
        } else {
            gl->fGetIntegerv(LOCAL_GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &mGLMaxFragmentUniformVectors);
            mGLMaxFragmentUniformVectors /= 4;
            gl->fGetIntegerv(LOCAL_GL_MAX_VERTEX_UNIFORM_COMPONENTS, &mGLMaxVertexUniformVectors);
            mGLMaxVertexUniformVectors /= 4;

            // we are now going to try to read GL_MAX_VERTEX_OUTPUT_COMPONENTS and GL_MAX_FRAGMENT_INPUT_COMPONENTS,
            // however these constants only entered the OpenGL standard at OpenGL 3.2. So we will try reading,
            // and check OpenGL error for INVALID_ENUM.

            // before we start, we check that no error already occurred, to prevent hiding it in our subsequent error handling
            error = gl->GetAndClearError();
            if (error != LOCAL_GL_NO_ERROR) {
                LogMessage("GL error 0x%x occurred during WebGL context initialization!", error);
                return false;
            }

            // On the public_webgl list, "problematic GetParameter pnames" thread, the following formula was given:
            //   mGLMaxVaryingVectors = min (GL_MAX_VERTEX_OUTPUT_COMPONENTS, GL_MAX_FRAGMENT_INPUT_COMPONENTS) / 4
            GLint maxVertexOutputComponents,
                  minFragmentInputComponents;
            gl->fGetIntegerv(LOCAL_GL_MAX_VERTEX_OUTPUT_COMPONENTS, &maxVertexOutputComponents);
            gl->fGetIntegerv(LOCAL_GL_MAX_FRAGMENT_INPUT_COMPONENTS, &minFragmentInputComponents);

            error = gl->GetAndClearError();
            switch (error) {
                case LOCAL_GL_NO_ERROR:
                    mGLMaxVaryingVectors = NS_MIN(maxVertexOutputComponents, minFragmentInputComponents) / 4;
                    break;
                case LOCAL_GL_INVALID_ENUM:
                    mGLMaxVaryingVectors = 16; // = 64/4, 64 is the min value for maxVertexOutputComponents in OpenGL 3.2 spec
                    break;
                default:
                    LogMessage("GL error 0x%x occurred during WebGL context initialization!", error);
                    return false;
            }   
        }
    }

    // Always 1 for GLES2
    mMaxFramebufferColorAttachments = 1;

    if (!gl->IsGLES2()) {
        // gl_PointSize is always available in ES2 GLSL, but has to be
        // specifically enabled on desktop GLSL.
        gl->fEnable(LOCAL_GL_VERTEX_PROGRAM_POINT_SIZE);

        // we don't do the following glEnable(GL_POINT_SPRITE) on ATI cards on Windows, because bug 602183 shows that it causes
        // crashes in the ATI/Windows driver; and point sprites on ATI seem like a lost cause anyway, see
        //    http://www.gamedev.net/community/forums/topic.asp?topic_id=525643
        // Also, if the ATI/Windows driver implements a recent GL spec version, this shouldn't be needed anyway.
#ifdef XP_WIN
        if (!(gl->WorkAroundDriverBugs() &&
              gl->Vendor() == gl::GLContext::VendorATI))
#else
        if (true)
#endif
        {
            // gl_PointCoord is always available in ES2 GLSL and in newer desktop GLSL versions, but apparently
            // not in OpenGL 2 and apparently not (due to a driver bug) on certain NVIDIA setups. See:
            //   http://www.opengl.org/discussion_boards/ubbthreads.php?ubb=showflat&Number=261472
            gl->fEnable(LOCAL_GL_POINT_SPRITE);
        }
    }

    // Check the shader validator pref
    NS_ENSURE_TRUE(Preferences::GetRootBranch(), false);

    mShaderValidation =
        Preferences::GetBool("webgl.shader_validator", mShaderValidation);

#if defined(USE_ANGLE)
    // initialize shader translator
    if (mShaderValidation) {
        if (!ShInitialize()) {
            LogMessage("GLSL translator initialization failed!");
            return false;
        }
    }
#endif

    // notice that the point of calling GetAndClearError here is not only to check for error,
    // it is also to reset the error flags so that a subsequent WebGL getError call will give the correct result.
    error = gl->GetAndClearError();
    if (error != LOCAL_GL_NO_ERROR) {
        LogMessage("GL error 0x%x occurred during WebGL context initialization!", error);
        return false;
    }

    return true;
}
