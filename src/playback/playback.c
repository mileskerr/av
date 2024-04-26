#include "playback.h"
#include "parallel.h"
#include "comms.h"
#include <libavformat/avformat.h>
#include <time.h>

extern bool quit;

struct FrameBuffer create_framebuffer(
    SDL_Renderer * renderer, SDL_PixelFormatEnum pixel_format,
    uint32_t width, uint32_t height
) {
    struct FrameBuffer ret = {};
    ret.frame = SDL_CreateTexture(
        renderer, pixel_format, SDL_TEXTUREACCESS_STREAMING,
        width, height
    );
    ret.next_frame = SDL_CreateTexture(
        renderer, pixel_format, SDL_TEXTUREACCESS_STREAMING,
        width, height
    );

    return ret;
}

void destroy_framebuffer(struct FrameBuffer * fb) {
    SDL_DestroyTexture(fb->frame);
    SDL_DestroyTexture(fb->next_frame);
}

struct InternalData {
    SDL_Thread * demuxer, * vdecoder, * adecoder, * manager;
    AVFormatContext * format_ctx;
    AVCodecContext * vcodec_ctx, * acodec_ctx;
    int astream_idx, vstream_idx;
    uint64_t expected_frame_response_type;
    struct ChNode ch_demux, ch_vdec, ch_man;
    struct PacketQueue demuxed_vpktq, demuxed_apktq, decoded_pktq;
    struct FrameBuffer framebuffer;
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

void playback_to_renderer(struct PlaybackCtx * pb_ctx, SDL_Renderer * renderer) {
    struct InternalData * id = pb_ctx->internal_data;

    id->framebuffer = create_framebuffer(
        renderer, SDL_PIXELFORMAT_RGB24, id->vcodec_ctx->width, id->vcodec_ctx->height
    );


    id->demuxed_apktq = create_packet_queue();
    id->demuxed_vpktq = create_packet_queue();
    id->decoded_pktq = create_packet_queue();

    id->ch_man = create_channel();
    id->ch_demux = create_channel();
    id->ch_vdec = create_channel();

    pktq_fill(&id->decoded_pktq);


    id->manager = SDL_CreateThread(
        manager_thread, "Manager",
        (void *) &(struct ManagerInfo) {
            .ch = ch_remote_node(id->ch_man),
            .ch_vdec = id->ch_vdec,
            .ch_demux = id->ch_demux
        }
    );

    id->demuxer = SDL_CreateThread(
        demuxing_thread, "Demuxer", 
        (void *) &(struct DemuxInfo) {
            id->format_ctx,
            &id->demuxed_vpktq,
            &id->demuxed_apktq,
            &id->decoded_pktq,
            ch_remote_node(id->ch_demux),
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
            ch_remote_node(id->ch_vdec)
        }
    );
    extern int threads_initialized;
    while (threads_initialized < 3);
}


int get_frame(struct PlaybackCtx * pb_ctx, SDL_Texture ** tex, int64_t * pts, int64_t * duration) {
    struct InternalData * id = pb_ctx->internal_data;

    //TODO: fix this so it doesnt eat messages
    struct Message msg = ch_receive(id->ch_vdec);

    int ret;
    if ((ret = (msg.type == id->expected_frame_response_type))) {
        id->framebuffer.pts = msg.got_frame.pts;
        id->framebuffer.duration = msg.got_frame.duration;

        SDL_UnlockTexture(id->framebuffer.next_frame);

        SDL_Texture * tmp = id->framebuffer.frame;
        id->framebuffer.frame = id->framebuffer.next_frame;
        id->framebuffer.next_frame = tmp;        
    } 

    if (pts) *pts = id->framebuffer.pts;
    if (duration) *duration = id->framebuffer.duration;
    if (tex) *tex = id->framebuffer.frame;

    return ret;
}

void advance_frame(struct PlaybackCtx * pb_ctx) {
    struct InternalData * id = pb_ctx->internal_data;

    int pitch;
    uint8_t * pixels;
    SDL_LockTexture(id->framebuffer.next_frame, NULL, (void **)&pixels, &pitch);

    ch_send(
        id->ch_man, 
        (struct Message) {
            MSG_ADVANCE_FRAME, 
            .needed_frame = {.pixels = pixels }
        }
    );
    id->expected_frame_response_type = MSG_NEXT_FRAME_READY;
}

void seek(struct PlaybackCtx * pb_ctx, int ts) {
    struct InternalData * id = pb_ctx->internal_data;


    int pitch;
    uint8_t * pixels;
    SDL_LockTexture(id->framebuffer.next_frame, NULL, (void **)&pixels, &pitch);
    ch_send(
        id->ch_man, 
        (struct Message) {
            MSG_SEEK,
            .needed_frame = { .timestamp = ts, .pixels = pixels } 
        }
    );
    id->expected_frame_response_type = MSG_SEEKED_FRAME_READY;
}

void destroy_playback_ctx(struct PlaybackCtx * pb_ctx) {
    struct InternalData * id = pb_ctx->internal_data;
    SDL_WaitThread(id->vdecoder, NULL);
    SDL_WaitThread(id->demuxer, NULL);

    destroy_packet_queue(&id->demuxed_apktq);
    destroy_packet_queue(&id->demuxed_vpktq);
    destroy_packet_queue(&id->decoded_pktq);

    destroy_channel(id->ch_demux);
    destroy_channel(id->ch_vdec);

    avformat_close_input(&id->format_ctx);
    avcodec_free_context(&id->vcodec_ctx);
    
}
