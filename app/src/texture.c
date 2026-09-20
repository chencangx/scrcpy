#include "texture.h"

#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include "util/log.h"

#define SC_TEXTURE_FMT_PROCESSED SDL_PIXELFORMAT_BGRA32

/*
 * Compute the luminance of a pixel expressed in linear [0, 1] rgb, using the
 * Rec.601 weights (identical floating point weights for the three channels).
 */
static inline float
sc_texture_luminance(float r, float g, float b) {
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

/* clang-format off */
/* Standard smoothstep: 0 if x <= 0, 1 if x >= 1, smooth otherwise */
static inline float
sc_texture_smoothstep(float x) {
    x = x > 0.f ? x : 0.f;
    x = x < 1.f ? x : 1.f;
    return x * x * (3.f - 2.f * x);
}
/* clang-format on */

/*
 * Apply the video effects on the current frame (grayscale and/or
 * luminance-to-alpha).
 *
 * The frame has been converted by libswscale to BGRA8888, stored in
 * tex->sws_buf. The processed pixels are written back in place and are
 * premultiplied by their alpha value, ready for the
 * SDL_BLENDMODE_PREMULTIPLIED blend mode.
 *
 * When alpha is fully opaque (1.0), premultiply is the identity, so this
 * handles both the filtered and unfiltered cases uniformly.
 */
static void
sc_texture_process_frame(struct sc_texture *tex) {
    uint8_t *data = tex->sws_buf->data;
    assert(data);

    size_t pitch = (size_t) tex->texture_size.width * 4;

    uint32_t filter_flags = tex->filter_flags;
    bool premultiplied = tex->premultiplied;

    float threshold = tex->luminance_threshold;
    float edge = tex->luminance_edge;
    float low = threshold - edge;

    for (uint32_t y = 0; y < tex->texture_size.height; ++y) {
        uint8_t *pix = data + y * pitch;
        for (uint32_t x = 0; x < tex->texture_size.width; ++x) {
            // BGRA order
            float b = pix[0] / 255.f;
            float g = pix[1] / 255.f;
            float r = pix[2] / 255.f;
            float a = pix[3] / 255.f;

            if (filter_flags & SC_TEXTURE_FILTER_GRAYSCALE) {
                float gray = sc_texture_luminance(r, g, b);
                r = gray;
                g = gray;
                b = gray;
            }

            if (filter_flags & SC_TEXTURE_FILTER_TRANSPARENT_WHITE) {
                float gray = sc_texture_luminance(r, g, b);
                float fade;
                if (gray >= threshold) {
                    fade = 1.f; // fully transparent
                } else if (gray <= low) {
                    fade = 0.f; // fully opaque
                } else {
                    // Anti-aliased text edges fade smoothly here
                    fade = sc_texture_smoothstep((gray - low) / edge);
                }
                a *= 1.f - fade;
            }

            // Clamp in case of floating point rounding errors
            a = a > 1.f ? 1.f : a;

            uint8_t alpha = (uint8_t) (a * 255.f + 0.5f);
            if (premultiplied) {
                // premultiply the color by alpha
                pix[0] = (uint8_t) (b * a * 255.f + 0.5f);
                pix[1] = (uint8_t) (g * a * 255.f + 0.5f);
                pix[2] = (uint8_t) (r * a * 255.f + 0.5f);
                pix[3] = alpha;
            } else {
                (void) alpha;
                pix[0] = (uint8_t) (b * 255.f + 0.5f);
                pix[1] = (uint8_t) (g * 255.f + 0.5f);
                pix[2] = (uint8_t) (r * 255.f + 0.5f);
                pix[3] = 255; // opaque
            }

            pix += 4;
        }
    }
}

void
sc_texture_set_luminance_params(struct sc_texture *tex, float threshold,
                                float edge) {
    tex->luminance_threshold = threshold;
    tex->luminance_edge = edge;
}

bool
sc_texture_get_luminance_params(const struct sc_texture *tex, float *threshold,
                                float *edge) {
    *threshold = tex->luminance_threshold;
    *edge = tex->luminance_edge;
    return true;
}

bool
sc_texture_init(struct sc_texture *tex, SDL_Renderer *renderer, bool mipmaps,
                uint8_t filter_flags, float luminance_threshold,
                float luminance_edge) {
    const char *renderer_name = SDL_GetRendererName(renderer);
    LOGI("Renderer: %s", renderer_name ? renderer_name : "(unknown)");

    tex->mipmaps = false;

    // starts with "opengl"
    bool use_opengl = renderer_name && !strncmp(renderer_name, "opengl", 6);
    if (use_opengl) {
        struct sc_opengl *gl = &tex->gl;
        sc_opengl_init(gl);

        LOGI("OpenGL version: %s", gl->version);

        if (mipmaps) {
            bool supports_mipmaps =
                sc_opengl_version_at_least(gl, 3, 0, /* OpenGL 3.0+ */
                                               2, 0  /* OpenGL ES 2.0+ */);
            if (supports_mipmaps) {
                LOGI("Trilinear filtering enabled");
                tex->mipmaps = true;
            } else {
                LOGW("Trilinear filtering disabled "
                     "(OpenGL 3.0+ or ES 2.0+ required)");
            }
        } else {
            LOGI("Trilinear filtering disabled");
        }
    } else if (mipmaps) {
        LOGD("Trilinear filtering disabled (not an OpenGL renderer)");
    }

    tex->renderer = renderer;
    tex->texture = NULL;

    tex->filter_flags = filter_flags;
    tex->premultiplied = false;
    tex->luminance_threshold = luminance_threshold;
    tex->luminance_edge = luminance_edge;

    tex->sws = NULL;
    tex->sws_buf = NULL;

    return true;
}

void
sc_texture_destroy(struct sc_texture *tex) {
    if (tex->texture) {
        SDL_DestroyTexture(tex->texture);
    }
    if (tex->sws_buf) {
        av_buffer_unref(&tex->sws_buf);
    }
    if (tex->sws) {
        sws_freeContext(tex->sws);
    }
}

static enum SDL_Colorspace
sc_texture_to_sdl_color_space(enum AVColorSpace color_space,
                              enum AVColorRange color_range) {
    bool full_range = color_range == AVCOL_RANGE_JPEG;

    switch (color_space) {
        case AVCOL_SPC_BT709:
        case AVCOL_SPC_RGB:
        case AVCOL_SPC_UNSPECIFIED:
        case AVCOL_SPC_YCGCO:
            return full_range ? SDL_COLORSPACE_BT709_FULL
                              : SDL_COLORSPACE_BT709_LIMITED;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return full_range ? SDL_COLORSPACE_BT601_FULL
                              : SDL_COLORSPACE_BT601_LIMITED;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return full_range ? SDL_COLORSPACE_BT2020_FULL
                              : SDL_COLORSPACE_BT2020_LIMITED;
        default:
            return SDL_COLORSPACE_JPEG;
    }
}

// Whether the frame filtering is enabled and needs libswscale processing
static inline bool
sc_texture_is_filtered(const struct sc_texture *tex) {
    return tex->filter_flags != 0;
}

// Whether the frame filtering requires alpha blending (transparent white)
static inline bool
sc_texture_has_alpha(const struct sc_texture *tex) {
    return tex->filter_flags & SC_TEXTURE_FILTER_TRANSPARENT_WHITE;
}

static SDL_Texture *
sc_texture_create_frame_texture(struct sc_texture *tex,
                                struct sc_size size,
                                enum AVColorSpace color_space,
                                enum AVColorRange color_range) {
    LOGV("Creating new texture: size=%" PRIu16 "x%" PRIu16 " color_space=%d "
         "color_range=%d", size.width, size.height, color_space, color_range);

    assert(size.width && size.height);

    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        return NULL;
    }

    bool ok;
    if (sc_texture_is_filtered(tex)) {
        // The frames are converted to BGRA8888 and filtered on the CPU
        ok = SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                                   SC_TEXTURE_FMT_PROCESSED);
    } else {
        // Pass the native YV12 frames directly to the renderer
        ok = SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                                   SDL_PIXELFORMAT_YV12);
    }
    ok &= SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                SDL_TEXTUREACCESS_STREAMING);
    ok &= SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                                size.width);
    ok &= SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                                size.height);
    if (!sc_texture_is_filtered(tex)) {
        // The colorspace is irrelevant once the conversion is done on the CPU
        enum SDL_Colorspace sdl_color_space =
            sc_texture_to_sdl_color_space(color_space, color_range);
        ok &= SDL_SetNumberProperty(props,
                                    SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                                    sdl_color_space);
    }

    if (!ok) {
        LOGE("Could not set texture properties");
        SDL_DestroyProperties(props);
        return NULL;
    }

    SDL_Renderer *renderer = tex->renderer;
    SDL_Texture *texture = SDL_CreateTextureWithProperties(renderer, props);
    SDL_DestroyProperties(props);
    if (!texture) {
        LOGD("Could not create texture: %s", SDL_GetError());
        return NULL;
    }

    if (tex->premultiplied) {
        SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
        if (sc_texture_has_alpha(tex)) {
            // The buffer stores premultiplied pixels (see
            // sc_texture_process_frame()) so that dark text blends correctly
            // over both bright and dark surroundings.
            SDL_BlendMode custom = SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                SDL_BLENDOPERATION_ADD);
            blend_mode = custom;
        }

        if (SDL_SetTextureBlendMode(texture, blend_mode)) {
            LOGW("Could not set texture blend mode: %s", SDL_GetError());
        }
    }

    if (tex->mipmaps) {
        struct sc_opengl *gl = &tex->gl;

        SDL_PropertiesID props = SDL_GetTextureProperties(texture);
        if (!props) {
            LOGE("Could not get texture properties: %s", SDL_GetError());
            SDL_DestroyTexture(texture);
            return NULL;
        }

        const char *renderer_name = SDL_GetRendererName(tex->renderer);
        const char *key = !renderer_name || !strcmp(renderer_name, "opengl")
                        ? SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER
                        : SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER;

        int64_t texture_id = SDL_GetNumberProperty(props, key, 0);
        SDL_DestroyProperties(props);
        if (!texture_id) {
            LOGE("Could not get texture id: %s", SDL_GetError());
            SDL_DestroyTexture(texture);
            return NULL;
        }

        assert(!(texture_id & ~0xFFFFFFFF)); // fits in uint32_t
        tex->texture_id = texture_id;
        gl->BindTexture(GL_TEXTURE_2D, tex->texture_id);

        // Enable trilinear filtering for downscaling
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                          GL_LINEAR_MIPMAP_LINEAR);
        gl->TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -1.f);

        gl->BindTexture(GL_TEXTURE_2D, 0);
    }

    return texture;
}

static int
sc_texture_impl_swscale_color_space(enum AVColorSpace color_space) {
    switch (color_space) {
        case AVCOL_SPC_BT709:
            return SWS_CS_ITU709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return SWS_CS_ITU601;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return SWS_CS_BT2020;
        case AVCOL_SPC_UNSPECIFIED:
        case AVCOL_SPC_RGB:
        default:
            // The server historically encodes in BT.601, which happens to also
            // be the default conversion matrix of libswscale.
            return SWS_CS_DEFAULT;
    }
}

static bool
sc_texture_resize_sws_buf(struct sc_texture *tex) {
    struct sc_size size = tex->texture_size;
    size_t needed = (size_t) size.width * size.height * 4;
    if (needed == 0) {
        return false;
    }

    size_t current = tex->sws_buf ? (size_t) tex->sws_buf->size : 0;
    if (current >= needed) {
        return true;
    }

    if (tex->sws_buf) {
        av_buffer_unref(&tex->sws_buf);
    }

    tex->sws_buf = av_buffer_alloc(needed);
    if (!tex->sws_buf) {
        LOG_OOM();
        return false;
    }

    return true;
}

bool
sc_texture_set_from_frame(struct sc_texture *tex, const AVFrame *frame) {

    struct sc_size size = {frame->width, frame->height};
    assert(size.width && size.height);

    if (!tex->texture
            || tex->texture_type != SC_TEXTURE_TYPE_FRAME
            || tex->texture_size.width != size.width
            || tex->texture_size.height != size.height) {
        // Incompatible texture, recreate it
        enum AVColorSpace color_space = frame->colorspace;
        enum AVColorRange color_range = frame->color_range;

        if (tex->texture) {
            SDL_DestroyTexture(tex->texture);
            tex->texture = NULL;
        }

        if (tex->sws) {
            sws_freeContext(tex->sws);
            tex->sws = NULL;
        }

        tex->texture_size = size;

        // Compute the blending flags once per texture (re)creation
        tex->premultiplied = sc_texture_has_alpha(tex);

        tex->texture = sc_texture_create_frame_texture(tex, size, color_space,
                                                       color_range);
        if (!tex->texture) {
            return false;
        }

        tex->texture_type = SC_TEXTURE_TYPE_FRAME;

        if (sc_texture_is_filtered(tex)) {
            if (!sc_texture_resize_sws_buf(tex)) {
                return false;
            }

            enum AVPixelFormat pix_fmt = frame->format;
            bool full_range = color_range == AVCOL_RANGE_JPEG;
            int cs = sc_texture_impl_swscale_color_space(color_space);
            tex->sws = sws_getContext(
                size.width, size.height, pix_fmt,
                size.width, size.height, AV_PIX_FMT_BGRA,
                SWS_BITEXACT, NULL, NULL, NULL);
            if (!tex->sws) {
                LOGE("Could not create scaling context");
                return false;
            }

            sws_setColorspaceDetails(tex->sws, sws_getCoefficients(cs),
                                     full_range, sws_getCoefficients(cs),
                                     false, 0, 1 << 16, 1 << 16);
            int sws_ret = sws_init_context(tex->sws, NULL, NULL);
            if (sws_ret < 0) {
                LOGE("Could not initialize scaling context");
                return false;
            }
        }

        LOGI("Texture: %" PRIu16 "x%" PRIu16, size.width, size.height);
    }

    assert(tex->texture);
    assert(tex->texture_type == SC_TEXTURE_TYPE_FRAME);

    bool ok;
    if (sc_texture_is_filtered(tex)) {
        assert(tex->sws);
        assert(tex->sws_buf);
        assert(frame->data[0] && frame->data[1] && frame->data[2]);

        // Convert the YUV frame to BGRA8888 (tightly packed, no padding)
        uint8_t *dst[] = {tex->sws_buf->data};
        int dst_stride[] = {(int) size.width * 4};

        int ret = sws_scale(tex->sws, (const uint8_t *const *) frame->data,
                            frame->linesize, 0, size.height, dst, dst_stride);
        if (ret < 0) {
            LOGD("Could not convert frame: %d", ret);
            return false;
        }

        sc_texture_process_frame(tex);

        ok = SDL_UpdateTexture(tex->texture, NULL, tex->sws_buf->data,
                               dst_stride[0]);
    } else {
        ok = SDL_UpdateYUVTexture(tex->texture, NULL,
                                  frame->data[0], frame->linesize[0],
                                  frame->data[1], frame->linesize[1],
                                  frame->data[2], frame->linesize[2]);
    }
    if (!ok) {
        LOGD("Could not update texture: %s", SDL_GetError());
        return false;
    }

    if (tex->mipmaps) {
        assert(tex->texture_id);
        struct sc_opengl *gl = &tex->gl;

        gl->BindTexture(GL_TEXTURE_2D, tex->texture_id);
        gl->GenerateMipmap(GL_TEXTURE_2D);
        gl->BindTexture(GL_TEXTURE_2D, 0);
    }

    return true;
}

bool
sc_texture_set_from_surface(struct sc_texture *tex, SDL_Surface *surface) {
    if (tex->texture) {
        SDL_DestroyTexture(tex->texture);
    }

    tex->texture = SDL_CreateTextureFromSurface(tex->renderer, surface);
    if (!tex->texture) {
        LOGE("Could not create texture: %s", SDL_GetError());
        return false;
    }

    tex->texture_size.width = surface->w;
    tex->texture_size.height = surface->h;
    tex->texture_type = SC_TEXTURE_TYPE_ICON;

    return true;
}

void
sc_texture_reset(struct sc_texture *tex) {
    if (tex->texture) {
        SDL_DestroyTexture(tex->texture);
        tex->texture = NULL;
    }
}
