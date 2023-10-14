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
    int duration, next_duration;
    SDL_Texture * frame, * next_frame;
    uint8_t * pixel_buf;
    bool frame_needed;
    SDL_mutex * mutex;
};

void destroy_framebuffer(struct FrameBuffer * fb);

struct FrameBuffer create_framebuffer(
    SDL_Renderer * renderer, SDL_PixelFormatEnum pixel_format,
    uint32_t width, uint32_t height
);

/* swaps the buffers and signals that a new frame should be aquired.
 * fb->current_frame is ok to use until this function is called again on fb. */
int framebuffer_swap(struct FrameBuffer * fb, bool wait);

struct InternalData;

struct PlaybackCtx {
    AVRational time_base;
    int start_time, duration;
    int width, height;
    struct InternalData * internal_data;
};

//void play_pause(struct PlaybackCtx * pb_ctx);

void seek(struct PlaybackCtx * pb_ctx, int ts);

struct PlaybackCtx * open_for_playback(char * filename);

void playback_to_framebuffer(struct PlaybackCtx * pb_ctx, struct FrameBuffer * fb);

void destroy_playback_ctx(struct PlaybackCtx * pb_ctx);

