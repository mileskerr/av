#include "decode.h"

int open_movie(const char * filename, Movie ** ptr) {

    *ptr = calloc(1, sizeof(**ptr));
    Movie * mov = *ptr;

    if (avformat_open_input(&mov->format_ctx, filename, NULL, NULL))
        return -1;

    int vid_stream_idx = -1;
    if (avformat_find_stream_info(mov->format_ctx, NULL))
        return -1;

    for (int i = 0; i < mov->format_ctx->nb_streams; i++)
        if (mov->format_ctx->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_VIDEO) {
            vid_stream_idx = i;
            break;
    }

    if (vid_stream_idx == -1) return -1;
    mov->vid_stream_idx = vid_stream_idx;

    const AVCodecParameters * codecpar = mov->format_ctx->streams[vid_stream_idx]->codecpar;
    const AVCodec * codec = avcodec_find_decoder(codecpar->codec_id);
    mov->codec_ctx = avcodec_alloc_context3(codec);

    if (avcodec_parameters_to_context(mov->codec_ctx, codecpar))
        return -1;

    if (avcodec_open2(mov->codec_ctx, codec, NULL))
        return -1;

    av_dump_format(mov->format_ctx, 0, filename, 0);

    mov->sws_ctx = sws_getContext(
        mov->codec_ctx->width, mov->codec_ctx->height, mov->codec_ctx->pix_fmt,
        mov->codec_ctx->width, mov->codec_ctx->width, AV_PIX_FMT_RGB24,
        SWS_POINT, NULL, NULL,
        NULL
    );

    mov->dec_frame = av_frame_alloc();
    mov->dec_pkt = av_packet_alloc();

    if ((mov->sws_ctx == NULL) || (mov->dec_frame == NULL) || (mov->dec_pkt == NULL))
        return -1;

    if (!mov->sws_ctx)
        return -1;

    return 0;
}

void close_movie(Movie * mov) {
    avcodec_free_context(&mov->codec_ctx);
    av_packet_free(&mov->dec_pkt);
    av_frame_free(&mov->dec_frame);
    avformat_free_context(mov->format_ctx);
}

int get_frame(Movie * mov, uint8_t * pixels, const int pitch) {

    AVPacket * pkt = mov->dec_pkt;
    AVFrame * frame = mov->dec_frame;

    //av_frame_unref(frame);
    //av_packet_unref(pkt);

    do {
        av_read_frame(mov->format_ctx, pkt);
    } while ( pkt->stream_index != mov->vid_stream_idx);

    send_packet:
    avcodec_send_packet(mov->codec_ctx, pkt);
    int ret;
    switch (avcodec_receive_frame(mov->codec_ctx, frame)) {
        case 0:
            break;
        case AVERROR(EAGAIN):
            goto send_packet;
        default:
            return -1;
    }

    sws_scale(
        mov->sws_ctx, (const uint8_t *const *) frame->data, frame->linesize,
        0, mov->codec_ctx->height, &pixels, 
        &pitch
    );

    return 0;
}
