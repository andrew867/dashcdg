#include "dashcdg/gl_renderer.h"

#include <stdio.h>
#include <string.h>

#include <GL/glut.h>

static const char *DASHCDG_VERTEX_SHADER =
        "#version 130\n"
        "out vec2 vertexCoord;\n"
        "void main() {\n"
        "    vertexCoord = gl_Vertex.xy;\n"
        "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
        "}\n";

static const char *DASHCDG_FRAGMENT_SHADER =
        "#version 130\n"
        "#extension GL_EXT_gpu_shader4 : enable\n"
        "uniform int cdgColorMap[16];\n"
        "uniform int cdgTransparencyMap[16];\n"
        "uniform sampler2D cdgFramebuffer;\n"
        "uniform int cdgViewportX;\n"
        "uniform int cdgViewportY;\n"
        "uniform int cdgOffsetX;\n"
        "uniform int cdgOffsetY;\n"
        "in vec2 vertexCoord;\n"
        "void main() {\n"
        "    int x = (int(vertexCoord.x) + cdgViewportX + cdgOffsetX) % 300;\n"
        "    int y = (int(vertexCoord.y) + cdgViewportY + cdgOffsetY) % 216;\n"
        "    if (x < 0) { x += 300; }\n"
        "    if (y < 0) { y += 216; }\n"
        "    int colorIndex = int(texelFetch(cdgFramebuffer, ivec2(x, y), 0).r * 255.0 + 0.5);\n"
        "    int rgb = cdgColorMap[colorIndex];\n"
        "    float alpha = 1.0 - (float(cdgTransparencyMap[colorIndex]) / 63.0);\n"
        "    gl_FragColor = vec4(\n"
        "        float((rgb >> 16) & 0xFF) / 255.0,\n"
        "        float((rgb >> 8) & 0xFF) / 255.0,\n"
        "        float(rgb & 0xFF) / 255.0,\n"
        "        alpha\n"
        "    );\n"
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

    renderer->color_table_location = glGetUniformLocation(renderer->program, "cdgColorMap");
    renderer->transparency_location = glGetUniformLocation(renderer->program, "cdgTransparencyMap");
    renderer->framebuffer_location = glGetUniformLocation(renderer->program, "cdgFramebuffer");
    renderer->offset_x_location = glGetUniformLocation(renderer->program, "cdgOffsetX");
    renderer->offset_y_location = glGetUniformLocation(renderer->program, "cdgOffsetY");
    {
        GLint viewport_x_location = glGetUniformLocation(renderer->program, "cdgViewportX");
        GLint viewport_y_location = glGetUniformLocation(renderer->program, "cdgViewportY");
        if (viewport_x_location == -1 || viewport_y_location == -1) {
            fprintf(stderr, "failed to get viewport uniform locations\n");
            return 0;
        }
        glUseProgram(renderer->program);
        glUniform1i(viewport_x_location, DASHCDG_VISIBLE_X);
        glUniform1i(viewport_y_location, DASHCDG_VISIBLE_Y);
        glUseProgram(0);
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
    int transparency[DASHCDG_COLORS];

    if (renderer == NULL || state == NULL) {
        return;
    }

    for (int i = 0; i < DASHCDG_COLORS; ++i) {
        transparency[i] = state->transparency[i];
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(renderer->program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, renderer->texture_id);
    glEnable(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_R8,
            DASHCDG_SCREEN_WIDTH,
            DASHCDG_SCREEN_HEIGHT,
            0,
            GL_RED,
            GL_UNSIGNED_BYTE,
            state->framebuffer
    );

    glUniform1i(renderer->framebuffer_location, 0);
    glUniform1iv(renderer->color_table_location, DASHCDG_COLORS, state->color_table);
    glUniform1iv(renderer->transparency_location, DASHCDG_COLORS, transparency);
    glUniform1i(renderer->offset_x_location, state->display_h_offset);
    glUniform1i(renderer->offset_y_location, state->display_v_offset);

    glBegin(GL_QUADS);
    glVertex2f(0.0f, 0.0f);
    glVertex2f((float) DASHCDG_VISIBLE_WIDTH, 0.0f);
    glVertex2f((float) DASHCDG_VISIBLE_WIDTH, (float) DASHCDG_VISIBLE_HEIGHT);
    glVertex2f(0.0f, (float) DASHCDG_VISIBLE_HEIGHT);
    glEnd();

    glFlush();
    glutSwapBuffers();
}
