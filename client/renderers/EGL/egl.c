/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "interface/renderer.h"

#include "common/debug.h"
#include "common/KVMFR.h"
#include "common/option.h"
#include "common/sysinfo.h"
#include "common/rects.h"
#include "common/time.h"
#include "common/locking.h"
#include "app.h"
#include "util.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include "cimgui.h"
#include "generator/output/cimgui_impl.h"

#include <math.h>
#include <string.h>

#include "app.h"
#include "egl_dynprocs.h"
#include "model.h"
#include "shader.h"
#include "damage.h"
#include "desktop.h"
#include "cursor.h"
#include "splash.h"
#include "postprocess.h"
#include "util.h"

#define SPLASH_FADE_TIME 1000000
#define MAX_BUFFER_AGE       3
#define DESKTOP_DAMAGE_COUNT 4
#define MAX_ACCUMULATED_DAMAGE ((KVMFR_MAX_DAMAGE_RECTS + MAX_OVERLAY_RECTS + 2) * MAX_BUFFER_AGE)
#define IDX_AGO(counter, i, total) ((counter) + (total) - i) % (total)

struct Options
{
  bool vsync;
  bool doubleBuffer;
};

struct Inst
{
  LG_Renderer base;

  bool dmaSupport;
  LG_RendererParams params;
  struct Options    opt;

  EGLNativeWindowType  nativeWind;
  EGLDisplay           display;
  EGLConfig            configs;
  EGLSurface           surface;
  EGLContext           context, frameContext;

  EGL_Desktop     * desktop; // the desktop
  EGL_Cursor      * cursor;  // the mouse cursor
  EGL_Splash      * splash;  // the splash screen
  EGL_Damage      * damage;  // the damage display
  bool              imgui;   // if imgui was initialized

  LG_RendererFormat    format;
  bool                 formatValid;
  bool                 start;
  uint64_t             waitFadeTime;
  bool                 waitDone;

  int               width, height;
  float             uiScale;
  struct DoubleRect destRect;
  LG_RendererRotate rotate; //client side rotation

  float translateX  , translateY;
  float scaleX      , scaleY;
  float splashRatio;
  float screenScaleX, screenScaleY;

  int viewportWidth, viewportHeight;
  enum EGL_DesktopScaleType scaleType;

  bool  cursorVisible;
  int   cursorX    , cursorY;
  float mouseWidth , mouseHeight;
  float mouseScaleX, mouseScaleY;
  bool  showDamage;

  struct CursorState cursorLast;

  bool                 hadOverlay;
  struct DesktopDamage desktopDamage[DESKTOP_DAMAGE_COUNT];
  unsigned int         desktopDamageIdx;
  LG_Lock              desktopDamageLock;

  bool         hasBufferAge;
  struct Rect  overlayHistory[DESKTOP_DAMAGE_COUNT][MAX_OVERLAY_RECTS + 1];
  int          overlayHistoryCount[DESKTOP_DAMAGE_COUNT];
  unsigned int overlayHistoryIdx;

  RingBuffer importTimings;
  GraphHandle importGraph;
};

static struct Option egl_options[] =
{
  {
    .module       = "egl",
    .name         = "vsync",
    .description  = "Enable vsync",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = false,
  },
  {
    .module       = "egl",
    .name         = "doubleBuffer",
    .description  = "Enable double buffering",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = false
  },
  {
    .module       = "egl",
    .name         = "multisample",
    .description  = "Enable Multisampling",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = true
  },
  {
    .module       = "egl",
    .name         = "nvGainMax",
    .description  = "The maximum night vision gain",
    .type         = OPTION_TYPE_INT,
    .value.x_int  = 1
  },
  {
    .module       = "egl",
    .name         = "nvGain",
    .description  = "The initial night vision gain at startup",
    .type         = OPTION_TYPE_INT,
    .value.x_int  = 0
  },
  {
    .module       = "egl",
    .name         = "cbMode",
    .description  = "Color Blind Mode (0 = Off, 1 = Protanope, 2 = Deuteranope, 3 = Tritanope)",
    .type         = OPTION_TYPE_INT,
    .value.x_int  = 0
  },
  {
    .module       = "egl",
    .name         = "scale",
    .description  = "Set the scale algorithm (0 = auto, 1 = nearest, 2 = linear)",
    .type         = OPTION_TYPE_INT,
    .validator    = egl_desktopScaleValidate,
    .value.x_int  = 0
  },
  {
    .module       = "egl",
    .name         = "debug",
    .description  = "Enable debug output",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = false
  },

  {0}
};

static const char * egl_getName(void)
{
  return "EGL";
}

static void egl_setup(void)
{
  option_register(egl_options);
  egl_postProcessEarlyInit();
}

static bool egl_create(LG_Renderer ** renderer, const LG_RendererParams params,
    bool * needsOpenGL)
{
  // check if EGL is even available
  if (!eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS))
  {
    DEBUG_WARN("EGL is not available");
    return false;
  }

  // create our local storage
  struct Inst * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(*this));
    return false;
  }
  *renderer = &this->base;

  // safe off parameteres and init our default option values
  memcpy(&this->params, &params, sizeof(LG_RendererParams));

  this->opt.vsync        = option_get_bool("egl", "vsync");
  this->opt.doubleBuffer = option_get_bool("egl", "doubleBuffer");

  this->translateX   = 0;
  this->translateY   = 0;
  this->scaleX       = 1.0f;
  this->scaleY       = 1.0f;
  this->screenScaleX = 1.0f;
  this->screenScaleY = 1.0f;
  this->uiScale      = 1.0;

  LG_LOCK_INIT(this->desktopDamageLock);
  this->desktopDamage[0].count = -1;

  this->importTimings = ringbuffer_new(256, sizeof(float));
  this->importGraph   = app_registerGraph("IMPORT", this->importTimings, 0.0f, 5.0f);

  *needsOpenGL = false;
  return true;
}

static bool egl_initialize(LG_Renderer * renderer)
{
  struct Inst * this = UPCAST(struct Inst, renderer);
  DEBUG_INFO("Double buffering is %s", this->opt.doubleBuffer ? "on" : "off");
  return true;
}

static void egl_deinitialize(LG_Renderer * renderer)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  if (this->imgui)
    ImGui_ImplOpenGL3_Shutdown();

  app_unregisterGraph(this->importGraph);
  ringbuffer_free(&this->importTimings);

  egl_desktopFree(&this->desktop);
  egl_cursorFree (&this->cursor);
  egl_splashFree (&this->splash);
  egl_damageFree (&this->damage);

  LG_LOCK_FREE(this->lock);
  LG_LOCK_FREE(this->desktopDamageLock);

  eglMakeCurrent(this->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (this->frameContext)
    eglDestroyContext(this->display, this->frameContext);

  if (this->context)
    eglDestroyContext(this->display, this->context);

  eglTerminate(this->display);

  free(this);
}

static bool egl_supports(LG_Renderer * renderer, LG_RendererSupport flag)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  switch(flag)
  {
    case LG_SUPPORTS_DMABUF:
      return this->dmaSupport;

    default:
      return false;
  }
}

static void egl_onRestart(LG_Renderer * renderer)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  eglDestroyContext(this->display, this->frameContext);
  this->frameContext = NULL;
  this->start        = false;
}

static void egl_calc_mouse_size(struct Inst * this)
{
  if (!this->formatValid)
    return;

  int w, h;

  switch(this->format.rotate)
  {
    case LG_ROTATE_0:
    case LG_ROTATE_180:
      this->mouseScaleX = 2.0f / this->format.width;
      this->mouseScaleY = 2.0f / this->format.height;
      w = this->format.width;
      h = this->format.height;
      break;

    case LG_ROTATE_90:
    case LG_ROTATE_270:
      this->mouseScaleX = 2.0f / this->format.height;
      this->mouseScaleY = 2.0f / this->format.width;
      w = this->format.height;
      h = this->format.width;
      break;

    default:
      DEBUG_UNREACHABLE();
  }

  switch((this->format.rotate + this->rotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
    case LG_ROTATE_180:
      egl_cursorSetSize(this->cursor,
        (this->mouseWidth  * (1.0f / w)) * this->scaleX,
        (this->mouseHeight * (1.0f / h)) * this->scaleY
      );
      break;

    case LG_ROTATE_90:
    case LG_ROTATE_270:
      egl_cursorSetSize(this->cursor,
        (this->mouseWidth  * (1.0f / w)) * this->scaleY,
        (this->mouseHeight * (1.0f / h)) * this->scaleX
      );
      break;
  }
}

static void egl_calc_mouse_state(struct Inst * this)
{
  if (!this->formatValid)
    return;

  switch((this->format.rotate + this->rotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
    case LG_ROTATE_180:
      egl_cursorSetState(
        this->cursor,
        this->cursorVisible,
        (((float)this->cursorX * this->mouseScaleX) - 1.0f) * this->scaleX,
        (((float)this->cursorY * this->mouseScaleY) - 1.0f) * this->scaleY
      );
      break;

    case LG_ROTATE_90:
    case LG_ROTATE_270:
      egl_cursorSetState(
        this->cursor,
        this->cursorVisible,
        (((float)this->cursorX * this->mouseScaleX) - 1.0f) * this->scaleY,
        (((float)this->cursorY * this->mouseScaleY) - 1.0f) * this->scaleX
      );
      break;
  }
}

static void egl_update_scale_type(struct Inst * this)
{
  int width = 0, height = 0;

  switch (this->rotate)
  {
    case LG_ROTATE_0:
    case LG_ROTATE_180:
      width  = this->format.width;
      height = this->format.height;
      break;

    case LG_ROTATE_90:
    case LG_ROTATE_270:
      width  = this->format.height;
      height = this->format.width;
      break;
  }

  if (width == this->viewportWidth || height == this->viewportHeight)
    this->scaleType = EGL_DESKTOP_NOSCALE;
  else if (width > this->viewportWidth || height > this->viewportHeight)
    this->scaleType = EGL_DESKTOP_DOWNSCALE;
  else
    this->scaleType = EGL_DESKTOP_UPSCALE;
}

void egl_resetViewport(EGL * this)
{
  glViewport(0, 0, this->width, this->height);
}

static void egl_onResize(LG_Renderer * renderer, const int width, const int height, const double scale,
    const LG_RendererRect destRect, LG_RendererRotate rotate)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  this->width   = width * scale;
  this->height  = height * scale;
  this->uiScale = (float) scale;
  this->rotate  = rotate;

  this->destRect.x = destRect.x * scale;
  this->destRect.y = destRect.y * scale;
  this->destRect.w = destRect.w * scale;
  this->destRect.h = destRect.h * scale;

  glViewport(0, 0, this->width, this->height);

  if (destRect.valid)
  {
    this->translateX     = 1.0f - (((this->destRect.w / 2) + this->destRect.x) * 2) / (float)this->width;
    this->translateY     = 1.0f - (((this->destRect.h / 2) + this->destRect.y) * 2) / (float)this->height;
    this->scaleX         = (float)this->destRect.w / (float)this->width;
    this->scaleY         = (float)this->destRect.h / (float)this->height;
    this->viewportWidth  = this->destRect.w;
    this->viewportHeight = this->destRect.h;
  }

  egl_update_scale_type(this);
  egl_calc_mouse_size(this);

  this->splashRatio  = (float)width / (float)height;
  this->screenScaleX = 1.0f / this->width;
  this->screenScaleY = 1.0f / this->height;

  egl_calc_mouse_state(this);

  INTERLOCKED_SECTION(this->desktopDamageLock, {
    this->desktopDamage[this->desktopDamageIdx].count = -1;
  });

  // this is needed to refresh the font atlas texture
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplOpenGL3_NewFrame();

  egl_damageResize(this->damage, this->translateX, this->translateY, this->scaleX, this->scaleY);
  egl_desktopResize(this->desktop, this->width, this->height);
}

static bool egl_onMouseShape(LG_Renderer * renderer, const LG_RendererCursor cursor,
    const int width, const int height,
    const int pitch, const uint8_t * data)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  if (!egl_cursorSetShape(this->cursor, cursor, width, height, pitch, data))
  {
    DEBUG_ERROR("Failed to update the cursor shape");
    return false;
  }

  this->mouseWidth  = width;
  this->mouseHeight = height;
  egl_calc_mouse_size(this);

  return true;
}

static bool egl_onMouseEvent(LG_Renderer * renderer, const bool visible, const int x, const int y)
{
  struct Inst * this = UPCAST(struct Inst, renderer);
  this->cursorVisible = visible;
  this->cursorX       = x;
  this->cursorY       = y;
  egl_calc_mouse_state(this);
  return true;
}

static bool egl_onFrameFormat(LG_Renderer * renderer, const LG_RendererFormat format)
{
  struct Inst * this = UPCAST(struct Inst, renderer);
  memcpy(&this->format, &format, sizeof(LG_RendererFormat));
  this->formatValid = true;

  /* this event runs in a second thread so we need to init it here */
  if (!this->frameContext)
  {
    static EGLint attrs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE
    };

    if (!(this->frameContext = eglCreateContext(this->display, this->configs, this->context, attrs)))
    {
      DEBUG_ERROR("Failed to create the frame context");
      return false;
    }

    if (!eglMakeCurrent(this->display, EGL_NO_SURFACE, EGL_NO_SURFACE, this->frameContext))
    {
      DEBUG_ERROR("Failed to make the frame context current");
      return false;
    }
  }

  egl_update_scale_type(this);
  egl_damageSetup(this->damage, format.width, format.height);

  return egl_desktopSetup(this->desktop, format);
}

static bool egl_onFrame(LG_Renderer * renderer, const FrameBuffer * frame, int dmaFd,
    const FrameDamageRect * damageRects, int damageRectsCount)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  uint64_t start = nanotime();
  if (!egl_desktopUpdate(this->desktop, frame, dmaFd, damageRects, damageRectsCount))
  {
    DEBUG_INFO("Failed to to update the desktop");
    return false;
  }
  ringbuffer_push(this->importTimings, &(float){ (nanotime() - start) * 1e-6f });

  this->start = true;

  INTERLOCKED_SECTION(this->desktopDamageLock, {
    struct DesktopDamage * damage = this->desktopDamage + this->desktopDamageIdx;
    if (damage->count == -1 || damageRectsCount == 0 ||
        damage->count + damageRectsCount >= KVMFR_MAX_DAMAGE_RECTS)
      damage->count = -1;
    else
    {
      memcpy(damage->rects + damage->count, damageRects, damageRectsCount * sizeof(FrameDamageRect));
      damage->count += damageRectsCount;
    }
  });

  return true;
}

static void debugCallback(GLenum source, GLenum type, GLuint id,
    GLenum severity, GLsizei length, const GLchar * message,
    const void * userParam)
{
  enum DebugLevel level = DEBUG_LEVEL_FIXME;
  switch (severity)
  {
    case GL_DEBUG_SEVERITY_HIGH:
      level = DEBUG_LEVEL_ERROR;
      break;
    case GL_DEBUG_SEVERITY_MEDIUM:
      level = DEBUG_LEVEL_WARN;
      break;
    case GL_DEBUG_SEVERITY_LOW:
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      level = DEBUG_LEVEL_INFO;
      break;
  }

  const char * sourceName = "unknown";
  switch (source)
  {
    case GL_DEBUG_SOURCE_API:
      sourceName = "OpenGL API";
      break;
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
      sourceName = "window system";
      break;
    case GL_DEBUG_SOURCE_SHADER_COMPILER:
      sourceName = "shader compiler";
      break;
    case GL_DEBUG_SOURCE_THIRD_PARTY:
      sourceName = "third party";
      break;
    case GL_DEBUG_SOURCE_APPLICATION:
      sourceName = "application";
      break;
    case GL_DEBUG_SOURCE_OTHER:
      sourceName = "other";
      break;
  }

  const char * typeName = "unknown";
  switch (type)
  {
    case GL_DEBUG_TYPE_ERROR:
      typeName = "error";
      break;
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
      typeName = "deprecated behaviour";
      break;
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
      typeName = "undefined behaviour";
      break;
    case GL_DEBUG_TYPE_PORTABILITY:
      typeName = "portability";
      break;
    case GL_DEBUG_TYPE_PERFORMANCE:
      typeName = "performance";
      break;
    case GL_DEBUG_TYPE_MARKER:
      typeName = "marker";
      break;
    case GL_DEBUG_TYPE_PUSH_GROUP:
      typeName = "group pushing";
      break;
    case GL_DEBUG_TYPE_POP_GROUP:
      typeName = "group popping";
      break;
    case GL_DEBUG_TYPE_OTHER:
      typeName = "other";
      break;
  }

  DEBUG_PRINT(level, "GL message (source: %s, type: %s): %s", sourceName, typeName, message);
}

static void egl_configUI(void * opaque, int * id)
{
  struct Inst * this = opaque;
  egl_damageConfigUI(this->damage);
  igSeparator();
  egl_desktopConfigUI(this->desktop);
}

static bool egl_renderStartup(LG_Renderer * renderer, bool useDMA)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  this->nativeWind = app_getEGLNativeWindow();
  if (!this->nativeWind)
  {
    DEBUG_ERROR("Failed to get EGL native window");
    return false;
  }

  this->display = app_getEGLDisplay();
  if (this->display == EGL_NO_DISPLAY)
  {
    DEBUG_ERROR("Failed to get EGL display");
    return false;
  }

  int maj, min;
  if (!eglInitialize(this->display, &maj, &min))
  {
    DEBUG_ERROR("Unable to initialize EGL");
    return false;
  }

  int maxSamples = 1;
  if (option_get_bool("egl", "multisample"))
  {
    if (app_getProp(LG_DS_MAX_MULTISAMPLE, &maxSamples) && maxSamples > 1)
    {
      if (maxSamples > 4)
        maxSamples = 4;

      DEBUG_INFO("Multisampling enabled, max samples: %d", maxSamples);
    }
  }

  EGLint attr[] =
  {
    EGL_BUFFER_SIZE    , 24,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SAMPLE_BUFFERS , maxSamples > 0 ? 1 : 0,
    EGL_SAMPLES        , maxSamples,
    EGL_NONE
  };

  EGLint num_config;
  if (!eglChooseConfig(this->display, attr, &this->configs, 1, &num_config))
  {
    DEBUG_ERROR("Failed to choose config (eglError: 0x%x)", eglGetError());
    return false;
  }

  const EGLint surfattr[] =
  {
    EGL_RENDER_BUFFER, this->opt.doubleBuffer ? EGL_BACK_BUFFER : EGL_SINGLE_BUFFER,
    EGL_NONE
  };

  this->surface = eglCreateWindowSurface(this->display, this->configs, this->nativeWind, surfattr);
  if (this->surface == EGL_NO_SURFACE)
  {
    DEBUG_ERROR("Failed to create EGL surface (eglError: 0x%x)", eglGetError());
    return false;
  }

  const char * client_exts = eglQueryString(this->display, EGL_EXTENSIONS);
  bool debugContext = option_get_bool("egl", "debug");
  EGLint ctxattr[5];
  int ctxidx = 0;

  ctxattr[ctxidx++] = EGL_CONTEXT_CLIENT_VERSION;
  ctxattr[ctxidx++] = 2;

  if (maj > 1 || (maj == 1 && min >= 5))
  {
    ctxattr[ctxidx++] = EGL_CONTEXT_OPENGL_DEBUG;
    ctxattr[ctxidx++] = debugContext ? EGL_TRUE : EGL_FALSE;
  }
  else if (util_hasGLExt(client_exts, "EGL_KHR_create_context"))
  {
    ctxattr[ctxidx++] = EGL_CONTEXT_FLAGS_KHR;
    ctxattr[ctxidx++] = debugContext ? EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR : 0;
  }
  else if (debugContext)
    DEBUG_WARN("Cannot create debug contexts before EGL 1.5 without EGL_KHR_create_context");

  ctxattr[ctxidx++] = EGL_NONE;

  this->context = eglCreateContext(this->display, this->configs, EGL_NO_CONTEXT, ctxattr);
  if (this->context == EGL_NO_CONTEXT)
  {
    DEBUG_ERROR("Failed to create EGL context (eglError: 0x%x)", eglGetError());
    return false;
  }

  EGLint rb = 0;
  eglQuerySurface(this->display, this->surface, EGL_RENDER_BUFFER, &rb);
  switch(rb)
  {
    case EGL_SINGLE_BUFFER:
      DEBUG_INFO("Single buffer mode");
      break;

    case EGL_BACK_BUFFER:
      DEBUG_INFO("Back buffer mode");
      break;

    default:
      DEBUG_WARN("Unknown render buffer mode: %d", rb);
      break;
  }

  eglMakeCurrent(this->display, this->surface, this->surface, this->context);
  const char * gl_exts = (const char *)glGetString(GL_EXTENSIONS);
  const char * vendor  = (const char *)glGetString(GL_VENDOR);

  DEBUG_INFO("EGL     : %d.%d", maj, min);
  DEBUG_INFO("Vendor  : %s", vendor);
  DEBUG_INFO("Renderer: %s", glGetString(GL_RENDERER));
  DEBUG_INFO("Version : %s", glGetString(GL_VERSION ));
  DEBUG_INFO("EGL APIs: %s", eglQueryString(this->display, EGL_CLIENT_APIS));
  DEBUG_INFO("EGL Exts: %s", client_exts);
  DEBUG_INFO("GL Exts : %s", gl_exts);

  GLint esMaj, esMin;
  glGetIntegerv(GL_MAJOR_VERSION, &esMaj);
  glGetIntegerv(GL_MINOR_VERSION, &esMin);

  if (!util_hasGLExt(gl_exts, "GL_EXT_buffer_storage"))
  {
    DEBUG_ERROR("GL_EXT_buffer_storage is needed to use EGL backend");
    return false;
  }

  if (!util_hasGLExt(gl_exts, "GL_EXT_texture_format_BGRA8888"))
  {
    DEBUG_ERROR("GL_EXT_texture_format_BGRA8888 is needed to use EGL backend");
    return false;
  }

  this->hasBufferAge = util_hasGLExt(client_exts, "EGL_EXT_buffer_age");
  if (!this->hasBufferAge)
    DEBUG_WARN("GL_EXT_buffer_age is not supported, will not perform as well.");

  if (g_egl_dynProcs.glEGLImageTargetTexture2DOES)
  {
    if (util_hasGLExt(client_exts, "EGL_EXT_image_dma_buf_import"))
      this->dmaSupport = true;
    else
      DEBUG_INFO("EGL_EXT_image_dma_buf_import unavailable, DMA support disabled");
  }
  else
    DEBUG_INFO("glEGLImageTargetTexture2DOES unavilable, DMA support disabled");

  if (!this->dmaSupport)
    useDMA = false;

  if (debugContext)
  {
    if ((esMaj > 3 || (esMaj == 3 && esMin >= 2)) && g_egl_dynProcs.glDebugMessageCallback)
    {
      g_egl_dynProcs.glDebugMessageCallback(debugCallback, NULL);
      DEBUG_INFO("Using debug message callback from OpenGL ES 3.2+");
    }
    else if (util_hasGLExt(gl_exts, "GL_KHR_debug") && g_egl_dynProcs.glDebugMessageCallbackKHR)
    {
      g_egl_dynProcs.glDebugMessageCallbackKHR(debugCallback, NULL);
      DEBUG_INFO("Using debug message callback from GL_KHR_debug");
    }
    else
      DEBUG_INFO("Debug message callback not supported");
  }
  else
    DEBUG_INFO("Debug messages disabled, enable with egl:debug=true");

  eglSwapInterval(this->display, this->opt.vsync ? 1 : 0);

  if (!egl_desktopInit(this, &this->desktop, this->display, useDMA, MAX_ACCUMULATED_DAMAGE))
  {
    DEBUG_ERROR("Failed to initialize the desktop");
    return false;
  }

  if (!egl_cursorInit(&this->cursor))
  {
    DEBUG_ERROR("Failed to initialize the cursor");
    return false;
  }

  if (!egl_splashInit(&this->splash))
  {
    DEBUG_ERROR("Failed to initialize the splash screen");
    return false;
  }

  if (!egl_damageInit(&this->damage))
  {
    DEBUG_ERROR("Failed to initialize the damage display");
    return false;
  }

  if (!ImGui_ImplOpenGL3_Init("#version 300 es"))
  {
    DEBUG_ERROR("Failed to initialize ImGui");
    return false;
  }

  app_overlayConfigRegister("EGL", egl_configUI, this);

  this->imgui = true;
  return true;
}

static bool egl_needsRender(LG_Renderer * renderer)
{
  struct Inst * this = UPCAST(struct Inst, renderer);
  return !this->waitDone;
}

inline static EGLint egl_buffer_age(struct Inst * this)
{
  if (!this->hasBufferAge)
    return 0;

  EGLint result;
  eglQuerySurface(this->display, this->surface, EGL_BUFFER_AGE_EXT, &result);
  return result;
}

inline static void renderLetterBox(struct Inst * this)
{
  bool hLB = this->destRect.x > 0;
  bool vLB = this->destRect.y > 0;

  if (hLB || vLB)
  {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_SCISSOR_TEST);

    if (hLB)
    {
      glScissor(0.0f, 0.0f, this->destRect.x + 0.5f, this->height);
      glClear(GL_COLOR_BUFFER_BIT);

      float x2 = this->destRect.x + this->destRect.w;
      glScissor(x2 - 0.5, 0.0f, this->width - x2, this->height + 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
    }

    if (vLB)
    {
      glScissor(0.0f, 0.0f, this->width, this->destRect.y + 0.5f);
      glClear(GL_COLOR_BUFFER_BIT);

      float y2 = this->destRect.y + this->destRect.h;
      glScissor(0.0f, y2 - 0.5, this->width, this->height - y2 + 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
    }

    glDisable(GL_SCISSOR_TEST);
  }
}

static bool egl_render(LG_Renderer * renderer, LG_RendererRotate rotate,
    const bool newFrame, const bool invalidateWindow,
    void (*preSwap)(void * udata), void * udata)
{
  struct Inst * this = UPCAST(struct Inst, renderer);
  EGLint bufferAge   = egl_buffer_age(this);
  bool renderAll     = invalidateWindow || !this->start || this->hadOverlay ||
                       bufferAge <= 0 || bufferAge > MAX_BUFFER_AGE;

  bool hasOverlay = false;
  struct CursorState cursorState = { .visible = false };
  struct DesktopDamage * desktopDamage;

  char accumulated_[
    sizeof(struct DamageRects) +
    MAX_ACCUMULATED_DAMAGE * sizeof(struct FrameDamageRect)
  ];
  struct DamageRects * accumulated = (struct DamageRects *) accumulated_;
  accumulated->count = 0;

  INTERLOCKED_SECTION(this->desktopDamageLock, {
    if (!renderAll)
    {
      for (int i = 0; i < bufferAge; ++i)
      {
        struct DesktopDamage * damage = this->desktopDamage +
            IDX_AGO(this->desktopDamageIdx, i, DESKTOP_DAMAGE_COUNT);

        if (damage->count < 0)
        {
          renderAll = true;
          break;
        }

        for (int j = 0; j < damage->count; ++j)
        {
          struct FrameDamageRect * rect = damage->rects + j;
          int x = rect->x > 0 ? rect->x - 1 : 0;
          int y = rect->y > 0 ? rect->y - 1 : 0;
          accumulated->rects[accumulated->count++] = (struct FrameDamageRect) {
            .x = x, .y = y,
            .width  = min(this->format.width  - x, rect->width  + 2),
            .height = min(this->format.height - y, rect->height + 2),
          };
        }
      }
    }
    desktopDamage = this->desktopDamage + this->desktopDamageIdx;
    this->desktopDamageIdx = (this->desktopDamageIdx + 1) % DESKTOP_DAMAGE_COUNT;
    this->desktopDamage[this->desktopDamageIdx].count = 0;
  });

  if (!renderAll)
  {
    double matrix[6];
    egl_screenToDesktopMatrix(matrix, this->format.width, this->format.height,
        this->translateX, this->translateY, this->scaleX, this->scaleY, rotate,
        this->width, this->height);

    for (int i = 0; i < bufferAge; ++i)
    {
      int idx   = IDX_AGO(this->overlayHistoryIdx, i, DESKTOP_DAMAGE_COUNT);
      int count = this->overlayHistoryCount[idx];
      struct Rect * damage = this->overlayHistory[idx];

      if (count < 0)
      {
        renderAll = true;
        break;
      }

      for (int j = 0; j < count; ++j)
        accumulated->count += egl_screenToDesktop(
          accumulated->rects + accumulated->count, matrix, damage + j,
          this->format.width, this->format.height
        );
    }

    accumulated->count = rectsMergeOverlapping(accumulated->rects, accumulated->count);
  }
  ++this->overlayHistoryIdx;

  if (this->start)
  {
    if (egl_desktopRender(this->desktop,
        this->destRect.w, this->destRect.h,
        this->translateX, this->translateY,
        this->scaleX    , this->scaleY    ,
        this->scaleType , rotate, renderAll ? NULL : accumulated))
    {
      if (!this->waitFadeTime)
      {
        if (!this->params.quickSplash)
          this->waitFadeTime = microtime() + SPLASH_FADE_TIME;
        else
          this->waitDone = true;
      }

      cursorState = egl_cursorRender(this->cursor,
          (this->format.rotate + rotate) % LG_ROTATE_MAX,
          this->width, this->height);
    }
    else
      hasOverlay = true;
  }

  renderLetterBox(this);

  if (!this->waitDone)
  {
    float a = 1.0f;
    if (!this->waitFadeTime)
      a = 1.0f;
    else
    {
      uint64_t t = microtime();
      if (t > this->waitFadeTime)
        this->waitDone = true;
      else
      {
        uint64_t delta = this->waitFadeTime - t;
        a = 1.0f / SPLASH_FADE_TIME * delta;
      }
    }

    if (!this->waitDone)
    {
      egl_splashRender(this->splash, a, this->splashRatio);
      hasOverlay = true;
    }
  }
  else if (!this->start)
  {
    egl_splashRender(this->splash, 1.0f, this->splashRatio);
    hasOverlay = true;
  }

  hasOverlay |= egl_damageRender(this->damage, rotate, newFrame ? desktopDamage : NULL);
  hasOverlay |= invalidateWindow;

  struct Rect damage[KVMFR_MAX_DAMAGE_RECTS + MAX_OVERLAY_RECTS + 2];
  int damageIdx = app_renderOverlay(damage, MAX_OVERLAY_RECTS);

  switch (damageIdx)
  {
    case 0: // no overlay
      break;
    case -1: // full damage
      hasOverlay = true;
      // fallthrough
    default:
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());

      for (int i = 0; i < damageIdx; ++i)
        damage[i].y = this->height - damage[i].y - damage[i].h;
  }

  if (damageIdx >= 0 && cursorState.visible)
    damage[damageIdx++] = cursorState.rect;

  int overlayHistoryIdx = this->overlayHistoryIdx % DESKTOP_DAMAGE_COUNT;
  if (hasOverlay)
    this->overlayHistoryCount[overlayHistoryIdx] = -1;
  else
  {
    if (damageIdx > 0)
      memcpy(this->overlayHistory[overlayHistoryIdx], damage, damageIdx * sizeof(struct Rect));
    this->overlayHistoryCount[overlayHistoryIdx] = damageIdx;
  }

  if (!hasOverlay && !this->hadOverlay)
  {
    if (this->cursorLast.visible)
      damage[damageIdx++] = this->cursorLast.rect;

    if (desktopDamage->count == -1)
      // -1 damage count means invalidating entire window.
      damageIdx = 0;
    else
    {
      double matrix[6];
      egl_desktopToScreenMatrix(matrix, this->format.width, this->format.height,
          this->translateX, this->translateY, this->scaleX, this->scaleY, rotate,
          this->width, this->height);

      for (int i = 0; i < desktopDamage->count; ++i)
        damage[damageIdx++] = egl_desktopToScreen(matrix, desktopDamage->rects + i);
    }
  }
  else
    damageIdx = 0;

  this->hadOverlay = hasOverlay;
  this->cursorLast = cursorState;

  preSwap(udata);
  app_eglSwapBuffers(this->display, this->surface, damage, damageIdx);
  return true;
}

struct LG_RendererOps LGR_EGL =
{
  .getName       = egl_getName,
  .setup         = egl_setup,
  .create        = egl_create,
  .initialize    = egl_initialize,
  .deinitialize  = egl_deinitialize,
  .supports      = egl_supports,
  .onRestart     = egl_onRestart,
  .onResize      = egl_onResize,
  .onMouseShape  = egl_onMouseShape,
  .onMouseEvent  = egl_onMouseEvent,
  .onFrameFormat = egl_onFrameFormat,
  .onFrame       = egl_onFrame,
  .renderStartup = egl_renderStartup,
  .needsRender   = egl_needsRender,
  .render        = egl_render
};
