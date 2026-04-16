#ifndef DASHCDG_GL_RENDERER_H
#define DASHCDG_GL_RENDERER_H

#include <GL/glew.h>

#include "dashcdg/cdg.h"

struct dashcdg_gl_renderer {
    GLuint program;
    GLuint texture_id;
    GLint rgba_sampler_location;
};

int dashcdg_gl_renderer_init(struct dashcdg_gl_renderer *renderer);
void dashcdg_gl_renderer_resize(struct dashcdg_gl_renderer *renderer, int width, int height);
void dashcdg_gl_renderer_render(struct dashcdg_gl_renderer *renderer, const struct dashcdg_cdg_state *state);

#endif
