#include "av.h"
#include "draw.h"
#include "playback/playback.h"
#include <SDL2/SDL_assert.h>
#include <SDL2/SDL_atomic.h>
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_mutex.h>
#include <SDL2/SDL_thread.h>
#include <libavcodec/avcodec.h>

bool quit = false;


#define SDL_AUDIO_FMT AUDIO_S16SYS
#define SDL_AUDIO_SAMPLES 1024

#define TIMELINE_HEIGHT 38
#define PROGRESS_HEIGHT 20

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

static struct PlayerState default_state(void) {
    return (struct PlayerState) {
        .window_h = 1000,
        .window_w = 1000,
        .playing = false,
    };
}

int init_sdl(SDL_Renderer * renderer, SDL_Window * window) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "failed to initialize SDL");
        return -1;
    };

    SDL_CreateWindowAndRenderer(
        1000, 1000, 0,
        &window, &renderer
    );
    if ((window == NULL) || (renderer == NULL)) {
        fprintf(stderr, "failed to open window");
        return -1;
    }
    return 0;
}


int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "provide filename\n");
        return -1;
    }
    char * filename = argv[1];

    SDL_Renderer * renderer;
    SDL_Window * window;
    init_sdl(renderer, window);


    TTF_Init();
    TTF_Font * font = default_font(13);

    struct PlaybackCtx * pb_ctx = open_for_playback(filename);

    struct ColorScheme colors = default_colors();
    struct PlayerState player_state = default_state();


    struct Layout layout = get_layout(
        player_state.window_w, player_state.window_h,
        pb_ctx->height, pb_ctx->width, TIMELINE_HEIGHT, PROGRESS_HEIGHT
    );

    struct FrameBuffer framebuffer = create_framebuffer(
        renderer, SDL_PIXELFORMAT_RGB24,
        pb_ctx->width, pb_ctx->height
    );

    playback_to_framebuffer(pb_ctx, &framebuffer);

    framebuffer_swap(&framebuffer);

    double ts = pb_ctx->start_time;
    double next_frame_ts = ts;
    double min_frame_time = 1.0/144.0;

    AVRational clocks_per_time_base =
        av_mul_q(pb_ctx->time_base, (AVRational){ CLOCKS_PER_SEC, 1 });
    AVRational time_base = pb_ctx->time_base;


    while (!quit) {
        SDL_Event event;
        struct timespec frame_start, frame_finish;
        double elapsed;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        enum Action action;

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

        #define SECS(TIME) av_q2d(av_mul_q((AVRational) { TIME, 1 }, pb_ctx->time_base))

        draw_progress(
            renderer, layout.progress_rect, 
            SECS(ts), SECS(pb_ctx->duration),
            &colors
        );
        draw_timeline(
            renderer, font, layout.timeline_rect,
            SECS(pb_ctx->start_time), SECS(ts),
            SECS(pb_ctx->duration), &colors
        );
        SDL_RenderPresent(renderer);

        do {
            clock_gettime(CLOCK_MONOTONIC, &frame_finish);
            elapsed = frame_finish.tv_sec - frame_start.tv_sec + (frame_finish.tv_nsec - frame_start.tv_nsec) / 1000000000.0;
        } while (elapsed < min_frame_time);

        ts += elapsed/av_q2d(pb_ctx->time_base);
    }

    destroy_framebuffer(&framebuffer);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}