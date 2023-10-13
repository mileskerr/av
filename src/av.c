#include "av.h"
#include "movie.h"
#include "drw.h"
#include <SDL2/SDL_assert.h>
#include <SDL2/SDL_atomic.h>
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_mutex.h>
#include <SDL2/SDL_thread.h>
#include <libavcodec/avcodec.h>

bool quit = false;

#define ASSERT(condition) {\
if ((condition)) { \
    fprintf(stderr, "assertion (%s) failed at %s:%d\n", #condition, __FILE__, __LINE__); \
    exit(1); \
}}

#define TO_CLOCKS(SECONDS) ((clock_t) (((float) SECONDS) * ((float) CLOCKS_PER_SEC)))
const clock_t REFRESH_TIME = TO_CLOCKS(0.02);

#define SDL_AUDIO_FMT AUDIO_S16SYS
#define SDL_AUDIO_SAMPLES 1024


static AVCodecContext * open_codec_context(AVFormatContext * format_ctx, int stream_idx) {
    const AVCodecParameters * codecpar = format_ctx->streams[stream_idx]->codecpar;
    const AVCodec * codec = avcodec_find_decoder(codecpar->codec_id);
    if (codec == NULL) return NULL;
    AVCodecContext * codec_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codec_ctx, codecpar))
        return NULL;
    if (avcodec_open2(codec_ctx, codec, NULL))
        return NULL;
    return codec_ctx;
}

struct PlayerState {
    int window_w, window_h;
    double cur_timestamp;
    bool playing;
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
    struct PlayerState * state,
    const struct Layout * layout
) {
    int nkeys;
    const uint8_t * keys = SDL_GetKeyboardState(&nkeys);

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    switch (event->type) {
        /*case SDL_MOUSEWHEEL: TODO: reimpliment
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

        */
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

static TTF_Font * default_font(int size) {
    return TTF_OpenFont("fonts/RobotoMono-Regular.ttf", size);
}

/* get default viewer state. */
static struct PlayerState default_state(void) {
    return (struct PlayerState) {
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

    /* get codecs and format */
    
    AVFormatContext * format_ctx = NULL;
    if (avformat_open_input(&format_ctx, filename, NULL, NULL)) {
        fprintf(stderr, "failed to open `%s`", filename);
        return -1;
    }

    avformat_find_stream_info(format_ctx, NULL);

    int vstream_idx = 
        av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1,-1, NULL, 0);

    int astream_idx =
        av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1,-1, NULL, 0);

    if (vstream_idx < 0) {
        fprintf(stderr, "failed to find video stream");
        return -1;
    }

    /* if there's no audio stream, no problem. */
    AVStream * astream;
    AVCodecContext * acodec_ctx;
    if (astream_idx < 0) {
        astream = NULL;
        acodec_ctx = NULL;
    } else {
        astream = format_ctx->streams[astream_idx];
        acodec_ctx = open_codec_context(format_ctx, astream_idx);
        if (acodec_ctx == NULL) {
            fprintf(stderr, "unsupported audio codec");
            astream = NULL;
            astream_idx = -1;
        }
    }

    AVStream * vstream = format_ctx->streams[vstream_idx];

    AVCodecContext * vcodec_ctx =
        open_codec_context(format_ctx, vstream_idx);


    if (vcodec_ctx == NULL) {
        fprintf(stderr, "unsupported video codec");
        return -1;
    }

    /* initialize SDL */

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 1;

    SDL_Window * window;
    SDL_Renderer * renderer;

    SDL_CreateWindowAndRenderer(
        1000, 1000, 0,
        &window, &renderer
    );
    if ((window == NULL) || (renderer == NULL)) {
        fprintf(stderr, "failed to open window");
        return -1;
    }

    int num_adev = SDL_GetNumAudioDevices(0);
    printf("%d\n",num_adev);
    for (int i = 0; i < num_adev; i++) {
        printf("%d: %s\n", i, SDL_GetAudioDeviceName(i, 0));
    }


    TTF_Init();
    TTF_Font * font = default_font(13);




    /* misc initializations */

    struct ColorScheme colors = default_colors();
    struct PlayerState player_state = default_state();
    SDL_Event event;

    #define TIMELINE_HEIGHT 38
    #define PROGRESS_HEIGHT 20

    struct Layout layout = get_layout(
        player_state.window_w, player_state.window_h,
        vcodec_ctx->height, vcodec_ctx->width, TIMELINE_HEIGHT, PROGRESS_HEIGHT
    );

    struct FrameBuffer framebuffer = create_framebuffer(
        renderer, SDL_PIXELFORMAT_RGB24,
        vcodec_ctx->width, vcodec_ctx->height
    );

    struct PacketQueue demuxed_apktq = create_packet_queue();
    struct PacketQueue demuxed_vpktq = create_packet_queue();
    struct PacketQueue decoded_pktq = create_packet_queue();

    pktq_fill(&decoded_pktq);


    SDL_Thread * demuxer = SDL_CreateThread(
        demuxing_thread, "Demuxer", 
        (void *) &(struct DemuxInfo) {
            format_ctx,
            &demuxed_apktq,
            &demuxed_vpktq,
            &decoded_pktq,
            vstream_idx,
            astream_idx
        }
    );
    SDL_Thread * vdecoder = SDL_CreateThread(
        video_thread, "Video Decoder", 
        (void *) &(struct VideoInfo) {
            vcodec_ctx,
            &demuxed_apktq,
            &decoded_pktq,
            &framebuffer,
        }
    );
    /*
    SDL_Thread * adecoder = SDL_CreateThread(
        video_thread, "Audio Decoder", 
        (void *) &(struct AudioInfo) {
            vcodec_ctx,
            &demuxed_apktq,
            &demuxed_vpktq
        }
    );*/



    /* aquire first frame */
    framebuffer_swap(&framebuffer);

    double ts = vstream->start_time;
    double next_frame_ts = ts;
    double min_frame_time = 1.0/144.0;


    AVRational clocks_per_time_base =
        av_mul_q(vstream->time_base, (AVRational){ CLOCKS_PER_SEC, 1 });
    AVRational time_base = vstream->time_base;
    printf("time_base: %d/%d\n", time_base.num, time_base.den);
    printf("clocks_per_sec: %ld\n", CLOCKS_PER_SEC);
    printf("clocks_per_tb: %d/%d\n", clocks_per_time_base.num, clocks_per_time_base.den);


    while (!quit) {
        struct timespec start, finish;
        double elapsed;
        clock_gettime(CLOCK_MONOTONIC, &start);
        enum Action action;

        /* handle events */
        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            action |= handle_event(
                &event, &player_state, &layout
            );
        }

        if (action & QUIT) quit = true;

        if (ts >= next_frame_ts) {
            draw_background(renderer, &colors);
            SDL_RenderCopy(renderer, framebuffer.frame, NULL, &layout.viewer_rect);
            next_frame_ts += framebuffer.duration;
            framebuffer_swap(&framebuffer);
        }

        #define SECS(TIME) av_q2d(av_mul_q((AVRational) { TIME, 1 }, vstream->time_base))

        draw_progress(
            renderer, layout.progress_rect, 
            SECS(ts),
            SECS(vstream->duration),
            &colors
        );
        draw_timeline(
            renderer, font, layout.timeline_rect,
            SECS(vstream->start_time),
            SECS(ts),
            SECS(vstream->duration),
            &colors
        );
        SDL_RenderPresent(renderer);

        do {
            clock_gettime(CLOCK_MONOTONIC, &finish);
            elapsed = finish.tv_sec - start.tv_sec + (finish.tv_nsec - start.tv_nsec) / 1000000000.0;
        } while (elapsed < min_frame_time);
        ts += elapsed/av_q2d(vstream->time_base);
    }

    /* clean up */
    SDL_WaitThread(vdecoder, NULL);
    SDL_WaitThread(demuxer, NULL);
    avformat_close_input(&format_ctx);
    avcodec_free_context(&vcodec_ctx);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}