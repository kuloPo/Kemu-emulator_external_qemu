/* Functional pixel/copy checks only: no timing or benchmarking.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c)) {                                                            \
            fprintf(stderr, "FAIL line %d: %s (EGL=%x GL=%x)\n", __LINE__, #c, \
                    eglGetError(), glGetError());                              \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
static void *(*volatile copyfn)(void *, const void *, size_t) = memcpy;
static void *(*volatile setfn)(void *, int, size_t) = memset;

static void test_libc(const char *resource)
{
    const unsigned sizes[] = {0,   1,   2,   3,   4,   7,    8,   15,
                              16,  17,  31,  32,  33,  63,   64,  65,
                              127, 128, 129, 255, 256, 1024, 4097};
    uint8_t input[8192], output[8192];
    volatile uint8_t *mmio;
    unsigned i, j, n;
    int fd = open(resource, O_RDWR | O_SYNC);
    CHECK(fd >= 0);
    mmio = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(mmio != MAP_FAILED);
    for (i = 0; i < sizeof(input); ++i) {
        input[i] = (uint8_t)(i * 37 + 11);
    }
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        n = sizes[i];
        for (j = 0; j < n + 2; ++j) {
            mmio[j] = 0xa5;
        }
        CHECK(copyfn((void *)(mmio + 1), input, n) == (void *)(mmio + 1));
        for (j = 0; j < n; ++j) {
            CHECK(mmio[j + 1] == input[j]);
        }
        copyfn(output, (void *)(mmio + 1), n);
        CHECK(!memcmp(output, input, n));
        setfn((void *)(mmio + 1), 0x6b, n);
        for (j = 0; j < n; ++j) {
            CHECK(mmio[j + 1] == 0x6b);
        }
        setfn((void *)(mmio + 1), 0, n);
        for (j = 0; j < n; ++j) {
            CHECK(mmio[j + 1] == 0);
        }
        CHECK(mmio[0] == 0xa5 && mmio[n + 1] == 0xa5);
    }
    munmap((void *)mmio, 8192);
    close(fd);
    puts("PASS real libc MMIO memcpy both directions, nonzero/zero memset: 23 "
         "sizes");
}

int main(int argc, char **argv)
{
    EGLDisplay display;
    EGLConfig config;
    EGLint count;
    EGLSurface surface;
    EGLContext context;
    const EGLint config_attrs[] = {EGL_SURFACE_TYPE,
                                   EGL_PBUFFER_BIT,
                                   EGL_RENDERABLE_TYPE,
                                   EGL_OPENGL_ES3_BIT,
                                   EGL_RED_SIZE,
                                   8,
                                   EGL_GREEN_SIZE,
                                   8,
                                   EGL_BLUE_SIZE,
                                   8,
                                   EGL_ALPHA_SIZE,
                                   8,
                                   EGL_NONE};
    const EGLint surface_attrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const unsigned widths[] = {1, 3, 7, 16, 31, 64, 129};
    GLuint texture, framebuffer, buffer;
    void *(*map_range)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
    GLboolean (*unmap)(GLenum);
    uint8_t input[129 * 17 * 4], output[sizeof(input)];
    unsigned i, j;
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef __aarch64__
    puts("ABI: ARM64 via NativeBridge");
#else
    puts("ABI: x86_64 native");
#endif
    if (argc > 1) {
        test_libc(argv[1]);
    }
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    CHECK(eglInitialize(display, NULL, NULL));
    CHECK(eglChooseConfig(display, config_attrs, &config, 1, &count) &&
          count == 1);
    surface = eglCreatePbufferSurface(display, config, surface_attrs);
    CHECK(surface != EGL_NO_SURFACE);
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    CHECK(context != EGL_NO_CONTEXT);
    CHECK(eglMakeCurrent(display, surface, surface, context));
    map_range = (void *(*)(GLenum, GLintptr, GLsizeiptr,
                           GLbitfield))eglGetProcAddress("glMapBufferRange");
    unmap = (GLboolean (*)(GLenum))eglGetProcAddress("glUnmapBuffer");
    CHECK(map_range && unmap);
    printf("Renderer: %s\nVersion: %s\n", glGetString(GL_RENDERER),
           glGetString(GL_VERSION));
    CHECK(strstr((const char *)glGetString(GL_VERSION), "ANGLE"));
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenBuffers(1, &buffer);
    for (i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        unsigned width = widths[i], size = width * 17 * 4;
        void *mapped;
        for (j = 0; j < size; ++j) {
            input[j] = (uint8_t)(j * 37 + i);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, 17, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, input);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture, 0);
        CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
              GL_FRAMEBUFFER_COMPLETE);
        glReadPixels(0, 0, width, 17, GL_RGBA, GL_UNSIGNED_BYTE, output);
        CHECK(glGetError() == GL_NO_ERROR && !memcmp(input, output, size));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, size, NULL, GL_STREAM_DRAW);
        mapped = map_range(GL_PIXEL_UNPACK_BUFFER, 0, size,
                           GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        CHECK(mapped);
        setfn(mapped, 0, size);
        copyfn(mapped, input, size);
        CHECK(unmap(GL_PIXEL_UNPACK_BUFFER));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 17, GL_RGBA,
                        GL_UNSIGNED_BYTE, 0);
        glReadPixels(0, 0, width, 17, GL_RGBA, GL_UNSIGNED_BYTE, output);
        CHECK(glGetError() == GL_NO_ERROR && !memcmp(input, output, size));
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(0, 0, width, 17, GL_RGBA, GL_UNSIGNED_BYTE, output);
        CHECK(glGetError() == GL_NO_ERROR);
        for (j = 0; j < size; ++j) {
            CHECK(output[j] == 0);
        }
    }
    glFinish();
    CHECK(glGetError() == GL_NO_ERROR);
    glDeleteBuffers(1, &buffer);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    puts("PASS ANGLE texture upload, mapped GPU buffer memcpy/memset, "
         "subimage, clear and exact pixel readback: 7 sizes");
    return 0;
}
