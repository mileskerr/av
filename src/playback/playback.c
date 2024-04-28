#include "playback.h"
#include "parallel.h"
#include "utils.h"
#include <libavformat/avformat.h>
#include <time.h>

extern bool quit;

/* data used to convert frames to a common format
 * sws_context is not used for scaling, only format conversion.
 * any scaling is done using SDL on the gpu */
struct VFrameConverter {
    struct SwsContext * sws_context;
    int linesize;
};

static struct VFrameConverter make_frame_converter(
    const AVCodecContext * const codec_ctx, const int format, const int linesize
) {
    return (struct VFrameConverter) {
        sws_getContext(
            codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
            codec_ctx->width, codec_ctx->height, format,
            SWS_POINT, NULL, NULL,
            NULL
        ),
        linesize
    };
}

static void destroy_frame_converter(struct VFrameConverter * frame_conv) {
    sws_freeContext(frame_conv->sws_context);
}

static void convert_frame(
    struct VFrameConverter * frame_conv, AVFrame * frame, uint8_t * pixels
) {
    //TODO: needs an array of linesizes to work with multi-plane images
    sws_scale(
        frame_conv->sws_context, 
        (const uint8_t *const *) frame->data,
        frame->linesize,
        0,
        frame->height,
        &pixels, 
        &frame_conv->linesize
    );    
}

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
    AVFormatContext * format_ctx;
    AVCodecContext * vcodec_ctx, * acodec_ctx;
    AVFrame * current_frame;
    SDL_mutex * current_frame_mutex;
    SDL_Thread * demuxer, * vdecoder, * adecoder, * manager;
    int astream_idx, vstream_idx;
    struct ChNode ch_demux, ch_vdec, ch_man;
    struct VFrameConverter frame_conv;
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

    *(struct InternalData *)ret->internal_data = (struct InternalData) {
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

    id->frame_conv = make_frame_converter(
        id->vcodec_ctx, AV_PIX_FMT_RGB24, get_texture_pitch(SDL_PIXELFORMAT_RGB24, id->vcodec_ctx->width)
    );

    id->ch_man = create_channel();
    id->ch_demux = create_channel();
    id->ch_vdec = create_channel();


    id->manager = SDL_CreateThread(
        manager_thread, "Manager",
        (void *) &(struct ManagerInfo) {
            .ch = ch_remote_node(id->ch_man),
            .ch_vdec = id->ch_vdec,
            .ch_demux = id->ch_demux,
            .current_frame_ptr = &id->current_frame,
            .current_frame_mutex = id->current_frame_mutex
        }
    );

    id->demuxer = SDL_CreateThread(
        demuxing_thread, "Demuxer", 
        (void *) &(struct DemuxInfo) {
            .ch = ch_remote_node(id->ch_demux),
            .format_ctx = id->format_ctx,
            .vstream_idx = id->vstream_idx,
            .astream_idx = id->astream_idx
        }
    );
    id->vdecoder = SDL_CreateThread(
        video_thread, "Video Decoder", 
        (void *) &(struct VideoInfo) {
            .ch = ch_remote_node(id->ch_vdec),
            .codec_ctx = id->vcodec_ctx,
        }
    );
    extern int threads_initialized;
    while (threads_initialized < 3);
}


int get_frame(struct PlaybackCtx * pb_ctx, SDL_Texture * tex, int64_t * pts, int64_t * duration) {
    struct InternalData * id = pb_ctx->internal_data;

    if (id->current_frame == NULL) return 1;
    
    int pitch;
    uint8_t * pixels;

    SDL_LockTexture(tex, NULL, (void **) &pixels, &pitch); 
    SDL_LockMutex(id->current_frame_mutex);

    *pts = id->current_frame->pts;
    *duration = id->current_frame->duration;

    convert_frame(&id->frame_conv, id->current_frame, pixels);

    SDL_UnlockTexture(tex);

    SDL_UnlockMutex(id->current_frame_mutex);

    return 0;
}

void advance_frame(struct PlaybackCtx * pb_ctx) {
    struct InternalData * id = pb_ctx->internal_data;

    ch_send(
        id->ch_man, 
        (struct Message) { .type = MSG_ADVANCE_FRAME }
    );
}

void seek(struct PlaybackCtx * pb_ctx, int ts) {
    //struct InternalData * id = pb_ctx->internal_data;
}

void destroy_playback_ctx(struct PlaybackCtx * pb_ctx) {
    struct InternalData * id = pb_ctx->internal_data;
    SDL_WaitThread(id->vdecoder, NULL);
    SDL_WaitThread(id->demuxer, NULL);

    destroy_frame_converter(&id->frame_conv);

    destroy_channel(id->ch_demux);
    destroy_channel(id->ch_vdec);

    avformat_close_input(&id->format_ctx);
    avcodec_free_context(&id->vcodec_ctx);
    
}
