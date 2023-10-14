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

enum EventType {
    EVENT_NONE,
    EVENT_PAUSE,
    EVENT_SEEK,
    EVENT_RESIZE,
    EVENT_QUIT
};

struct Event {
    uint32_t type;
    union {
        double seconds;
        struct {
            int w, h;
        };
    };
};

struct QueuedEvent {
    struct Event event;
    struct QueuedEvent * next;
};

struct EventQueue {
    struct QueuedEvent * first;
    struct QueuedEvent * last;
    int count;
};

struct EventQueue create_event_queue(void) {
    return (struct EventQueue) {
        NULL, NULL, 0
    };
}

void queue_event(struct EventQueue * eventq, struct Event event) {

    struct QueuedEvent * qd_event = malloc(sizeof(struct QueuedEvent));

    *qd_event = (struct QueuedEvent){
        .event = event,
        .next = NULL
    };

    if (eventq->last) {
        eventq->last->next = qd_event;
        eventq->last = qd_event;
    } else {
        eventq->first = eventq->last = qd_event;
    }

    eventq->count++;
}

struct Event poll_events(struct EventQueue * eventq) {

    struct Event event = { .type = EVENT_NONE };

    if (eventq->count) {
        struct QueuedEvent * qd_event = eventq->first;
        event = qd_event->event;
        if (eventq->last == eventq->first) {
            eventq->last = eventq->first = NULL;
        } else {
            eventq->first = eventq->first->next;
        }
        free(qd_event);
        eventq->count--;
    }

    return event;

}


static uint64_t handle_event(
    SDL_Event * sdl_event,
    struct EventQueue * eventq,
    struct Layout * layout
) {
    int nkeys;
    const uint8_t * keys = SDL_GetKeyboardState(&nkeys);

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    switch (sdl_event->type) {
        case SDL_MOUSEWHEEL:
            if (SDL_PointInRect(&(SDL_Point){ mouse_x, mouse_y }, &layout->timeline_rect)) {

                if (keys[SDL_SCANCODE_LSHIFT])
                    queue_event(
                        eventq, (struct Event){ EVENT_SEEK, .seconds = sdl_event->wheel.y * 2.0 }
                    );
                else
                    queue_event(
                        eventq, (struct Event){ EVENT_SEEK, .seconds = sdl_event->wheel.y * 0.5 }
                    );
            }
            break;

        case SDL_KEYDOWN:
            switch (sdl_event->key.keysym.sym) {
                case SDLK_SPACE:
                    queue_event( eventq, (struct Event){ EVENT_PAUSE });
                    break;
                case SDLK_ESCAPE:
                    queue_event( eventq, (struct Event){ EVENT_QUIT });
                    break;
            }
            break;

        case SDL_QUIT:
            queue_event( eventq, (struct Event){ EVENT_QUIT });

        case SDL_WINDOWEVENT:
            if (sdl_event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                queue_event(eventq, (struct Event) {
                    EVENT_RESIZE, {
                        .w = sdl_event->window.data1,
                        .h = sdl_event->window.data2 
                    }
                });
            }
            break;
    }
    return 0;
}

static TTF_Font * default_font(int size) {
    return TTF_OpenFont("fonts/RobotoMono-Regular.ttf", size);
}

int init_sdl(SDL_Renderer ** renderer, SDL_Window ** window) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "failed to initialize SDL");
        return -1;
    };

    SDL_CreateWindowAndRenderer(
        1000, 1000, 0,
        window, renderer
    );
    if ((window == NULL) || (renderer == NULL)) {
        fprintf(stderr, "failed to open window");
        return -1;
    }
    return 0;
}


int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "provide filename\n\n");
        return -1;
    }
    char * filename = argv[1];

    SDL_Renderer * renderer;
    SDL_Window * window;
    if (init_sdl(&renderer, &window)) return -1;


    TTF_Init();
    TTF_Font * font = default_font(13);

    struct PlaybackCtx * pb_ctx = open_for_playback(filename);

    struct ColorScheme colors = default_colors();



    struct Layout layout;
    {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);

        layout = get_layout(
            w, h,
            pb_ctx->height, pb_ctx->width, TIMELINE_HEIGHT, PROGRESS_HEIGHT
        );
    }

    struct FrameBuffer framebuffer = create_framebuffer(
        renderer, SDL_PIXELFORMAT_RGB24,
        pb_ctx->width, pb_ctx->height
    );

    playback_to_framebuffer(pb_ctx, &framebuffer);

    framebuffer_swap(&framebuffer, true);

    double ts = pb_ctx->start_time;
    double next_frame_ts = ts;
    double min_frame_time = 1.0/144.0;

    AVRational clocks_per_time_base =
        av_mul_q(pb_ctx->time_base, (AVRational){ CLOCKS_PER_SEC, 1 });
    AVRational time_base = pb_ctx->time_base;


    bool paused = true;

    struct EventQueue eventq = create_event_queue();

    while (!quit) {
        struct timespec frame_start, frame_finish;
        double elapsed;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        /* if the current timestamp is greater than the timestamp of the next
         * frame, it's time to display a new frame. if we have a new frame we
         * definitely need to redraw the video */
        bool newframe = (ts >= next_frame_ts);
        bool redraw_ui = !paused;
        bool redraw_video = newframe;
        #define SECS(TIME_BASE) av_q2d(av_mul_q((AVRational) { (TIME_BASE), 1 }, pb_ctx->time_base))
        #define TIME_BASE(SECS) av_q2d(av_div_q( av_d2q(SECS, 0xffff), pb_ctx->time_base))

        SDL_PumpEvents();
        SDL_Event * sdl_event = &(SDL_Event){};
        while (SDL_PollEvent(sdl_event)) {
            handle_event(sdl_event, &eventq, &layout);
        }

        struct Event event = poll_events(&eventq);
        switch (event.type) {
            case EVENT_NONE: break;
            case EVENT_QUIT:
                quit = true;
                break;

            case EVENT_PAUSE: 
                paused = !paused; 
                break;

            case EVENT_RESIZE:
                layout = get_layout(
                    event.w, event.h,
                    pb_ctx->height, pb_ctx->width,
                    TIMELINE_HEIGHT, PROGRESS_HEIGHT
                );
                redraw_ui = true;
                break;

            case EVENT_SEEK:
                ts = ts + TIME_BASE(event.seconds);
                ts = next_frame_ts =
                    MIN(MAX(ts, pb_ctx->start_time), pb_ctx->duration);
                seek(pb_ctx, ts);
                framebuffer_swap(&framebuffer, true);
                framebuffer_swap(&framebuffer, true);
                redraw_video = true;
                break;
        }

        if (redraw_video) {
            draw_background(renderer, &colors);
            SDL_RenderCopy(renderer, framebuffer.frame, NULL, &layout.viewer_rect);
            redraw_ui = true;
        }

        if (newframe) {
            if (!framebuffer_swap(&framebuffer, false)) {
                next_frame_ts += framebuffer.duration;
            }
        }

        if (redraw_ui) {
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
        }

        do {
            clock_gettime(CLOCK_MONOTONIC, &frame_finish);
            elapsed = frame_finish.tv_sec - frame_start.tv_sec + (frame_finish.tv_nsec - frame_start.tv_nsec) / 1000000000.0;
        } while (elapsed < min_frame_time);

        if (!paused)
            ts += elapsed/av_q2d(pb_ctx->time_base);
    }

    destroy_framebuffer(&framebuffer);
    destroy_playback_ctx(pb_ctx);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}