#pragma once
#include "../av.h"

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
int get_frame(struct PlaybackCtx * pb_ctx, SDL_Texture * tex, int64_t * pts, int64_t * duration);

struct PlaybackCtx * open_for_playback(char * filename);

void destroy_playback_ctx(struct PlaybackCtx * pb_ctx);

