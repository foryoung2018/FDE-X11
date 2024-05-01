#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma ide diagnostic ignored "UnusedParameter"
#pragma ide diagnostic ignored "DanglingPointer"
#pragma ide diagnostic ignored "ConstantConditionsOC"
#pragma ide diagnostic ignored "OCUnusedGlobalDeclarationInspection"
#pragma ide diagnostic ignored "UnreachableCode"
#pragma ide diagnostic ignored "OCUnusedMacroInspection"
#pragma ide diagnostic ignored "misc-no-recursion"
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include "node.h"
#include <dlfcn.h>
#include "renderer.h"
#include "os.h"
#include <globals.h>

#define log(...) __android_log_print(ANDROID_LOG_DEBUG, "huyang_renderer", __VA_ARGS__)
#define loge(...) __android_log_print(ANDROID_LOG_ERROR, "huyang_renderer", __VA_ARGS__)

static GLuint create_program(const char* p_vertex_source, const char* p_fragment_source);

static int eglCheckError(int line) {
    char* desc;
    int err = eglGetError();
    switch(err) {
#define E(code, text) case code: desc = (char*) text; break
        case EGL_SUCCESS: desc = NULL; // "No error"
        E(EGL_NOT_INITIALIZED, "EGL not initialized or failed to initialize");
        E(EGL_BAD_ACCESS, "Resource inaccessible");
        E(EGL_BAD_ALLOC, "Cannot allocate resources");
        E(EGL_BAD_ATTRIBUTE, "Unrecognized attribute or attribute value");
        E(EGL_BAD_CONTEXT, "Invalid EGL context");
        E(EGL_BAD_CONFIG, "Invalid EGL frame buffer configuration");
        E(EGL_BAD_CURRENT_SURFACE, "Current surface is no longer valid");
        E(EGL_BAD_DISPLAY, "Invalid EGL display");
        E(EGL_BAD_SURFACE, "Invalid surface");
        E(EGL_BAD_MATCH, "Inconsistent arguments");
        E(EGL_BAD_PARAMETER, "Invalid argument");
        E(EGL_BAD_NATIVE_PIXMAP, "Invalid native pixmap");
        E(EGL_BAD_NATIVE_WINDOW, "Invalid native window");
        E(EGL_CONTEXT_LOST, "Context lost");
#undef E
        default: desc = (char*) "Unknown error";
    }

    if (desc)
        log("Xlorie: egl error on line %d: %s\n", line, desc);

    return err;
}

static const char* eglErrorLabel(int code) {
    switch(code) {
        case EGL_SUCCESS: return NULL; // "No error"
#define E(code) case code: return #code; break
        E(EGL_NOT_INITIALIZED);
        E(EGL_BAD_ACCESS);
        E(EGL_BAD_ALLOC);
        E(EGL_BAD_ATTRIBUTE);
        E(EGL_BAD_CONTEXT);
        E(EGL_BAD_CONFIG);
        E(EGL_BAD_CURRENT_SURFACE);
        E(EGL_BAD_DISPLAY);
        E(EGL_BAD_SURFACE);
        E(EGL_BAD_MATCH);
        E(EGL_BAD_PARAMETER);
        E(EGL_BAD_NATIVE_PIXMAP);
        E(EGL_BAD_NATIVE_WINDOW);
        E(EGL_CONTEXT_LOST);
#undef E
        default: return "EGL_UNKNOWN_ERROR";
    }

}

static void checkGlError(int line) {
    GLenum error;
    char *desc = NULL;
    for (error = glGetError(); error; error = glGetError()) {
        switch (error) {
#define E(code) case code: desc = (char*)#code; break
            E(GL_INVALID_ENUM);
            E(GL_INVALID_VALUE);
            E(GL_INVALID_OPERATION);
            E(GL_STACK_OVERFLOW_KHR);
            E(GL_STACK_UNDERFLOW_KHR);
            E(GL_OUT_OF_MEMORY);
            E(GL_INVALID_FRAMEBUFFER_OPERATION);
            E(GL_CONTEXT_LOST_KHR);
            default:
                continue;
#undef E
        }
        log("Xlorie: GLES %d ERROR: %s.\n", line, desc);
        return;
    }
}

#define checkGlError() checkGlError(__LINE__)


static const char vertex_shader[] =
    "attribute vec4 position;\n"
    "attribute vec2 texCoords;"
    "varying vec2 outTexCoords;\n"
    "void main(void) {\n"
    "   outTexCoords = texCoords;\n"
    "   gl_Position = position;\n"
    "}\n";

#define FRAGMENT_SHADER(texture) \
    "precision mediump float;\n" \
    "varying vec2 outTexCoords;\n" \
    "uniform sampler2D texture;\n" \
    "void main(void) {\n" \
    "   gl_FragColor = texture2D(texture, outTexCoords)" texture ";\n" \
    "}\n"

static const char fragment_shader[] = FRAGMENT_SHADER();
static const char fragment_shader_bgra[] = FRAGMENT_SHADER(".bgra");

static EGLDisplay global_egl_display = EGL_NO_DISPLAY;
static EGLContext global_ctx = EGL_NO_CONTEXT;
static EGLConfig global_config = 0;
static EGLSurface sfc = EGL_NO_SURFACE;
static EGLSurface sfc1 = EGL_NO_SURFACE;
static EGLSurface sfc2 = EGL_NO_SURFACE;
static EGLNativeWindowType win = 0;
static jobject surface = NULL;
static AHardwareBuffer *buffer = NULL;
static EGLImageKHR image = NULL;
static int renderedFrames = 0;
static jmethodID Surface_release = NULL;
static jmethodID Surface_destroy = NULL;
struct WindowNode* NamedWindow_WindowPtr = NULL;
extern void TransferBuffer2FDE(WindowPtr ptr);
extern void UpdateBuffer(int index);


static struct {
    GLuint id;
    float width, height;
} display;
static struct {
    GLuint id;
    float x, y, width, height, xhot, yhot;
} cursor;




GLuint g_texture_program = 0, gv_pos = 0, gv_coords = 0;
GLuint g_texture_program_bgra = 0, gv_pos_bgra = 0, gv_coords_bgra = 0;

int renderer_init(JNIEnv* env, int* legacy_drawing, uint8_t* flip) {
    EGLint major, minor;
    EGLint numConfigs;
    const EGLint configAttribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 0,
            EGL_NONE
    };
    const EGLint configAttribs2[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
    };
    const EGLint ctxattribs[] = {
            EGL_CONTEXT_CLIENT_VERSION,2, EGL_NONE
    };

    if (global_ctx)
        return 1;

    jclass Surface = (*env)->FindClass(env, "android/view/Surface");
    Surface_release = (*env)->GetMethodID(env, Surface, "release", "()V");
    Surface_destroy = (*env)->GetMethodID(env, Surface, "destroy", "()V");
    if (!Surface_release) {
        loge("Failed to find required Surface.release method. Aborting.\n");
        abort();
    }
    if (!Surface_destroy) {
        loge("Failed to find required Surface.destroy method. Aborting.\n");
        abort();
    }

    global_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (global_egl_display == EGL_NO_DISPLAY) {
        log("Xlorie: Got no EGL display.\n");
        eglCheckError(__LINE__);
        return 0;
    }

    if (eglInitialize(global_egl_display, &major, &minor) != EGL_TRUE) {
        log("Xlorie: Unable to initialize EGL\n");
        eglCheckError(__LINE__);
        return 0;
    }
    log("Xlorie: Initialized EGL version %d.%d\n", major, minor);
    eglBindAPI(EGL_OPENGL_ES_API);

    if (eglChooseConfig(global_egl_display, configAttribs, &global_config, 1, &numConfigs) != EGL_TRUE &&
        eglChooseConfig(global_egl_display, configAttribs2, &global_config, 1, &numConfigs) != EGL_TRUE) {
        log("Xlorie: eglChooseConfig failed.\n");
        eglCheckError(__LINE__);
        return 0;
    }

    global_ctx = eglCreateContext(global_egl_display, global_config, NULL, ctxattribs);
    if (global_ctx == EGL_NO_CONTEXT) {
        log("Xlorie: eglCreateContext failed.\n");
        eglCheckError(__LINE__);
        return 0;
    }
    if (eglMakeCurrent(global_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
        log("Xlorie: eglMakeCurrent failed.\n");
        eglCheckError(__LINE__);
        return 0;
    }

    {
        // Some devices do not support sampling from HAL_PIXEL_FORMAT_BGRA_8888, here we are checking it.
        const EGLint imageAttributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        EGLClientBuffer clientBuffer;
        EGLImageKHR img;
        AHardwareBuffer *new = NULL;
        int status;
        AHardwareBuffer_Desc d0 = {
                .width = 64,
                .height = 64,
                .layers = 1,
                .usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                .format = AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM
        };

        status = AHardwareBuffer_allocate(&d0, &new);
        if (status != 0 || new == NULL) {
            loge("Failed to allocate native buffer (%p, error %d)", new, status);
            loge("Forcing legacy drawing");
            *legacy_drawing = 1;
            return 1;
        }

        uint32_t *pixels;
        if (AHardwareBuffer_lock(new, d0.usage, -1, NULL, (void **) &pixels) == 0) {
            pixels[0] = 0xAABBCCDD;
            AHardwareBuffer_unlock(new, NULL);
        } else {
            loge("Failed to lock native buffer (%p, error %d)", new, status);
            loge("Forcing legacy drawing");
            *legacy_drawing = 1;
            return 1;
        }

        clientBuffer = eglGetNativeClientBufferANDROID(new);
        if (!clientBuffer) {
            eglCheckError(__LINE__);
            loge("Failed to obtain EGLClientBuffer from AHardwareBuffer");
            loge("Forcing legacy drawing");
            *legacy_drawing = 1;
            return 1;
        }

        if (!(img = eglCreateImageKHR(global_egl_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer, imageAttributes))) {
            if (eglGetError() == EGL_BAD_PARAMETER) {
                loge("Sampling from HAL_PIXEL_FORMAT_BGRA_8888 is not supported, forcing AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM");
                *flip = 1;
            } else {
                loge("Failed to obtain EGLImageKHR from EGLClientBuffer");
                loge("Forcing legacy drawing");
                *legacy_drawing = 1;
            }
        } else {
            // For some reason all devices I checked had no GL_EXT_texture_format_BGRA8888 support, but some of them still provided BGRA extension.
            // EGL does not provide functions to query texture format in runtime.
            // Workarounds are less performant but at least they let us use Termux:X11 on devices with missing BGRA support.
            // We handle two cases.
            // If resulting texture has BGRA format but still drawing RGBA we should flip format to RGBA and flip pixels manually in shader.
            // In the case if for some reason we can not use HAL_PIXEL_FORMAT_BGRA_8888 we should fallback to legacy drawing method (uploading pixels via glTexImage2D).
            const EGLint configAttributes[] = {
                    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                    EGL_RED_SIZE, 8,
                    EGL_GREEN_SIZE, 8,
                    EGL_BLUE_SIZE, 8,
                    EGL_ALPHA_SIZE, 8,
                    EGL_NONE
            };
            EGLConfig checkcfg = 0;
            GLuint fbo = 0, texture = 0;
            if (eglChooseConfig(global_egl_display, configAttributes, &checkcfg, 1, &numConfigs) != EGL_TRUE) {
                log("Xlorie: check eglChooseConfig failed.\n");
                eglCheckError(__LINE__);
                return 0;
            }

            EGLContext testctx = eglCreateContext(global_egl_display, checkcfg, NULL, ctxattribs);
            if (testctx == EGL_NO_CONTEXT) {
                log("Xlorie: check eglCreateContext failed.\n");
                eglCheckError(__LINE__);
                return 0;
            }

            const EGLint pbufferAttributes[] = {
                    EGL_WIDTH, 64,
                    EGL_HEIGHT, 64,
                    EGL_NONE,
            };
            EGLSurface checksfc = eglCreatePbufferSurface(global_egl_display, checkcfg, pbufferAttributes);

            if (eglMakeCurrent(global_egl_display, checksfc, checksfc, testctx) != EGL_TRUE) {
                log("Xlorie: check eglMakeCurrent failed.\n");
                eglCheckError(__LINE__);
                return 0;
            }

            glActiveTexture(GL_TEXTURE0); checkGlError();
            glGenTextures(1, &texture); checkGlError();
            glBindTexture(GL_TEXTURE_2D, texture); checkGlError();
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); checkGlError();
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); checkGlError();
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); checkGlError();
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); checkGlError();
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img); checkGlError();
            glGenFramebuffers(1, &fbo); checkGlError();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo); checkGlError();
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0); checkGlError();
            uint32_t pixel[64*64];
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel); checkGlError();
            if (pixel[0] == 0xAABBCCDD) {
                log("Xlorie: GLES draws pixels unchanged, probably system does not support AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM. Forcing bgra.\n");
                *flip = 1;
            } else if (pixel[0] != 0xAADDCCBB) {
                log("Xlorie: GLES receives broken pixels. Forcing legacy drawing. 0x%X\n", pixel[0]);
                *legacy_drawing = 1;
            }
            eglMakeCurrent(global_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    }

    return 1;
}

static void renderer_unset_buffer(void) {
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        loge("There is no current context, `renderer_set_buffer` call is cancelled");
        return;
    }

    log("renderer_set_buffer0");
    if (image)
        eglDestroyImageKHR(global_egl_display, image);
    if (buffer)
        AHardwareBuffer_release(buffer);

    buffer = NULL;
}

void renderer_set_buffer(JNIEnv* env, AHardwareBuffer* buf) {
    log("renderer_set_buffer");
    const EGLint imageAttributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLClientBuffer clientBuffer;
    AHardwareBuffer_Desc desc = {0};
    uint8_t flip = 0;

    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        loge("There is no current context, `renderer_set_buffer` call is cancelled");
        return;
    }

    renderer_unset_buffer();

    buffer = buf;

    glBindTexture(GL_TEXTURE_2D, display.id); checkGlError();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); checkGlError();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); checkGlError();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); checkGlError();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); checkGlError();
    log("renderer_set_buffer 1");

    if (buffer) {
        AHardwareBuffer_acquire(buffer);
        AHardwareBuffer_describe(buffer, &desc);
        log("renderer_set_buffer width:%d height:%d", desc.width, desc.height);
        display.width = (float) desc.width;
        display.height = (float) desc.height;

        clientBuffer = eglGetNativeClientBufferANDROID(buffer);
        if (!clientBuffer) {
            eglCheckError(__LINE__);
            loge("Failed to obtain EGLClientBuffer from AHardwareBuffer");
        }
        image = clientBuffer ? eglCreateImageKHR(global_egl_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer, imageAttributes) : NULL;
        if (image != NULL) {
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
            flip = desc.format != AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM;
        } else {
            if (clientBuffer) {
                eglCheckError(__LINE__);
                loge("Binding AHardwareBuffer to an EGLImage failed.");
            }

            display.width = 1;
            display.height = 1;
            uint32_t data = {0};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &data);
            checkGlError();
        }
        checkGlError();
    } else {
        display.width = 1;
        display.height = 1;
        uint32_t data = {0};
        loge("There is no AHardwareBuffer, nothing to be bound.");
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &data); checkGlError();
    }
    log("renderer_set_buffer 2");

    renderer_redraw(env, flip);

    log("renderer_set_buffer %p %d %d", buffer, desc.width, desc.height);
}


void initAnotherSurface(JNIEnv *env, jobject surface, int index, float offsetX, float offsetY,
                        float width, float height,
                        WindowPtr ptr) {
    log("initAnotherSurface surface:%p index:%d offsetx:%f offsety:%f width:%f height:%f winPtr:%ld",
        surface, index, offsetX, offsetY
        ,width, height, ptr);

    WindowNode * window_node = node_search(NamedWindow_WindowPtr, 1000);
    if(window_node){
        window_node->data.offset_x = offsetX;
        window_node->data.offset_y = offsetY;
        window_node->data.width = width;
        window_node->data.height = height;
        window_node->data.pWin = (WindowPtr) ptr;
        window_node->data.index = index;

    } else {
        WindAttribute  windAttribute  = {
                .offset_x = offsetX,
                .offset_y = offsetY,
                .width = width,
                .height = height,
                .pWin = (WindowPtr) ptr,
                .index = index
        };
        node_append(&NamedWindow_WindowPtr, windAttribute);
    }

    EGLNativeWindowType w = ANativeWindow_fromSurface(env, surface);
    EGLSurface eglSurface = eglCreateWindowSurface(global_egl_display, global_config, w, NULL);
//    if (w && global_ctx && ANativeWindow_getWidth(w) > 0 && ANativeWindow_getHeight(w) > 0)
//        glViewport(0, 0, ANativeWindow_getWidth(w), ANativeWindow_getHeight(w)); checkGlError();
    if (eglSurface == EGL_NO_SURFACE) {
        eglCheckError(__LINE__);
        return;
    } else {
        WindowNode * index_node = node_get_at_index(NamedWindow_WindowPtr, index);
        index_node->data.sfc = eglSurface;
    }
}

void renderer_set_window_init(JNIEnv* env, AHardwareBuffer* new_buffer){
    if (!g_texture_program) {
        g_texture_program = create_program(vertex_shader, fragment_shader);
        if (!g_texture_program) {
            log("Xlorie: GLESv2: Unable to create shader program.\n");
            eglCheckError(__LINE__);
            return;
        }
        g_texture_program_bgra = create_program(vertex_shader, fragment_shader_bgra);
        if (!g_texture_program_bgra) {
            log("Xlorie: GLESv2: Unable to create bgra shader program.\n");
            eglCheckError(__LINE__);
            return;
        }
        gv_pos = (GLuint) glGetAttribLocation(g_texture_program, "position"); checkGlError();
        gv_coords = (GLuint) glGetAttribLocation(g_texture_program, "texCoords"); checkGlError();
        gv_pos_bgra = (GLuint) glGetAttribLocation(g_texture_program_bgra, "position"); checkGlError();
        gv_coords_bgra = (GLuint) glGetAttribLocation(g_texture_program_bgra, "texCoords"); checkGlError();
        glActiveTexture(GL_TEXTURE0); checkGlError();
        glGenTextures(1, &display.id); checkGlError();
        glGenTextures(1, &cursor.id); checkGlError();
    }
    eglSwapInterval(global_egl_display, 0);
    if (!new_buffer) {
        glClearColor(0.f, 0.f, 0.f, 0.0f); checkGlError();
        glClear(GL_COLOR_BUFFER_BIT); checkGlError();
    } else renderer_set_buffer(env, new_buffer);
}

void renderer_set_window(JNIEnv* env, jobject new_surface, AHardwareBuffer* new_buffer) {
    EGLNativeWindowType window;
    log("renderer_set_window begin0 %p", window);

    if (new_surface && surface && new_surface != surface && (*env)->IsSameObject(env, new_surface, surface)) {
        (*env)->DeleteGlobalRef(env, new_surface);
        new_surface = NULL;
        return;
    }

    window = new_surface ? ANativeWindow_fromSurface(env, new_surface) : NULL;
    int width = window ? ANativeWindow_getWidth(window) : 0;
    int height = window ? ANativeWindow_getHeight(window) : 0;
    log("renderer_set_window begin %p %d %d", window, width, height);
    if (window && win == window)
        return;

    if (sfc != EGL_NO_SURFACE) {
        if (eglMakeCurrent(global_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
            log("Xlorie: eglMakeCurrent (EGL_NO_SURFACE) failed.\n");
            eglCheckError(__LINE__);
            return;
        }
        if (eglDestroySurface(global_egl_display, sfc) != EGL_TRUE) {
            log("Xlorie: eglDestoySurface failed.\n");
            eglCheckError(__LINE__);
            return;
        }
    }
    log("renderer_set_window begin1 %p %d %d", window, width, height);
    sfc = EGL_NO_SURFACE;
    if (win)
        ANativeWindow_release(win);

    if (surface) {
        (*env)->CallVoidMethod(env, surface, Surface_release);
        (*env)->CallVoidMethod(env, surface, Surface_destroy);
        (*env)->DeleteGlobalRef(env, surface);
    }
    log("renderer_set_window begin2 %p %d %d", window, width, height);

    if (window && (width <= 0 || height <= 0)) {
        log("Xlorie: We've got invalid surface. Probably it became invalid before we started working with it.\n");
        ANativeWindow_release(window);
        window = NULL;
        if (new_surface) {
            (*env)->CallVoidMethod(env, new_surface, Surface_release);
            (*env)->CallVoidMethod(env, new_surface, Surface_destroy);
            (*env)->DeleteGlobalRef(env, new_surface);
            new_surface = NULL;
        }
    }
    log("renderer_set_window begin3 %p %d %d", window, width, height);

    win = window;
    surface = new_surface;

    if (!win)
        return;

    sfc = eglCreateWindowSurface(global_egl_display, global_config, win, NULL);
    if (sfc == EGL_NO_SURFACE) {
        log("Xlorie: eglCreateWindowSurface failed.\n");
        eglCheckError(__LINE__);
        return;
    }

    if (eglMakeCurrent(global_egl_display, sfc, sfc, global_ctx) != EGL_TRUE) {
        log("Xlorie: eglMakeCurrent failed.\n");
        eglCheckError(__LINE__);
        return;
    }
    log("renderer_set_window begin4 %p %d %d  sfc:%p", window, width, height, sfc);


    if (!g_texture_program) {
        g_texture_program = create_program(vertex_shader, fragment_shader);
        if (!g_texture_program) {
            log("Xlorie: GLESv2: Unable to create shader program.\n");
            eglCheckError(__LINE__);
            return;
        }

        g_texture_program_bgra = create_program(vertex_shader, fragment_shader_bgra);
        if (!g_texture_program_bgra) {
            log("Xlorie: GLESv2: Unable to create bgra shader program.\n");
            eglCheckError(__LINE__);
            return;
        }

        gv_pos = (GLuint) glGetAttribLocation(g_texture_program, "position"); checkGlError();
        gv_coords = (GLuint) glGetAttribLocation(g_texture_program, "texCoords"); checkGlError();

        gv_pos_bgra = (GLuint) glGetAttribLocation(g_texture_program_bgra, "position"); checkGlError();
        gv_coords_bgra = (GLuint) glGetAttribLocation(g_texture_program_bgra, "texCoords"); checkGlError();

        glActiveTexture(GL_TEXTURE0); checkGlError();
        glGenTextures(1, &display.id); checkGlError();
        glGenTextures(1, &cursor.id); checkGlError();
    }
    log("renderer_set_window begin5 %p %d %d", window, width, height);

    return;
    eglSwapInterval(global_egl_display, 0);

    if (win && global_ctx && ANativeWindow_getWidth(win) > 0 && ANativeWindow_getHeight(win) > 0){
        glViewport(0, 0, ANativeWindow_getWidth(win), ANativeWindow_getHeight(win)); checkGlError();
    }

//    log("Xlorie: new surface applied: %p\n", sfc);
    if (!new_buffer) {
        glClearColor(0.f, 0.f, 0.f, 0.0f); checkGlError();
        glClear(GL_COLOR_BUFFER_BIT); checkGlError();
    } else {
        renderer_set_buffer(env, new_buffer);
    }
    log("renderer_set_window after %p %d %d", window, width, height);
}

void renderer_set_window_each(JNIEnv* env, SurfaceRes* res, AHardwareBuffer* new_buffer) {
    WindowNode * window_node = node_search(NamedWindow_WindowPtr, res->window);
    if(window_node){
        window_node->data.offset_x = res->offset_y;
        window_node->data.offset_y = res->offset_y;
        window_node->data.width = res->width;
        window_node->data.height = res->height;
        window_node->data.pWin = (WindowPtr) res->pWin;
        window_node->data.index = res->id;
        window_node->data.window = res->window;
        log("renderer_set_window_each surface:%p id:%d",  res->surface , res->id);
        if(!res->surface && res->id != 0){
            node_delete(&NamedWindow_WindowPtr, window_node->data);
            log("renderer_set_window_each surface:%p id:%d",  res->surface , res->id);
            return;
        }
    } else {
        WindAttribute  windAttribute  = {
                .offset_x = res->offset_y,
                .offset_y = res->offset_y,
                .width =  res->width,
                .height =  res->height,
                .pWin = (WindowPtr) res->pWin,
                .index = res->id
        };
        node_append(&NamedWindow_WindowPtr, windAttribute);
    }
    jobject new_surface = res->surface;
    EGLNativeWindowType  window = new_surface ? ANativeWindow_fromSurface(env, new_surface) : NULL;
    int width = window ? ANativeWindow_getWidth(window) : 0;
    int height = window ? ANativeWindow_getHeight(window) : 0;
//    log("renderer_set_window_each window:%p width:%d heigght:%d index:%d %x %x new_buffer:%p",
//        window, width, height, res->id, res->window, res->pWin, new_buffer);
    WindowNode * index_node = node_get_at_index(NamedWindow_WindowPtr, res->id);
    EGLSurface sfc = index_node->data.sfc;
    if (sfc != EGL_NO_SURFACE) {
        if (eglMakeCurrent(global_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
            log("Xlorie: eglMakeCurrent (EGL_NO_SURFACE) failed.\n");
            eglCheckError(__LINE__);
            return;
        }
        if (eglDestroySurface(global_egl_display, sfc) != EGL_TRUE) {
            log("Xlorie: eglDestoySurface failed.\n");
            eglCheckError(__LINE__);
            return;
        }
    }
    sfc = EGL_NO_SURFACE;
    if (window && (width <= 0 || height <= 0)) {
        log("Xlorie: We've got invalid surface. Probably it became invalid before we started working with it.\n");
        ANativeWindow_release(window);
        window = NULL;
        if (new_surface) {
            (*env)->CallVoidMethod(env, new_surface, Surface_release);
            (*env)->CallVoidMethod(env, new_surface, Surface_destroy);
            (*env)->DeleteGlobalRef(env, new_surface);
            new_surface = NULL;
        }
    }
    if (!window)
        return;
    sfc = eglCreateWindowSurface(global_egl_display, global_config, window, NULL);
    if (sfc == EGL_NO_SURFACE) {
        log("Xlorie: eglCreateWindowSurface failed.\n");
        eglCheckError(__LINE__);
        return;
    }

    if (eglMakeCurrent(global_egl_display, sfc, sfc, global_ctx) != EGL_TRUE) {
        log("Xlorie: eglMakeCurrent failed.\n");
        eglCheckError(__LINE__);
        return;
    }
    index_node->data.sfc = sfc;

//    log("renderer_set_window_each begin4 %p %d %d  sfc:%p", window, width, height, sfc);

    if (!g_texture_program) {
        g_texture_program = create_program(vertex_shader, fragment_shader);
        if (!g_texture_program) {
            log("Xlorie: GLESv2: Unable to create shader program.\n");
            eglCheckError(__LINE__);
            return;
        }
        g_texture_program_bgra = create_program(vertex_shader, fragment_shader_bgra);
        if (!g_texture_program_bgra) {
            log("Xlorie: GLESv2: Unable to create bgra shader program.\n");
            eglCheckError(__LINE__);
            return;
        }
        gv_pos = (GLuint) glGetAttribLocation(g_texture_program, "position"); checkGlError();
        gv_coords = (GLuint) glGetAttribLocation(g_texture_program, "texCoords"); checkGlError();
        gv_pos_bgra = (GLuint) glGetAttribLocation(g_texture_program_bgra, "position"); checkGlError();
        gv_coords_bgra = (GLuint) glGetAttribLocation(g_texture_program_bgra, "texCoords"); checkGlError();
    }

    glActiveTexture(GL_TEXTURE0); checkGlError();
    if(!index_node->data.texture_id){
        glGenTextures(1, &index_node->data.texture_id); checkGlError();
    }
    if(!cursor.id){
        glGenTextures(1, &cursor.id); checkGlError();
    }
}

void renderer_update_root(int w, int h, void* data, uint8_t flip) {
    if (eglGetCurrentContext() == EGL_NO_CONTEXT || !w || !h) {
        return;
    }
//    log("renderer_update_root w:%d h:%d data:%p flip:%d display.width=%f display.height:%f",
//        w, h, data, flip, display.width, display.height );
    if (display.width != (float) w || display.height != (float) h) {
        display.width = (float) w;
        display.height = (float) h;

        glBindTexture(GL_TEXTURE_2D, display.id); checkGlError();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); checkGlError();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); checkGlError();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); checkGlError();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); checkGlError();
        glTexImage2D(GL_TEXTURE_2D, 0, flip ? GL_RGBA : GL_BGRA_EXT, w, h, 0, flip ? GL_RGBA : GL_BGRA_EXT, GL_UNSIGNED_BYTE, data); checkGlError();
    } else {
        glBindTexture(GL_TEXTURE_2D, display.id); checkGlError();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, flip ? GL_RGBA : GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
        checkGlError();
    }
    log("renderer_update_root w:%d h:%d data:%p flip:%d display.width=%f display.height:%f",
        w, h, data, flip, display.width, display.height );

}

void
renderer_update_root_process1(int x, int y, int w, int h, void *data, uint8_t flip, int index) {
    if (eglGetCurrentContext() == EGL_NO_CONTEXT || !w || !h) {
        return;
    }
    WindAttribute*  windAttributePtr = (WindAttribute *) node_get_at_index(NamedWindow_WindowPtr, index);
    log("renderer_update_root_process1 x:%d y:%d offsetx:%f offsety:%f data:%p flip:%d display.width=%f display.height:%f index:%d id:%d",
        x, y, windAttributePtr->offset_x, windAttributePtr->offset_y, data, flip, windAttributePtr->width, windAttributePtr->height, index, windAttributePtr->texture_id );

    windAttributePtr->offset_x = (float) x;
    windAttributePtr->offset_y = (float) y;

    if (windAttributePtr->width != (float) w || windAttributePtr->height != (float) h) {
        windAttributePtr->width = (float) w;
        windAttributePtr->height = (float) h;
        if(!windAttributePtr->texture_id){
            glGenTextures(1, &windAttributePtr->texture_id); checkGlError();
        }
        glBindTexture(GL_TEXTURE_2D, windAttributePtr->texture_id);
        checkGlError();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        checkGlError();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        checkGlError();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        checkGlError();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        checkGlError();
        glTexImage2D(GL_TEXTURE_2D, 0, flip ? GL_RGBA : GL_BGRA_EXT, w, h, 0,
                     flip ? GL_RGBA : GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
        checkGlError();
    } else {
        glBindTexture(GL_TEXTURE_2D, windAttributePtr->texture_id);
        checkGlError();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, flip ? GL_RGBA : GL_BGRA_EXT,
                        GL_UNSIGNED_BYTE, data);
        checkGlError();
    }
}

void renderer_update_cursor(int w, int h, int xhot, int yhot, void* data) {
//    log("Xlorie: updating cursor w:%d  h:%d xhot:%d yhot:%d \n", w, h, xhot, yhot);
    cursor.width = (float) w;
    cursor.height = (float) h;
    cursor.xhot = (float) xhot;
    cursor.yhot = (float) yhot;

    if (eglGetCurrentContext() == EGL_NO_CONTEXT || !cursor.width || !cursor.height)
        return;

    glBindTexture(GL_TEXTURE_2D, cursor.id); checkGlError();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); checkGlError();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); checkGlError();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); checkGlError();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); checkGlError();

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data); checkGlError();
}

void renderer_set_cursor_coordinates(int x, int y) {
//    log("set_cursor x:%d, y :%d", x , y);
    cursor.x = (float) x;
    cursor.y = (float) y;
}

static void draw(GLuint id, float x0, float y0, float x1, float y1, uint8_t flip);
static void draw_cursor(int index);

float ia = 0;

int renderer_should_redraw(void) {
    int egl_good = sfc != EGL_NO_SURFACE && eglGetCurrentContext() != EGL_NO_CONTEXT;
//    log("renderer_should_redraw egl_good:%d" , egl_good);
    return 1;
}

int renderer_redraw(JNIEnv* env, uint8_t flip) {
    int err_traversal = TRUE;
//    err_traversal = renderer_redraw_traversal(env, flip, 0);
    WindowNode* node = NamedWindow_WindowPtr;
    while (node){
        err_traversal = renderer_redraw_traversal(env, flip, node->data.index);
        node = node->next;
    }
    renderedFrames++;
    return TRUE;
}


int renderer_redraw_traversal(JNIEnv* env, uint8_t flip, int index) {
    int err = EGL_SUCCESS;
    EGLSurface eglSurface = NULL;
    int id;
    float width, height;
    WindowNode * windowNode = node_get_at_index(NamedWindow_WindowPtr, index);
//    if (index == 0){
//        eglSurface = sfc;
//        id = display.id;
//        width = display.width;
//        height = display.height;
//    } else
        if(windowNode){
        UpdateBuffer(index);
        eglSurface = windowNode->data.sfc;
        id = windowNode->data.texture_id;
        width = windowNode->data.width;
        height = windowNode->data.height;
    }
//    log("renderer_redraw_traversal eglSurface:%p index:%d width:%f height:%f id:%d", eglSurface, index, width, height, id);
    if (!eglSurface || eglGetCurrentContext() == EGL_NO_CONTEXT || !id) {
        return FALSE;
    }

    glViewport(0, 0, width, height); checkGlError();
    if (eglMakeCurrent(global_egl_display, eglSurface, eglSurface, global_ctx) != EGL_TRUE) {
        log("Xlorie: eglMakeCurrent failed.\n");
        eglCheckError(__LINE__);
    }
    draw(id,  -1.f, -1.f, 1.f, 1.f, flip);
    draw_cursor(index);
    if (eglSwapBuffers(global_egl_display, eglSurface) != EGL_TRUE) {
        err = eglGetError();
        eglCheckError(__LINE__);
        if (err == EGL_BAD_NATIVE_WINDOW || err == EGL_BAD_SURFACE) {
            log("We've got %s so window is to be destroyed. "
                "Native window disconnected/abandoned, probably activity is destroyed or in background",
                eglErrorLabel(err));
//            renderer_clear_window(env, index);
//            renderer_set_window(env, NULL, NULL);
            return FALSE;
        }
    }
    return TRUE;
}

void renderer_print_fps(float millis) {
    if (renderedFrames)
        log("%d frames in %.1f seconds = %.1f FPS",
                                renderedFrames, millis / 1000, (float) renderedFrames *  1000 / millis);
    renderedFrames = 0;
}

static GLuint load_shader(GLenum shaderType, const char* pSource) {
    GLint compiled = 0;
    GLuint shader = glCreateShader(shaderType); checkGlError();
    if (shader) {
        glShaderSource(shader, 1, &pSource, NULL); checkGlError();
        glCompileShader(shader); checkGlError();
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled); checkGlError();
        if (!compiled) {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen); checkGlError();
            if (infoLen) {
                char* buf = (char*) malloc(infoLen);
                if (buf) {
                    glGetShaderInfoLog(shader, infoLen, NULL, buf); checkGlError();
                    log("Xlorie: Could not compile shader %d:\n%s\n", shaderType, buf);
                    free(buf);
                }
                glDeleteShader(shader); checkGlError();
                shader = 0;
            }
        }
    }
    return shader;
}

static GLuint create_program(const char* p_vertex_source, const char* p_fragment_source) {
    GLuint program, vertexShader, pixelShader;
    GLint linkStatus = GL_FALSE;
    vertexShader = load_shader(GL_VERTEX_SHADER, p_vertex_source);
    pixelShader = load_shader(GL_FRAGMENT_SHADER, p_fragment_source);
    if (!pixelShader || !vertexShader) {
        return 0;
    }

    program = glCreateProgram(); checkGlError();
    if (program) {
        glAttachShader(program, vertexShader); checkGlError();
        glAttachShader(program, pixelShader); checkGlError();
        glLinkProgram(program); checkGlError();
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus); checkGlError();
        if (linkStatus != GL_TRUE) {
            GLint bufLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength); checkGlError();
            if (bufLength) {
                char* buf = (char*) malloc(bufLength);
                if (buf) {
                    glGetProgramInfoLog(program, bufLength, NULL, buf); checkGlError();
                    log("Xlorie: Could not link program:\n%s\n", buf);
                    free(buf);
                }
            }
            glDeleteProgram(program); checkGlError();
            program = 0;
        }
    }
    return program;
}

static void draw(GLuint id, float x0, float y0, float x1, float y1, uint8_t flip) {
    float coords[20] = {
        x0, -y0, 0.f, 0.f, 0.f,
        x1, -y0, 0.f, 1.f, 0.f,
        x0, -y1, 0.f, 0.f, 1.f,
        x1, -y1, 0.f, 1.f, 1.f,
    };

    GLuint p = flip ? gv_pos_bgra : gv_pos, c = flip ? gv_coords_bgra : gv_coords;

    glActiveTexture(GL_TEXTURE0); checkGlError();
    glUseProgram(flip ? g_texture_program_bgra : g_texture_program); checkGlError();
    glBindTexture(GL_TEXTURE_2D, id); checkGlError();

    glVertexAttribPointer(p, 3, GL_FLOAT, GL_FALSE, 20, coords); checkGlError();
    glVertexAttribPointer(c, 2, GL_FLOAT, GL_FALSE, 20, &coords[3]); checkGlError();
    glEnableVertexAttribArray(p); checkGlError();
    glEnableVertexAttribArray(c); checkGlError();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); checkGlError();
}

maybe_unused static void draw_cursor(int index) {
    float x, y, w, h;

    if (!cursor.width || !cursor.height)
        return;

    float width = display.width;
    float height = display.height;
    float cursor_x = cursor.x;
    float cursor_y = cursor.y;
    float cursor_xhot = cursor.xhot;
    float cursor_yhot = cursor.yhot;
    if (index != 0) {
        WindowNode * window_node = node_get_at_index( NamedWindow_WindowPtr, index);
        if(window_node){
            width = window_node->data.width;
            height = window_node->data.height;
            cursor_x -= window_node->data.offset_x;
            cursor_y -= window_node->data.offset_y;
        }
    }
    x = 2.f * (cursor_x - cursor_xhot) / width - 1.f;
    y = 2.f * (cursor_y - cursor_yhot) / height - 1.f;
    w = 2.f * cursor.width / width;
    h = 2.f * cursor.height / height;
    glEnable(GL_BLEND); checkGlError();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); checkGlError();
    draw(cursor.id, x, y, x + w, y + h, false);
    glDisable(GL_BLEND); checkGlError();
}

