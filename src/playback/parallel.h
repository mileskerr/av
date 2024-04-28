#pragma once
#include "../av.h"
#include "ipc.h"


#define SDL_AUDIO_FMT AUDIO_S16SYS
#define SDL_AUDIO_SAMPLES 1024

struct ManagerInfo {
    struct ChNode ch;
    struct ChNode ch_demux;
    struct ChNode ch_vdec;
    AVFrame ** current_frame_ptr;
    SDL_mutex * current_frame_mutex;
};
int manager_thread(void *);

struct AudioInfo {
    AVCodecContext * codec_ctx;
};
int audio_thread(void *);

struct DemuxInfo {
    struct ChNode ch;
    AVFormatContext * format_ctx;
    int vstream_idx;
    int astream_idx;
};
int demuxing_thread(void *);

struct VideoInfo {
    struct ChNode ch;
    AVCodecContext * codec_ctx;
};
int video_thread(void *);

