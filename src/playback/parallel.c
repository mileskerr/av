#include "../av.h"
#include "parallel.h"
#include "playback.h"
#include "utils.h"
#include <SDL2/SDL_mutex.h>
#include <SDL2/SDL_thread.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>

extern bool quit;

int threads_initialized = 0;


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


/* decode a single frame from pkt.
 * returns 0 on success, AVERROR_EOF on end of file, nonzero error code on decode error. */
static int decode_frame(AVCodecContext * codec_ctx, AVPacket * pkt, AVFrame * frame_out) {
    int ret;
    send_packet:
    switch (ret = avcodec_send_packet(codec_ctx, pkt)) {
        case 0:
            break;
        default:
            return ret;
    }
    //return avcodec_receive_frame(codec_ctx, frame_out);
    switch (ret = avcodec_receive_frame(codec_ctx, frame_out)) {
        case AVERROR(EAGAIN):
            goto send_packet;
        case 0:
            break;
        default:
            return ret;
    }
    return 0;
}

struct MessageQueue create_message_queue(void) {
    return (struct MessageQueue) {
        0, SDL_CreateMutex(),
        NULL, NULL 
    };
}

void msgq_send(struct MessageQueue * msgq, void * content) {
    SDL_LockMutex(msgq->mutex);

    struct Message * msg = malloc(sizeof(struct Message));

    *msg = (struct Message) {
        .content = content,
        .next = NULL
    };

    if (msgq->last) {
        msgq->last->next = msg;
        msgq->last = msg;
    } else {
        msgq->first = msgq->last = msg;
    }

    msgq->count++;

    SDL_UnlockMutex(msgq->mutex);    
}

/* get the first message in the queue and store it in content, if there
 * is one. Otherwise, set content to NULL. caller is responsible for freeing
 * content */
void msgq_receive(struct MessageQueue * msgq, void ** content) {
    SDL_LockMutex(msgq->mutex);

    if (msgq->count) {
        struct Message * got_msg = msgq->first;
        *content = got_msg->content;
        if (msgq->last == msgq->first) {
            msgq->last = msgq->first = NULL;
        } else {
            msgq->first = msgq->first->next;
        }
        free(got_msg);
        msgq->count--;
    } else {
        *content = NULL;
    }

    SDL_UnlockMutex(msgq->mutex);
}

struct PacketQueue create_packet_queue(void) {
    return (struct PacketQueue) {
        .capacity = SDL_CreateSemaphore(0),
        .mutex = SDL_CreateMutex()
    };
}

void destroy_packet_queue(struct PacketQueue * pktq) {
    for (int i = 0; i < SDL_SemValue(pktq->capacity); i++) {
        int idx = (i + pktq->front_idx) % PACKET_QUEUE_SIZE;
        av_packet_free(&pktq->data[idx]);
    }
    SDL_DestroySemaphore(pktq->capacity);
}

static void queue_pkt(struct PacketQueue * pktq, AVPacket * pkt) {
    int back = (pktq->front_idx + SDL_SemValue(pktq->capacity)) % PACKET_QUEUE_SIZE;

    SDL_LockMutex(pktq->mutex);
    pktq->data[back] = pkt;
    SDL_UnlockMutex(pktq->mutex);
    SDL_SemPost(pktq->capacity);
}

static AVPacket * dequeue_pkt(struct PacketQueue * pktq) {
    SDL_SemWait(pktq->capacity);

    /* mutex is not needed because only one thread will ever be getting
     * from the queue, and putting and getting simultaneously is fine */

    AVPacket * ret = pktq->data[pktq->front_idx];
    pktq->front_idx = (pktq->front_idx + 1) % PACKET_QUEUE_SIZE;

    return ret;
}

static void pktq_drain(struct PacketQueue * src, struct PacketQueue * dst) {
    SDL_LockMutex(src->mutex);
    SDL_LockMutex(dst->mutex);


    int dst_back = (dst->front_idx + SDL_SemValue(dst->capacity)) % PACKET_QUEUE_SIZE;
    int src_front = src->front_idx;

    int iters = SDL_SemValue(src->capacity);
    for (int i = 0; i < iters; i++) {
        int src_idx = (src_front + i) % PACKET_QUEUE_SIZE;
        int dst_idx = (dst_back + i) % PACKET_QUEUE_SIZE;
        dst->data[dst_idx] = src->data[src_idx];
        SDL_SemPost(dst->capacity);
        SDL_SemWait(src->capacity);
    }


    SDL_UnlockMutex(src->mutex);
    SDL_UnlockMutex(dst->mutex);
}

void pktq_fill(struct PacketQueue * pktq) {
    for (int i = 0; i < PACKET_QUEUE_SIZE; i++) {
        queue_pkt(pktq, av_packet_alloc());
    }
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
    ret.mutex = SDL_CreateMutex();

    return ret;
}

void destroy_framebuffer(struct FrameBuffer * fb) {
    SDL_DestroyTexture(fb->frame);
    SDL_DestroyTexture(fb->next_frame);
    SDL_DestroyMutex(fb->mutex);
}

int framebuffer_swap(struct FrameBuffer * fb, bool wait) {
    if (wait) {
        SDL_LockMutex(fb->mutex);
    } else {
        if (SDL_TryLockMutex(fb->mutex)) return 1;
    }

    if (fb->frame_needed); /* we haven't even started decoding this frame yet. lets not get ahead of ourselves */
    else {
        SDL_UnlockTexture(fb->next_frame);

        SDL_Texture * new_frame = fb->next_frame;
        fb->next_frame = fb->frame;
        fb->frame = new_frame;
        fb->duration = fb->next_duration;
        fb->frame_needed = true;

        int _;
        if (SDL_LockTexture(fb->next_frame, NULL, (void **)&fb->pixel_buf, &_)) {
            fprintf(stderr, "error locking texture: %s\n", SDL_GetError());
        }
    }

    SDL_UnlockMutex(fb->mutex);
    return 0;
}

void wait_for_frame_needed(struct FrameBuffer * fb) {
    while (!quit) {
        SDL_LockMutex(fb->mutex);

        if (fb->frame_needed) {
            break;
        }

        SDL_UnlockMutex(fb->mutex);
        usleep(80); /* wait for about a tenth of a screen refresh */
    }
}

void frame_ready(struct FrameBuffer * fb) {
    fb->frame_needed = false;
    SDL_UnlockMutex(fb->mutex);
}

#define DONT_DISPLAY 1

int demuxing_thread(void * data) {
    struct DemuxInfo * info = data;
    AVFormatContext * format_ctx = info->format_ctx;
    struct PacketQueue * decoded_pktq = info->decoded_pktq;
    struct PacketQueue * demuxed_vpktq = info->demuxed_vpktq;
    struct PacketQueue * demuxed_apktq = info->demuxed_apktq;
    struct MessageQueue * msgq_in = info->msgq_in;
    struct MessageQueue * msgq_out = info->msgq_out;
    int vstream_idx = info->vstream_idx;
    int astream_idx = info->astream_idx;

    threads_initialized += 1;

    uint32_t * msg;

    int start_time = format_ctx->streams[vstream_idx]->start_time;
    int seek_time = start_time - 1;

    bool seeking = false;
    int last_ts = start_time;

    while (!quit) {

        msgq_receive(msgq_in, (void**)&msg);

        if (!msg) goto nomsg;

        switch (*msg) {
            case SEEK: {
                seek_time = msg[1];
                seeking = true;

                if (avformat_seek_file(
                    format_ctx, vstream_idx,
                    start_time , seek_time,
                    seek_time, 0
                )) break;


                pktq_drain(demuxed_vpktq, decoded_pktq);
            }
        }

        nomsg:;
        
        AVPacket * pkt = dequeue_pkt(decoded_pktq);

        av_packet_unref(pkt);

        int ret;
        if ((ret = av_read_frame(format_ctx, pkt))) {
            /* the packet must be recycled into the queue if we aren't decoding it */
            fprintf(stderr, "%s", av_err2str(ret));
            queue_pkt(decoded_pktq, pkt);
        } else if (pkt->stream_index == vstream_idx) {
            pkt->opaque = (void *)0;
            if (pkt->dts < seek_time) {
                pkt->opaque = (void *)DONT_DISPLAY;
            } else if (seeking) {
                seeking = false;
                uint32_t * content = malloc(8);
                content[0] = TS_CHANGED;
                content[1] = last_ts;
                msgq_send(msgq_out, content);
            }
            last_ts = pkt->dts;
            queue_pkt(demuxed_vpktq, pkt);
        } else
            queue_pkt(decoded_pktq, pkt);
    }
    return 0;
}

/* this should probably be rewritten as a callback to avoid the double queueing */
int audio_thread(void * data) {
    struct AudioInfo * info = data;
    AVCodecContext * codec_ctx = info->codec_ctx;
    struct PacketQueue * demuxed_pktq = info->demuxed_pktq;
    struct PacketQueue * decoded_pktq = info->decoded_pktq;

    threads_initialized += 1;

    uint8_t * audio_buf = NULL;
    size_t audio_buf_len = 0;
    
    AVFrame * frame = av_frame_alloc();

    SDL_AudioSpec aspec;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(
        0, 0,
        &(SDL_AudioSpec) {
            .freq = codec_ctx->sample_rate,
            .format = SDL_AUDIO_FMT,
            .channels = codec_ctx->ch_layout.nb_channels,
            .silence = 0,
            .samples = SDL_AUDIO_SAMPLES,
            .callback = NULL,
            .userdata = NULL,
        },
        &aspec, 0
    );

    SDL_PauseAudioDevice(adev, 0);

    struct SwrContext * swr_ctx = NULL;
    AVChannelLayout ch_layout = nb_ch_to_av_ch_layout(aspec.channels);
    if (codec_ctx) {
        swr_alloc_set_opts2(
            &swr_ctx,
            &ch_layout,
            sample_fmt_sdl_to_av(aspec.format),
            aspec.freq,
            &codec_ctx->ch_layout,
            codec_ctx->sample_fmt,
            codec_ctx->sample_rate,
            0,
            NULL
        );
        if (swr_init(swr_ctx)) {
            fprintf(stderr, "failed to create audio resampling context");
            avcodec_free_context(&codec_ctx);
        };
    }
    while (!quit) {
        AVPacket * pkt = dequeue_pkt(demuxed_pktq);

        int ret;
        if ((ret = decode_frame(codec_ctx, pkt, frame))) {
            printf("%s\n", av_err2str(ret));
        }
        int len;
        av_samples_get_buffer_size(
            &len, 
            aspec.channels, 
            frame->nb_samples,
            sample_fmt_sdl_to_av(aspec.format),
            1
        );
        if (len > audio_buf_len) { 
            /* this should work fine assuming most packets are
             * about the same size, which is potentially true */
            if (audio_buf) free(audio_buf);
            audio_buf = malloc(len);
            audio_buf_len = len;
        }
        swr_convert(
            swr_ctx,
            &audio_buf,
            audio_buf_len,
            (const uint8_t **) frame->data,
            frame->nb_samples
        );
        SDL_QueueAudio(adev, audio_buf, audio_buf_len);   
    }
    return 0;
}


int video_thread(void * data) {
    struct VideoInfo * info = data;
    AVCodecContext * codec_ctx = info->codec_ctx;
    struct MessageQueue * msgq_in = info->msgq_in;
    struct PacketQueue * demuxed_pktq = info->demuxed_pktq;
    struct PacketQueue * decoded_pktq = info->decoded_pktq;
    struct FrameBuffer * fb = info->fb;

    threads_initialized += 1;

    AVFrame * frame = av_frame_alloc();

    struct VFrameConverter frame_conv = make_frame_converter(
        codec_ctx, AV_PIX_FMT_RGB24, get_texture_pitch(fb->frame)
    );

    uint32_t * msg;

    while (!quit) {
        //msgq_receive(msgq, (void**)&msg);

        AVPacket * pkt = dequeue_pkt(demuxed_pktq);

        bool display = !((uint64_t)pkt->opaque == DONT_DISPLAY);


        int error;
        if ((error = decode_frame(codec_ctx, pkt, frame)))
            fprintf(stderr, "%s\n", av_err2str(error));

        if (display && !error) {
            wait_for_frame_needed(fb);
            convert_frame(&frame_conv, frame, fb->pixel_buf);
            fb->next_duration = pkt->duration;
            frame_ready(fb);
        }

        queue_pkt(decoded_pktq, pkt);
    }
    return 0;
}