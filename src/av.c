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
    EVENT_SEEK_REL,
    EVENT_SEEK,
    EVENT_RESIZE,
    EVENT_QUIT
};

struct Event {
    uint32_t type;
    union {
        double seconds;
        double position;
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



static void handle_sdl_event(
    SDL_Event * sdl_event,
    struct EventQueue * eventq,
    struct Layout * layout,
    const uint8_t * keys,
    int mouse_x, int mouse_y
) {


    switch (sdl_event->type) {
        case SDL_MOUSEWHEEL:
            if (SDL_PointInRect(&(SDL_Point){ mouse_x, mouse_y }, &layout->timeline_rect)) {

                if (keys[SDL_SCANCODE_LSHIFT])
                    queue_event(
                        eventq, (struct Event){ EVENT_SEEK_REL, .seconds = sdl_event->wheel.y * 2.0 }
                    );
                else
                    queue_event(
                        eventq, (struct Event){ EVENT_SEEK_REL, .seconds = sdl_event->wheel.y * 0.5 }
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
            break;

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
}

static void handle_input(
    struct EventQueue * eventq,
    struct Layout * layout
) {
    static bool dragging_progress_bar = false;

    SDL_PumpEvents();


    int mouse_x, mouse_y;
    uint32_t mouse = SDL_GetMouseState(&mouse_x, &mouse_y);

    int nkeys;
    const uint8_t * keys = SDL_GetKeyboardState(&nkeys);

    SDL_Event * sdl_event = &(SDL_Event){};
    while (SDL_PollEvent(sdl_event)) {
        handle_sdl_event(sdl_event, eventq, layout, keys, mouse_x, mouse_y);
    }
    
    if (
        (mouse & SDL_BUTTON(1)) &&
        (SDL_PointInRect(&(SDL_Point){ mouse_x, mouse_y }, &layout->progress_rect))
    )
        dragging_progress_bar = true;

    if (!(mouse & SDL_BUTTON(1))) dragging_progress_bar = false;

    if (dragging_progress_bar) {
        double mouse_rel = mouse_x - layout->progress_rect.x;
        double position = mouse_rel / layout->progress_rect.w;
        queue_event(eventq, (struct Event){ EVENT_SEEK, .position = position });
    }
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


    playback_to_renderer(pb_ctx, renderer);

    double ts = pb_ctx->start_time;
    double next_frame_ts = ts;
    double min_frame_time = 1.0/144.0;

    bool paused = true;

    struct EventQueue eventq = create_event_queue();

    while (!quit) {
        struct timespec frame_start, frame_finish;
        double elapsed;
        SDL_Texture * video_tex;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        #define SECS(TIME_BASE) av_q2d(av_mul_q((AVRational) { (TIME_BASE), 1 }, pb_ctx->time_base))
        #define TIME_BASE(SECS) av_q2d(av_div_q( av_d2q(SECS, 0xffff), pb_ctx->time_base))

        handle_input(&eventq, &layout);

        printf("jjddf: %d\n", paused);

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
                break;

            case EVENT_SEEK:
                ts = event.position * pb_ctx->duration;
                goto seek_to_ts;

            case EVENT_SEEK_REL:
                ts = ts + TIME_BASE(event.seconds);

                seek_to_ts:
                ts = next_frame_ts =
                    MIN(MAX(ts, pb_ctx->start_time), pb_ctx->duration);
                seek(pb_ctx, ts);
                break;
        }

        if (!paused && (ts >= next_frame_ts)) {
            advance_frame(pb_ctx);
        }

        int64_t pts, dur;
        video_tex = get_frame(pb_ctx, &pts, &dur);

        draw_background(renderer, &colors);
        SDL_RenderCopy(renderer, video_tex, NULL, &layout.viewer_rect);


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

        if (!paused)
            ts += elapsed/av_q2d(pb_ctx->time_base);
    }

    destroy_playback_ctx(pb_ctx);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}