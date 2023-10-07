#pragma once
#include "av.h"

/* Handle to a video+audio file for decoding */
struct {
    AVFormatContext * format_ctx;
    AVCodecContext * codec_ctx;

    /* not used for scaling, but for changing the video format
     * scaling is done by SDL on the gpu */
    struct SwsContext * sws_ctx;

    int vid_stream_idx;

    /* the same frame and packet structs are reused to decode
     * each frame */
    AVFrame * dec_frame;
    AVPacket * dec_pkt;
} typedef Movie;

int open_movie(const char * filename, Movie ** ptr);

void close_movie(Movie * mov);

/* gets the next frame of video from vid and stores its
 * pixels into pixels. */
int get_frame(Movie * mov, uint8_t * pixels, const int pitch);
