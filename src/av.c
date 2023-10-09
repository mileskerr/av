#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
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
#include <math.h>

#define MIN(A, B) (A < B) ? (A) : (B)
#define MAX(A, B) (A > B) ? (A) : (B)

#define CL(COLOR) COLOR.r, COLOR.g, COLOR.b, COLOR.a

#define TO_CLOCKS(SECONDS) ((clock_t) (((float) SECONDS) * ((float) CLOCKS_PER_SEC)))
const clock_t REFRESH_TIME = TO_CLOCKS(0.02);



struct ColorScheme {
    SDL_Color bg[5]; /* backgrounds, lowest to highest contrast */
    SDL_Color fg[5]; /* foregrounds, lowest to highest contrast */
    SDL_Color highl_bg; /* bright color for selected items */
    SDL_Color highl_fg;
    SDL_Color acc_bg; /* bright color complimentary to highlight */
    SDL_Color acc_fg;
};

static SDL_Color cl_mul(SDL_Color * cl, double m) {
    return (SDL_Color) { 
        (uint8_t) (((double) cl->r) * m),
        (uint8_t) (((double) cl->g) * m),
        (uint8_t) (((double) cl->b) * m),
        0xff
    };
    
}

static struct ColorScheme default_colors(void) {
    SDL_Color base_fg = { 0xcd, 0xd6, 0xf4, 0xff };
    SDL_Color base_bg = { 0x31, 0x32, 0x44, 0xff };
    return (struct ColorScheme) {
        .bg = {
            cl_mul(&base_bg, 1.4), cl_mul(&base_bg, 1.2), base_bg,
            cl_mul(&base_bg, 0.7), cl_mul(&base_bg, 0.4)
        },
        .fg = {
            cl_mul(&base_fg, 0.6), cl_mul(&base_fg, 0.8), base_fg,
            cl_mul(&base_fg, 1.1), cl_mul(&base_fg, 1.2)
        },
        .highl_bg = { 0xb4, 0xbe, 0xfe, 0xff },
        .highl_fg = { 0x18, 0x18, 0x25, 0xff },
        .acc_bg = { 0xf3, 0x8b, 0xa8, 0xff },
        .acc_fg = { 0x18, 0x18, 0x25, 0xff }
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


struct Layout {
    SDL_Rect viewer_rect;
    SDL_Rect timeline_rect;
    SDL_Rect progress_rect;
};

/* +--+----------+--+
 * |  |          |  | <-view_bounds
 * |  |        <-|--|-viewer_rect
 * |  |          |  |
 * +--+----------+--+ <-progress_rect
 * +----------------+ 
 * |                | <-timeline_rect
 * +----------------+ 
 *
 * computes bounds of each rectangle comprising the layout shown above
 * based on original video dimensions (pic_w, pic_h), window dimensions
 * (window_w, window_h), and height of progress bar and timeline */
static struct Layout get_layout(
    const int window_w, const int window_h, 
    const int pic_w, const int pic_h,
    const int timeline_h,
    const int progress_h
) {
    struct Layout layout;
    SDL_Rect view_bounds = (SDL_Rect) {
        0, 0, window_w, window_h - timeline_h - progress_h
    };
    SDL_Rect viewer_rect = (SDL_Rect) {
        .w = MIN(view_bounds.w, view_bounds.h * pic_h / pic_w),
        .h = MIN(view_bounds.h, view_bounds.w * pic_w / pic_h),
    };
    viewer_rect.x = (view_bounds.w - viewer_rect.w) / 2 + view_bounds.x;
    viewer_rect.y = (view_bounds.h - viewer_rect.h) / 2 + view_bounds.y;

    layout.viewer_rect = viewer_rect;

    layout.timeline_rect = (SDL_Rect) {
        0, (window_h - timeline_h), window_w, timeline_h
    };
    layout.progress_rect = (SDL_Rect) {
        0, (window_h - timeline_h - progress_h), window_w, progress_h
    };
    return layout;
}

enum TextAlignment {
    ALIGN_LEFT,
    ALIGN_CENTER,
    ALIGN_RIGHT
};

void draw_text(
    SDL_Renderer * renderer, const char * message, 
    TTF_Font * font, const SDL_Color color, 
    enum TextAlignment alignment, int x, int y
) {

	SDL_Surface * surf = TTF_RenderText_Blended(font, message, color);
	SDL_Texture * texture = SDL_CreateTextureFromSurface(renderer, surf);

	SDL_FreeSurface(surf);
   
    int w, h;
    SDL_QueryTexture(texture, NULL, NULL, &w, &h);
    switch (alignment) {
        case ALIGN_RIGHT:
            x -= w;
            break;
        case ALIGN_CENTER:
            x -= w/2;            
            break;
        case ALIGN_LEFT:;
    };
    SDL_Rect dstrect = {
        .x = x,
        .y = y,
        .w = w,
        .h = h
    };
    SDL_RenderCopy(renderer, texture, NULL, &dstrect);

}

struct ViewerState {
    int window_w;
    int window_h;
    double cur_timestamp;
    bool playing;
};

struct VideoInfo {
    int w;
    int h;
    double duration;
    double start_time;
    double avg_fps;
};

enum Action {
    SEEK_FWD = 1 << 1,
    SEEK_BACK = 1 << 2,
    WINDOW_RESIZE = 1 << 3,
    PLAY_PAUSE = 1 << 4,
    QUIT = 1 << 5,
};

static enum Action handle_event(
    SDL_Event * event,
    struct VideoInfo * vid,
    struct ViewerState * state,
    const struct Layout * layout
) {
    int nkeys;
    const uint8_t * keys = SDL_GetKeyboardState(&nkeys);

    int mouse_x, mouse_y;
    uint32_t mouse = SDL_GetMouseState(&mouse_x, &mouse_y);

    switch (event->type) {
        case SDL_MOUSEWHEEL:
            if (SDL_PointInRect(&(SDL_Point){ mouse_x, mouse_y }, &layout->timeline_rect)) {
                double seek_by;
                double new_ts;
                if (keys[SDL_SCANCODE_LSHIFT])
                    seek_by = event->wheel.y * 1.0;
                else if (keys[SDL_SCANCODE_LCTRL])
                    seek_by = event->wheel.y * 1.0/vid->avg_fps;
                else
                    seek_by = event->wheel.y * 10.0/vid->avg_fps;

                new_ts = state->cur_timestamp + seek_by;

                if (new_ts < vid->start_time) {
                    state->cur_timestamp = vid->start_time;
                    return SEEK_BACK;
                } else if (new_ts > vid->duration) {
                    state->cur_timestamp = vid->duration;
                    return SEEK_FWD;
                } else {
                    state->cur_timestamp = new_ts;
                    if (seek_by > 0)
                        return SEEK_FWD;
                    else
                        return SEEK_BACK;
                }
            }
            break;

        case SDL_KEYDOWN:
            switch (event->key.keysym.sym) {
                case SDLK_ESCAPE:
                    return QUIT;
                case SDLK_SPACE:
                    state->playing = !state->playing;
                    return PLAY_PAUSE;
            }
            break;

        case SDL_QUIT:
            return QUIT;

        case SDL_WINDOWEVENT:
            if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                state->window_w = event->window.data1;
                state->window_h = event->window.data2;
                return WINDOW_RESIZE;
            }
            break;
    }
    return 0;
}

/* draw progress bar. */
static void draw_progress(
    SDL_Renderer * renderer, const SDL_Rect rect,
    double timestamp, double duration,
    const struct ColorScheme * colors
) {
    int label_w = 0;
    int bar_h = 4;
    int line_w = 2;

    /* fill background */
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[2]));
    SDL_RenderFillRect(renderer, &rect);

    /* draw bar bounds */
    SDL_Rect bar_bounds = (SDL_Rect) {
        .w = rect.w - (label_w * 2.0),
        .h = bar_h
    };

    bar_bounds.x = (rect.w - bar_bounds.w) / 2.0 + rect.x;
    bar_bounds.y = rect.y + rect.h - bar_h; //(rect.h - bar_bounds.h) / 2.0 + rect.y;

    SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
    SDL_RenderFillRect(renderer, &bar_bounds);

    /* draw bar */
    int position = timestamp * bar_bounds.w / duration;
    SDL_Rect bar = (SDL_Rect) {
        .x = bar_bounds.x, .y = bar_bounds.y,
        .w = position, .h = bar_bounds.h
    };
    SDL_SetRenderDrawColor(renderer, CL(colors->highl_bg));
    SDL_RenderFillRect(renderer, &bar);

    /* draw line */
    SDL_SetRenderDrawColor(renderer, CL(colors->highl_bg));
    SDL_RenderFillRect(
        renderer,
        &(SDL_Rect) { position + bar_bounds.x - line_w/2, rect.y, line_w, rect.h }
    );

}

/* converts a duration in seconds to string in format: hh:mm:ss.ss
 * writes maximum of 12 bytes including null terminator. */
static void dur_to_str(double duration, char * dst) {
    duration = MAX(duration, 0.0);
    int hours = ((int)duration / 3600) % 24;
    int minutes = ((int)duration / 60) % 60;
    int seconds = ((int) duration) % 60;
    int hundredths = (int)(duration * 100) % 100;

    char src[12] = {
        (hours / 10) % 10 + '0',
        hours % 10 + '0',
        ':',
        (minutes / 10) % 10 + '0',
        minutes % 10 + '0',
        ':',
        (seconds / 10) % 10 + '0',
        seconds % 10 + '0',
        '.',
        (hundredths / 10) % 10 + '0',
        (hundredths % 10) + '0',
        '\0'
    };

    int i;
    for (i = 0; 
        (i < 7) && ((src[i] == '0') || (src[i] == ':'));
        i++);

    memcpy(dst, src+i, 12-i);
}


static void draw_timeline(
    SDL_Renderer * renderer, TTF_Font * font, SDL_Rect rect,
    double start_time, double timestamp, double duration,
    const struct ColorScheme * colors
) {
    int label_h = 20;
    int label_w = 100;
    int pixels_per_sec = 120;
    int mnr_tms_per_mjr = 4;
    int mjr_tm_w = 2;
    int mnr_tm_w = 2;
    double halfwidth = ((double) rect.w)/2.0;
    char timestamp_str[12];


    /* draw tickmark area */
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[1]));
    SDL_RenderFillRect(
        renderer,
        &(SDL_Rect) { rect.x, rect.y + label_h, rect.w, rect.h - label_h }
    );

    /* draw darker area left of the video start point & right of video end*/
    double tlend= timestamp - halfwidth/pixels_per_sec;
    double trend = timestamp + halfwidth/pixels_per_sec;
    if (tlend < start_time) {
        SDL_Rect r = { 
            rect.x, rect.y,
            (start_time - timestamp) * pixels_per_sec + halfwidth,
            rect.h, 
        };
        SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
        SDL_RenderFillRect(renderer, &r);
    }
    if (trend > duration) {
        int w = 
            halfwidth - (duration - timestamp) * pixels_per_sec;
        SDL_Rect r = { 
            rect.x + rect.w - w, rect.y,
            w, rect.h,
        };
        SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
        SDL_RenderFillRect(renderer, &r);
    }

    /* draw label area */
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
    SDL_RenderFillRect(
        renderer, 
        &(SDL_Rect) { rect.x, rect.y, rect.w, label_h }
    );

    /* draw tickmarks */
    int nmaj_tms = (int) ceil(((double) rect.w) / pixels_per_sec);

    double ts_intg, ts_frac;
    ts_frac = modf(timestamp, &ts_intg);

    for (int i = -nmaj_tms/2; i < nmaj_tms/2 + 2; i++) {

        int time = ts_intg + i;
        if (time < 0)
            continue;
        if (time > duration)
            continue;

        int x = (time - timestamp) * pixels_per_sec + halfwidth;

        SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
        SDL_RenderFillRect(
            renderer, 
            &(SDL_Rect) {
                x + rect.x - ((float)mjr_tm_w/2), rect.y + label_h,
                mjr_tm_w, rect.h - label_h
            }
        );
        dur_to_str(time, timestamp_str);
        draw_text(
            renderer, timestamp_str, font,
            colors->fg[1], ALIGN_CENTER, x,
            rect.y
        );
        double mnr_spacing = ((double)pixels_per_sec)/mnr_tms_per_mjr;
        for (int j = 1; j < mnr_tms_per_mjr; j++) {
            double mnr_x = x + j * mnr_spacing;
            SDL_SetRenderDrawColor(renderer, CL(colors->bg[2]));
            SDL_RenderFillRect(
                renderer, 
                &(SDL_Rect) {
                    mnr_x + rect.x - ((float)mnr_tm_w/2), rect.y + label_h,
                    mnr_tm_w, rect.h - label_h
                }
            );
        }
    }
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
    SDL_RenderDrawLine(renderer, rect.x, rect.y + label_h, rect.w + rect.x, rect.y + label_h);

    /* draw current frame */
    SDL_SetRenderDrawColor(renderer, CL(colors->highl_bg));
    SDL_RenderFillRect(
        renderer, 
        &(SDL_Rect){
            halfwidth - ((float)mjr_tm_w/2),
            rect.y, mjr_tm_w, rect.h
        }
    );

    SDL_RenderFillRect(
        renderer, 
        &(SDL_Rect){
            halfwidth - label_w,
            rect.y, label_w, label_h - 2
        }
    );
    SDL_SetRenderDrawColor(renderer, CL(colors->highl_fg));

    dur_to_str(timestamp, timestamp_str);
    draw_text(
        renderer, timestamp_str, font,
        colors->highl_fg, ALIGN_RIGHT, halfwidth,
        rect.y
    );
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[4]));
    SDL_RenderDrawLine(renderer, rect.x, rect.y, rect.w + rect.x, rect.y);
}


static double avr_to_dbl(AVRational rational) {
    return ((double) rational.num) / ((double) rational.den);
}

static TTF_Font * default_font(int size) {
    return TTF_OpenFont("fonts/RobotoMono-Regular.ttf", size);
}

/* get default viewer state. */
static struct ViewerState default_state(void) {
    return (struct ViewerState) {
        .window_h = 1000,
        .window_w = 1000,
        .playing = false,
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

    AVStream * stream = format_ctx->streams[video_stream_idx];

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

    /* initialize TTF */

    TTF_Init();
    TTF_Font * font = default_font(13);



    /* misc initializations */
    
    struct FrameConverter frame_conv = make_frame_converter(
        codec_ctx, AV_PIX_FMT_RGB24, get_texture_pitch(video_tex)
    );    

    struct ColorScheme colors = default_colors();
    struct ViewerState viewer_state = default_state();
    SDL_Event event;
    double time_base = avr_to_dbl(stream->time_base);
    clock_t next_frame_time = 0;

    #define TIMELINE_HEIGHT 38
    #define PROGRESS_HEIGHT 20

    struct Layout layout = get_layout(
        viewer_state.window_w, viewer_state.window_h,
        frame->height, frame->width, TIMELINE_HEIGHT, PROGRESS_HEIGHT
    );

    struct VideoInfo vid_info = {
        .w = frame->width, .h = frame->height,
        .duration = ((double) stream->duration) * time_base,
        .start_time = ((double) stream->start_time) * time_base,
        .avg_fps = avr_to_dbl(stream->avg_frame_rate)
    };

    /* main loop */
    goto render;
    loop: {
        clock_t start_time = clock();
        enum Action action = 0;
        bool redraw, newframe;
        redraw = newframe = false;

        /* handle events */
        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            action |= handle_event(
                &event, &vid_info, &viewer_state, &layout
            );
        }

        SDL_Point mouse_pt;
        uint32_t mouse = SDL_GetMouseState(&mouse_pt.x, &mouse_pt.y);
        if (
            (mouse & SDL_BUTTON(1)) &&
            SDL_PointInRect(&mouse_pt, &layout.progress_rect)
        ) {
            double position = 
                (double)(mouse_pt.x - layout.progress_rect.x)/
                (double)(layout.progress_rect.w);
            double ts = position * vid_info.duration;

            if (ts > viewer_state.cur_timestamp)
                action |= SEEK_FWD;
            else
                action |= SEEK_BACK;

            viewer_state.cur_timestamp = ts;
        }

        if (action & QUIT)
            goto quit;

        /* decode frame */
        if (action & (SEEK_FWD | SEEK_BACK)) {
            newframe = true;
            AVRational fps = stream->avg_frame_rate;
            uint64_t ts = 
                viewer_state.cur_timestamp / time_base;
            uint64_t max_ts = 
                ts + avr_to_dbl((AVRational){ fps.den, fps.num }) / time_base;
            int flags;
            if (action & SEEK_BACK)
                flags = AVSEEK_FLAG_BACKWARD;
            else
                flags = 0;
            avformat_seek_file(
                format_ctx,
                video_stream_idx,
                -0xffffff, ts, max_ts,
                flags
            );
        }

        if (action & PLAY_PAUSE)
            next_frame_time = clock();

        int t;
        if (
            viewer_state.playing && 
            ((t = clock()) > next_frame_time)
        ) newframe = true;

        if  (newframe) {
            /* seeking can only take us to keyframes, so we may
             * need to read a few frames before we get to the desired
             * timestamp. if the video is playing normally loop
             * should not repeat */
            int decode_res;
            do {
                do {
                    av_frame_unref(frame);
                    av_packet_unref(pkt);
                    if (av_read_frame(format_ctx, pkt))
                        /* in case the duration estimate is wrong and we reach EOF without
                         * passing cur_timestamp, don't keep reading packets forever */
                        goto demux_end; 
                } while ( pkt->stream_index != video_stream_idx);
                decode_res = decode_frame(codec_ctx, pkt, frame);
            } while(pkt->dts < viewer_state.cur_timestamp/time_base - 1.0/vid_info.avg_fps);
            demux_end:

            switch (decode_res) {
                case 0: {
                    uint8_t * pixels = NULL;
                    int _;
                    SDL_LockTexture(video_tex, NULL, (void **) &pixels, &_);

                    convert_frame(&frame_conv, frame, pixels);

                    SDL_UnlockTexture(video_tex);
                    redraw = true;
                    next_frame_time += ((double)pkt->duration * time_base) * CLOCKS_PER_SEC;
                    int delay = clock() - next_frame_time;
                    if (delay > 0)
                        fprintf(stderr, "behind by %lf\n", (double)delay/CLOCKS_PER_SEC);
                    viewer_state.cur_timestamp = (double)pkt->dts * time_base;
                    break;
                }
                case -2: /* EOF */
                    viewer_state.cur_timestamp = vid_info.duration;
                    viewer_state.playing = false;
                    break;
                default: /* Decode Error */
                    return -1;
            }
        }

        if (action & WINDOW_RESIZE)
            redraw = true;

        /* draw screen */
        if (redraw) {
            render:
            layout = get_layout(
                viewer_state.window_w, viewer_state.window_h,
                frame->height, frame->width, TIMELINE_HEIGHT, PROGRESS_HEIGHT
            );
            SDL_SetRenderDrawColor(renderer, CL(colors.bg[4]));
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, video_tex, NULL, &layout.viewer_rect);
            draw_progress(
                renderer,
                layout.progress_rect, 
                viewer_state.cur_timestamp,
                vid_info.duration,
                &colors
            );
            draw_timeline(
                renderer,
                font,
                layout.timeline_rect,
                vid_info.start_time,
                viewer_state.cur_timestamp,
                vid_info.duration,
                &colors
            );
            SDL_RenderPresent(renderer);
        }
    
        while (REFRESH_TIME - (clock() - start_time) > 0);
        goto loop;
    }
    quit:

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

