#pragma once
#include "../av.h"

/* buffer for storing frames as SDL_Texture:s 
 *
 * - video thread writes texture data to pixel_buf.    <-----------------+
 * - main thread swaps the textures, points pixel_buf to the data of     |
 *   next_frame and signals to video thread that it can begin decoding   |
     the next frame                                                      |
 * - main thread reads video data from current_frame as... --------------+
 * 
 * mutex only guards next_frame, next_timestamp, and frame_needed. video thread 
 * should lock mutex the whole time it is decoding, and main thread should lock
 * it only while swapping the frames */
struct FrameBuffer {
    SDL_Texture * frame, * next_frame;
    int64_t pts, duration;
};

void destroy_framebuffer(struct FrameBuffer * fb);

struct FrameBuffer create_framebuffer(
    SDL_Renderer * renderer, SDL_PixelFormatEnum pixel_format,
    uint32_t width, uint32_t height
);

struct InternalData;

struct PlaybackCtx {
    AVRational time_base;
    int start_time, duration;
    int width, height;
    struct InternalData * internal_data;
};

void seek(struct PlaybackCtx * pb_ctx, int ts);

void advance_frame(struct PlaybackCtx * pb_ctx);

/* Returns 1 if this frame is new, 0 if the frame is unchanged since the last call.
 * Either way, stores pointer to current frame in tex, frame's presentation timestamp
 * in pts and duration in duration, both in video stream units.
 * tex, pts, and duration can be NULL */
int get_frame(struct PlaybackCtx * pb_ctx, SDL_Texture ** tex, int64_t * pts, int64_t * duration);

struct PlaybackCtx * open_for_playback(char * filename);

void playback_to_renderer(struct PlaybackCtx * pb_ctx, SDL_Renderer * renderer);

void destroy_playback_ctx(struct PlaybackCtx * pb_ctx);

