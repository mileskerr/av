#include "decode.h"
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

AVFormatContext * format_ctx;
AVCodecContext * codec_ctx;

int video_stream = -1;


int init_dec(const char * filename) {
    TRY(avformat_open_input(&format_ctx, filename, NULL, NULL));

    if (avformat_find_stream_info(format_ctx, NULL)) return -1;
    for (int i = 0; i < format_ctx->nb_streams; i++)
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
          video_stream = i;
          break;
        }
    if (video_stream == -1) return -1;

    const AVCodecParameters * codecpar = format_ctx->streams[video_stream]->codecpar;
    const AVCodec * codec = avcodec_find_decoder(codecpar->codec_id);
    codec_ctx = avcodec_alloc_context3(codec);

    TRY(avcodec_parameters_to_context(codec_ctx, codecpar));
    TRY(avcodec_open2(codec_ctx, codec, NULL));

    av_dump_format(format_ctx, 0, filename, 0);

    return 0;
}

int dec_get_pixels(uint8_t * pixels, int w, int h, int pitch) {
    struct SwsContext * sws_ctx = sws_getContext(
        codec_ctx->width,   codec_ctx->height, codec_ctx->pix_fmt,
        w,                  h,                 AV_PIX_FMT_RGB24,
        SWS_BILINEAR,       NULL,              NULL,
        NULL
    );

    AVFrame * frame = av_frame_alloc();
    AVPacket * pkt = av_packet_alloc();


    do {
        TRY(av_read_frame(format_ctx, pkt));
    } while ( pkt->stream_index != video_stream );

    do {
        TRY(avcodec_send_packet(codec_ctx, pkt));
        int ret;
        if ((ret = avcodec_receive_frame(codec_ctx, frame)) != AVERROR(EAGAIN)) { 
          TRY(ret);
          break;
        }
    } while (1);

    sws_scale(
        sws_ctx, (const uint8_t *const *) frame->data, frame->linesize,
        0, codec_ctx->height, &pixels, 
        &pitch
    );

    return 0;
}

int dec_get_frame(AVFrame * out_frame, int w, int h) {
    struct SwsContext * sws_ctx = sws_getContext(
        codec_ctx->width,   codec_ctx->height, codec_ctx->pix_fmt,
        w,                  h,                 AV_PIX_FMT_RGB24,
        SWS_BILINEAR,       NULL,              NULL,
        NULL
    );

    AVFrame * frame = av_frame_alloc();
    AVPacket * pkt = av_packet_alloc();


    do {
        TRY(av_read_frame(format_ctx, pkt));
    } while ( pkt->stream_index != video_stream );

    do {
        TRY(avcodec_send_packet(codec_ctx, pkt));
        int ret;
        if ((ret = avcodec_receive_frame(codec_ctx, frame)) != AVERROR(EAGAIN)) { 
          TRY(ret);
          break;
        }
    } while (1);

    TRY(sws_frame_start(sws_ctx, out_frame, frame));
    TRY(sws_send_slice(sws_ctx, 0, frame->height));
    sws_receive_slice(sws_ctx, 0, frame->height);
    sws_frame_end(sws_ctx);

    sws_scale_frame(sws_ctx, out_frame, frame);

    av_frame_free(&frame);
    av_packet_free(&pkt);

    return 0;
}
