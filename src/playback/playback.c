#include "playback.h"
#include "parallel.h"
#include <libavformat/avformat.h>


struct InternalData {
    SDL_Thread * demuxer, * vdecoder, * adecoder;
    AVFormatContext * format_ctx;
    AVCodecContext * vcodec_ctx, * acodec_ctx;
    int astream_idx, vstream_idx;
    struct PacketQueue demuxed_vpktq, demuxed_apktq, decoded_pktq;
};

static AVCodecContext * open_codec_context(AVFormatContext * format_ctx, int stream_idx) {
    const AVCodecParameters * codecpar = format_ctx->streams[stream_idx]->codecpar;
    const AVCodec * codec = avcodec_find_decoder(codecpar->codec_id);
    if (codec == NULL) return NULL;
    AVCodecContext * codec_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codec_ctx, codecpar))
        return NULL;
    if (avcodec_open2(codec_ctx, codec, NULL))
        return NULL;
    return codec_ctx;
}

struct PlaybackCtx * open_for_playback(char * filename) {
    AVFormatContext * format_ctx = NULL;
    if (avformat_open_input(&format_ctx, filename, NULL, NULL)) {
        fprintf(stderr, "failed to open `%s`", filename);
        return NULL;
    }

    avformat_find_stream_info(format_ctx, NULL);

    int vstream_idx = 
        av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1,-1, NULL, 0);

    int astream_idx =
        av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1,-1, NULL, 0);

    if (vstream_idx < 0) {
        fprintf(stderr, "failed to find video stream");
        return NULL;
    }

    /* if there's no audio stream, no problem. */
    AVStream * astream;
    AVCodecContext * acodec_ctx;
    if (astream_idx < 0) {
        astream = NULL;
        acodec_ctx = NULL;
    } else {
        astream = format_ctx->streams[astream_idx];
        acodec_ctx = open_codec_context(format_ctx, astream_idx);
        if (acodec_ctx == NULL) {
            fprintf(stderr, "unsupported audio codec");
            astream = NULL;
            astream_idx = -1;
        }
    }

    AVStream * vstream = format_ctx->streams[vstream_idx];

    AVCodecContext * vcodec_ctx =
        open_codec_context(format_ctx, vstream_idx);


    if (vcodec_ctx == NULL) {
        fprintf(stderr, "unsupported video codec");
        return NULL;
    }

    struct PlaybackCtx * ret;
    ret = malloc(sizeof(*ret));

    *ret = (struct PlaybackCtx){
        .width = vcodec_ctx->width,
        .height = vcodec_ctx->height,

        .time_base = vstream->time_base,
        .start_time = vstream->start_time,
        .duration = vstream->duration,

        .internal_data = malloc(sizeof(struct InternalData)),
    };

    *(struct InternalData *)ret->internal_data = (struct InternalData){
        .format_ctx = format_ctx,
        .vcodec_ctx = vcodec_ctx,
        .acodec_ctx = acodec_ctx,
        .astream_idx = astream_idx,
        .vstream_idx = vstream_idx
    };
    return ret;
}

void playback_to_framebuffer(struct PlaybackCtx * pb_ctx, struct FrameBuffer * fb) {
    struct InternalData * id = pb_ctx->internal_data;


    id->demuxed_apktq = create_packet_queue();
    id->demuxed_vpktq = create_packet_queue();
    id->decoded_pktq = create_packet_queue();

    pktq_fill(&id->decoded_pktq);


    id->demuxer = SDL_CreateThread(
        demuxing_thread, "Demuxer", 
        (void *) &(struct DemuxInfo) {
            id->format_ctx,
            &id->demuxed_vpktq,
            &id->demuxed_apktq,
            &id->decoded_pktq,
            id->vstream_idx,
            id->astream_idx
        }
    );
    id->vdecoder = SDL_CreateThread(
        video_thread, "Video Decoder", 
        (void *) &(struct VideoInfo) {
            id->vcodec_ctx,
            &id->demuxed_vpktq,
            &id->decoded_pktq,
            fb,
        }
    );  
    usleep(50); /* dirty hack to make sure the stack allocated variables stick around long enough */
}

void destroy_playback_ctx(struct PlaybackCtx * pb_ctx) {
    struct InternalData * id = pb_ctx->internal_data;
    SDL_WaitThread(id->vdecoder, NULL);
    SDL_WaitThread(id->demuxer, NULL);

    destroy_packet_queue(&id->demuxed_apktq);
    destroy_packet_queue(&id->demuxed_vpktq);
    destroy_packet_queue(&id->decoded_pktq);

    avformat_close_input(&id->format_ctx);
    avcodec_free_context(&id->vcodec_ctx);
    
}
