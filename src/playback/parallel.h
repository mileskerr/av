#pragma once
#include "../av.h"
#include "ipc.h"


#define SDL_AUDIO_FMT AUDIO_S16SYS
#define SDL_AUDIO_SAMPLES 1024

struct ManageInfo {
    struct ChNode ch;
    struct ChNode ch_demux;
    struct ChNode ch_vdec;
    struct ChNode ch_adec;
    AVFrame ** current_frame_ptr;
    SDL_mutex * current_frame_mutex;
};
int thread_manage(void *);

struct ADecodeInfo {
    struct ChNode ch;
    AVCodecContext * codec_ctx;
};
int thread_adec(void *);

struct DemuxInfo {
    struct ChNode ch;
    AVFormatContext * format_ctx;
    int vstream_idx;
    int astream_idx;
};
int thread_demux(void *);

struct VDecodeInfo {
    struct ChNode ch;
    AVCodecContext * codec_ctx;
};
int thread_vdec(void *);

