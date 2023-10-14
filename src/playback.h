#include "av.h"
#include "playback_threads.h"

struct InternalData;

struct PlaybackCtx {
    AVRational time_base;
    int start_time, duration;
    int width, height;
    struct InternalData * internal_data;
};

struct PlaybackCtx * open_for_playback(char * filename);
void playback_to_framebuffer(struct PlaybackCtx * pb_ctx, struct FrameBuffer * fb);
