#include "dashcdg/gl_renderer.h"

#include <stdio.h>
#include <string.h>

#include <GL/glut.h>

#include "dashcdg/cdg_raster.h"
#include "dashcdg/common.h"

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(DASHCDG_VISIBLE_WIDTH == 288, "gl_renderer fragment shader u-scale must match DASHCDG_VISIBLE_WIDTH");
_Static_assert(DASHCDG_VISIBLE_HEIGHT == 192, "gl_renderer fragment shader v-scale must match DASHCDG_VISIBLE_HEIGHT");
#endif

static const char *DASHCDG_VERTEX_SHADER =
        "#version 130\n"
        "out vec2 vertexCoord;\n"
        "void main() {\n"
        "    vertexCoord = gl_Vertex.xy;\n"
        "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
        "}\n";

static const char *DASHCDG_FRAGMENT_SHADER =
        "#version 130\n"
        "uniform sampler2D cdgRgba;\n"
        "in vec2 vertexCoord;\n"
        "void main() {\n"
        "    float u = (vertexCoord.x + 0.5) / 288.0;\n"
        "    float v = (vertexCoord.y + 0.5) / 192.0;\n"
        "    gl_FragColor = texture2D(cdgRgba, vec2(u, v));\n"
        "}\n";

static GLuint dashcdg_compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    GLint status = GL_FALSE;
    char buffer[512];

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

    if (status != GL_TRUE) {
        glGetShaderInfoLog(shader, sizeof(buffer), NULL, buffer);
        fprintf(stderr, "shader compile failed: %s\n", buffer);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

int dashcdg_gl_renderer_init(struct dashcdg_gl_renderer *renderer) {
    GLuint vertex_shader;
    GLuint fragment_shader;
    GLint status = GL_FALSE;
    char buffer[512];

    if (renderer == NULL) {
        return 0;
    }

    memset(renderer, 0, sizeof(*renderer));
    vertex_shader = dashcdg_compile_shader(GL_VERTEX_SHADER, DASHCDG_VERTEX_SHADER);
    fragment_shader = dashcdg_compile_shader(GL_FRAGMENT_SHADER, DASHCDG_FRAGMENT_SHADER);
    if (vertex_shader == 0 || fragment_shader == 0) {
        return 0;
    }

    renderer->program = glCreateProgram();
    glAttachShader(renderer->program, vertex_shader);
    glAttachShader(renderer->program, fragment_shader);
    glLinkProgram(renderer->program);
    glGetProgramiv(renderer->program, GL_LINK_STATUS, &status);

    if (status != GL_TRUE) {
        glGetProgramInfoLog(renderer->program, sizeof(buffer), NULL, buffer);
        fprintf(stderr, "shader link failed: %s\n", buffer);
        glDeleteProgram(renderer->program);
        renderer->program = 0;
        return 0;
    }

    glDetachShader(renderer->program, vertex_shader);
    glDetachShader(renderer->program, fragment_shader);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    renderer->rgba_sampler_location = glGetUniformLocation(renderer->program, "cdgRgba");
    if (renderer->rgba_sampler_location == -1) {
        fprintf(stderr, "failed to get cdgRgba uniform location\n");
        glDeleteProgram(renderer->program);
        renderer->program = 0;
        return 0;
    }

    glGenTextures(1, &renderer->texture_id);
    return renderer->program != 0 && renderer->texture_id != 0;
}

void dashcdg_gl_renderer_resize(struct dashcdg_gl_renderer *renderer, int width, int height) {
    (void) renderer;

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glViewport(0, 0, width, height);
    glOrtho(0, DASHCDG_VISIBLE_WIDTH, DASHCDG_VISIBLE_HEIGHT, 0, 0.0, 100.0);
}

void dashcdg_gl_renderer_render(struct dashcdg_gl_renderer *renderer, const struct dashcdg_cdg_state *state) {
    static uint8_t rgba_scratch[DASHCDG_CDG_RGBA_BYTES];

    if (renderer == NULL || state == NULL) {
        return;
    }

    dashcdg_cdg_state_to_rgba8(state, rgba_scratch);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(renderer->program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, renderer->texture_id);
    glEnable(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            DASHCDG_VISIBLE_WIDTH,
            DASHCDG_VISIBLE_HEIGHT,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            rgba_scratch
    );

    glUniform1i(renderer->rgba_sampler_location, 0);

    glBegin(GL_QUADS);
    glVertex2f(0.0f, 0.0f);
    glVertex2f((float) DASHCDG_VISIBLE_WIDTH, 0.0f);
    glVertex2f((float) DASHCDG_VISIBLE_WIDTH, (float) DASHCDG_VISIBLE_HEIGHT);
    glVertex2f(0.0f, (float) DASHCDG_VISIBLE_HEIGHT);
    glEnd();

    glFlush();
}
