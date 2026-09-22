#ifndef SC_DISPLAY_H
#define SC_DISPLAY_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
#include <SDL3/SDL.h>

#include "coords.h"
#include "opengl.h"

enum sc_texture_type {
    SC_TEXTURE_TYPE_FRAME,
    SC_TEXTURE_TYPE_ICON,
};

// Post-processing effects applied on the video frames before upload
enum sc_texture_filter {
    SC_TEXTURE_FILTER_GRAYSCALE = 1 << 0,
    SC_TEXTURE_FILTER_TRANSPARENT_WHITE = 1 << 1,
    SC_TEXTURE_FILTER_BINARY = 1 << 2,
};

struct sc_texture {
    SDL_Renderer *renderer; // owned by the caller
    SDL_Texture *texture;
    // Only valid if texture != NULL
    struct sc_size texture_size;
    enum sc_texture_type texture_type;

    struct sc_opengl gl;

    bool mipmaps;
    uint32_t texture_id; // only set if mipmaps is enabled

    uint8_t filter_flags; // OR of enum sc_texture_filter values

    // The final pixels are premultiplied by their alpha, ready for
    // SDL_BLENDMODE_PREMULTIPLIED
    bool premultiplied;

    // White (luminance) to transparent parameters
    float luminance_threshold;
    float luminance_edge;
    float luminance_opacity; // minimum alpha kept in the transparent areas

    // Binary (black and white) rendering threshold
    float binary_threshold;

    // Frames are converted to BGRA8888 by libswscale
    struct SwsContext *sws;
    // BGRA8888 buffer for the current frame
    AVBufferRef *sws_buf;
};

bool
sc_texture_init(struct sc_texture *tex, SDL_Renderer *renderer, bool mipmaps,
                uint8_t filter_flags, float luminance_threshold,
                float luminance_edge, float luminance_opacity,
                float binary_threshold);

void
sc_texture_set_luminance_params(struct sc_texture *tex, float threshold,
                                float edge, float opacity);

bool
sc_texture_get_luminance_params(const struct sc_texture *tex, float *threshold,
                                float *edge, float *opacity);

void
sc_texture_set_binary_threshold(struct sc_texture *tex, float threshold);

void
sc_texture_destroy(struct sc_texture *tex);

bool
sc_texture_set_from_frame(struct sc_texture *tex, const AVFrame *frame);

bool
sc_texture_set_from_surface(struct sc_texture *tex, SDL_Surface *surface);

void
sc_texture_reset(struct sc_texture *tex);

#endif
