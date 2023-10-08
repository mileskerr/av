#include <SDL2/SDL.h>
#include <SDL2/SDL_keycode.h>
#include <SDL2/SDL_pixels.h>
#include <SDL2/SDL_render.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define MIN(A, B) (A < B) ? (A) : (B)
#define MAX(A, B) (A > B) ? (A) : (B)

#define CL(COLOR) COLOR.r, COLOR.g, COLOR.b, COLOR.a

#define TO_CLOCKS(SECONDS) ((clock_t) (((float) SECONDS) * ((float) CLOCKS_PER_SEC)))
const clock_t REFRESH_TIME = TO_CLOCKS(0.02);





struct ViewerState {
    int window_w;
    int window_h;
    int current_frame;
    bool playing;
    bool frame_changed;
    bool redraw_required;
    bool quit;
};


struct ColorScheme {
    SDL_Color text;
    SDL_Color input_bg;
    SDL_Color bg;
    SDL_Color borders;
    SDL_Color letterboxing;
    SDL_Color timeline_fg;
    SDL_Color timeline_bg;
    SDL_Color timeline_labels;
    SDL_Color current_frame;
    SDL_Color tickmarks_major;
    SDL_Color tickmarks_minor;
};

static struct ColorScheme default_colors(void) {
    return (struct ColorScheme) {
        .text            = (SDL_Color) {0xff, 0xff, 0xff, 0xff},
        .bg              = (SDL_Color) {0x20, 0x20, 0x20, 0xff},
        .input_bg        = (SDL_Color) {0x40, 0x40, 0x40, 0xff},
        .borders         = (SDL_Color) {0x80, 0x80, 0x80, 0xff},
        .letterboxing    = (SDL_Color) {0x00, 0x00, 0x00, 0xff},
        .timeline_fg     = (SDL_Color) {0x80, 0x80, 0xff, 0xff},
        .timeline_bg     = (SDL_Color) {0x20, 0x20, 0x20, 0xff},
        .timeline_labels = (SDL_Color) {0xb0, 0xb0, 0xb0, 0xff},
        .current_frame   = (SDL_Color) {0xff, 0xff, 0xff, 0xff},
        .tickmarks_minor = (SDL_Color) {0x40, 0x40, 0x40, 0xff},
        .tickmarks_major = (SDL_Color) {0x80, 0x80, 0x80, 0xff},
    };
    
}

/* data used to convert frames to a common format
 * sws_context is not used for scaling, only format conversion.
 * any scaling is done using SDL on the gpu */
struct FrameConverter {
    struct SwsContext * sws_context;
    int linesize;
};

static AVCodecContext * open_codec_context(AVFormatContext * format_ctx, int stream_idx) {
    const AVCodecParameters * codecpar = format_ctx->streams[stream_idx]->codecpar;
    const AVCodec * codec = avcodec_find_decoder(codecpar->codec_id);
    AVCodecContext * codec_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codec_ctx, codecpar))
        return NULL;
    if (avcodec_open2(codec_ctx, codec, NULL))
        return NULL;
    return codec_ctx;
}

static struct FrameConverter make_frame_converter(
    const AVCodecContext * const codec_ctx, const int format, const int linesize
) {
    return (struct FrameConverter) {
        sws_getContext(
            codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
            codec_ctx->width, codec_ctx->height, format,
            SWS_POINT, NULL, NULL,
            NULL
        ),
        linesize
    };
}

static void convert_frame(
    struct FrameConverter * frame_conv, AVFrame * frame, uint8_t * pixels
) {
    //TODO: needs an array of linesizes to work with multi-plane images
    sws_scale(
        frame_conv->sws_context, 
        (const uint8_t *const *) frame->data,
        frame->linesize,
        0,
        frame->height,
       &pixels, 
        &frame_conv->linesize
    );    
}

static void destroy_frame_converter(struct FrameConverter * frame_conv) {
    sws_freeContext(frame_conv->sws_context);
}

/* decode a packet containing a single frame and store it in frame_out.
 * returns 0 on success, -1 on failure, -2 on EOF */
static int decode_frame(AVCodecContext * codec_ctx, AVPacket * pkt, AVFrame * frame_out) {
send_packet:
    switch (avcodec_send_packet(codec_ctx, pkt)) {
        case 0: case AVERROR(EAGAIN): case AVERROR_EOF:
            break;
        default:
            return -1;
    }
    switch (avcodec_receive_frame(codec_ctx, frame_out)) {
        case 0:
            break;
        case AVERROR(EAGAIN):
            goto send_packet;
        case AVERROR_EOF:
            return -2;
        default:
            return -1;
    }
    return 0;
}

static int get_texture_pitch(SDL_Texture * texture) {
    int w;
    uint32_t format;
    SDL_QueryTexture(texture, &format, NULL, &w, NULL);
    return (w * SDL_BYTESPERPIXEL(format) + 3) & ~3;
}


static void handle_event(SDL_Event * event, struct ViewerState * state) {
    switch (event->type) {
    case SDL_KEYDOWN:
        switch (event->key.keysym.sym) {
        case SDLK_ESCAPE:
            state->quit = true;
            break;
        case SDLK_SPACE:
            state->playing = !state->playing;
            break;
        }
        break;
    case SDL_QUIT:
        state->quit = true;
        break;
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            state->window_w = event->window.data1;
            state->window_h = event->window.data2;
            state->redraw_required = true;
        }
    }
}


static void get_layout(
    const int window_w, const int window_h, 
    const int pic_w, const int pic_h,
    const int timeline_h,
    SDL_Rect * viewer_rect,
    SDL_Rect * timeline_rect
) {
    SDL_Rect view_bounds = (SDL_Rect) {
        0, 0, window_w, window_h - timeline_h
    };
    *viewer_rect = (SDL_Rect) {
        .w = MIN(view_bounds.w, view_bounds.h * pic_h / pic_w),
        .h = MIN(view_bounds.h, view_bounds.w * pic_w / pic_h),
    };
    viewer_rect->x = (view_bounds.w - viewer_rect->w) / 2 + view_bounds.x;
    viewer_rect->y = (view_bounds.h - viewer_rect->h) / 2 + view_bounds.y;

    *timeline_rect = (SDL_Rect) {
        0, (window_h - timeline_h), window_w, timeline_h
    };
}


int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "provide filename\n");
        return -1;
    }
    char * filename = argv[1];

    /* get codec and format */

    AVFormatContext * format_ctx = NULL;
    if (avformat_open_input(&format_ctx, filename, NULL, NULL))
        return -1;


    int video_stream_idx = 
        av_find_best_stream(
            format_ctx, AVMEDIA_TYPE_VIDEO, -1,
            -1, NULL, 0
        );

    if (video_stream_idx < 0)
        return -1;


    AVCodecContext * codec_ctx =
        open_codec_context(format_ctx, video_stream_idx);

    if (codec_ctx == NULL)
        return -1;


    /* create packet and frame buffers, then decode first frame */

    AVPacket * pkt = av_packet_alloc();
    AVFrame * frame = av_frame_alloc();

    do {
        av_read_frame(format_ctx, pkt);
    } while ( pkt->stream_index != video_stream_idx);

    decode_frame(codec_ctx, pkt, frame);


    /* initialize SDL */

    if (SDL_Init(SDL_INIT_VIDEO)) return 1;

    SDL_Window * window;
    SDL_Renderer * renderer;
    SDL_Texture * video_tex;

    SDL_CreateWindowAndRenderer(
        1000, 1000, 0,
        &window, &renderer
    );
    if ((window == NULL) || (renderer == NULL))
        return -1;

    video_tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        codec_ctx->width, codec_ctx->height
    );
    if (video_tex == NULL)
        return -1;


    /* initialize frame converter */
    
    struct FrameConverter frame_conv = make_frame_converter(
        codec_ctx, AV_PIX_FMT_RGB24, get_texture_pitch(video_tex)
    );

    /* get color scheme */

    struct ColorScheme colors = default_colors();


    /* main loop */

    struct ViewerState viewer_state =
        (struct ViewerState){};
    SDL_Event event;
    viewer_state.window_h = 1000;
    viewer_state.window_w = 1000;
    viewer_state.playing = false;
    viewer_state.frame_changed = true;
    viewer_state.redraw_required = true;
    while (!viewer_state.quit) {
        clock_t start_time = clock();

        /* handle events */
        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            handle_event(&event, &viewer_state);
        }

        /* if frame_changed is true at this point, it means we need to seek
         * to the correct frame and not just go forward one */
        bool seek = false;
        if (viewer_state.frame_changed)
            seek = true;

        if (viewer_state.playing) {
            viewer_state.current_frame++;
            viewer_state.frame_changed = true;
        }

        /* decode frame */
        if (viewer_state.frame_changed) {
            if (seek) {
                int frame_n = viewer_state.current_frame;
                avformat_seek_file(
                    format_ctx,
                    video_stream_idx,
                    frame_n, frame_n, frame_n,
                    AVSEEK_FLAG_FRAME
                );
            }
            do {
                av_read_frame(format_ctx, pkt);
            } while ( pkt->stream_index != video_stream_idx);

            switch (decode_frame(codec_ctx, pkt, frame)) {
                case 0: {
                    uint8_t * pixels = NULL;
                    int _;
                    SDL_LockTexture(video_tex, NULL, (void **) &pixels, &_);

                    convert_frame(&frame_conv, frame, pixels);

                    SDL_UnlockTexture(video_tex);
                    viewer_state.redraw_required = true;
                    break;
                }
                case -2: /* EOF */
                    viewer_state.playing = false;
                    break;
                default: /* Decode Error */
                    return -1;
            }
        }
        /* draw screen */
        if (viewer_state.redraw_required) {
            SDL_Rect viewer_rect, timeline_rect;
            get_layout(
                viewer_state.window_w, viewer_state.window_h,
                frame->height, frame->width, 50,
                &viewer_rect, &timeline_rect
            );
            SDL_SetRenderDrawColor(renderer, CL(colors.letterboxing));
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, video_tex, NULL, &viewer_rect);
            SDL_SetRenderDrawColor(renderer, CL(colors.timeline_bg));
            SDL_RenderFillRect(renderer, &timeline_rect);
            SDL_RenderPresent(renderer);
        }
        viewer_state.redraw_required = false;
        viewer_state.frame_changed = false;
        
        while (REFRESH_TIME - (clock() - start_time) > 0);
    };

    /* clean up */
    destroy_frame_converter(&frame_conv);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avformat_close_input(&format_ctx);
    avcodec_free_context(&codec_ctx);
    SDL_DestroyTexture(video_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

