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
        SDL_CreateMutex(),
        SDL_CreateSemaphore(0),
        NULL, NULL 
    };
}

void msgq_print(struct MessageQueue * msgq) {
    SDL_LockMutex(msgq->mutex);

    struct QueuedMessage * next = msgq->first;
    printf("(MessageQueue) [ ");

    while (next) {
        printf("%ld", next->msg.type);
        next = next->next;
        if (next) printf(", "); else printf(" ");
    }

    printf("]\n");
    SDL_UnlockMutex(msgq->mutex);
}

struct Message msgq_peek(struct MessageQueue * msgq) {
    if (msgq->first) return msgq->first->msg;
    else return (struct Message) { .type = MSG_NONE };
}

void msgq_send(struct MessageQueue * msgq, struct Message msg) {
    SDL_LockMutex(msgq->mutex);

    struct QueuedMessage * qd_msg = malloc(sizeof(struct QueuedMessage));

    *qd_msg = (struct QueuedMessage) {
        .msg = msg,
        .next = NULL
    };

    if (msgq->last) {
        msgq->last->next = qd_msg;
        msgq->last = qd_msg;
    } else {
        msgq->first = msgq->last = qd_msg;
    }

    SDL_UnlockMutex(msgq->mutex);    

    SDL_SemPost(msgq->count);
}

struct Message msgq_wait_receive(struct MessageQueue * msgq) {
    SDL_LockMutex(msgq->mutex);

    SDL_SemWait(msgq->count);

    struct QueuedMessage * got_msg = msgq->first;
    struct Message ret = got_msg->msg;

    if (msgq->last == msgq->first) {
        msgq->last = msgq->first = NULL;
    } else {
        msgq->first = msgq->first->next;
    }
    free(got_msg);

    SDL_UnlockMutex(msgq->mutex);

    return ret;

}

struct Message msgq_receive(struct MessageQueue * msgq) {
    if (SDL_SemValue(msgq->count))
        return msgq_wait_receive(msgq);
    else
        return (struct Message) { MSG_NONE };
}

struct ChNode create_channel(void) {
    struct ChNode ret = {
        .msgq_in = malloc(sizeof(struct MessageQueue)),
        .msgq_out = malloc(sizeof(struct MessageQueue))
    };
    *ret.msgq_in = create_message_queue();
    *ret.msgq_out = create_message_queue();
    return ret;
}

void destroy_channel(struct ChNode node) {
    free(node.msgq_in);
    free(node.msgq_out);
}
struct ChNode ch_remote_node(struct ChNode local_node) {
    return (struct ChNode) {
        .msgq_in = local_node.msgq_out,
        .msgq_out = local_node.msgq_in
    };
}

struct Message ch_receive(struct ChNode ch) { return msgq_receive(ch.msgq_in); }

struct Message ch_wait_receive(struct ChNode ch) { return msgq_wait_receive(ch.msgq_in); }

void ch_send(struct ChNode ch, struct Message msg) { msgq_send(ch.msgq_out, msg); }

struct PacketQueue create_packet_queue(void) {
    return (struct PacketQueue) {
        .capacity = SDL_CreateSemaphore(0),
        .mutex = SDL_CreateMutex()
    };
}

void destroy_packet_queue(struct PacketQueue * pktq) {
    for (unsigned i = 0; i < SDL_SemValue(pktq->capacity); i++) {
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
    if (SDL_SemWaitTimeout(pktq->capacity, 10)) return NULL;

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


int demuxing_thread(void * data) {
    struct DemuxInfo * info = data;
    AVFormatContext * format_ctx = info->format_ctx;
    struct PacketQueue * decoded_pktq = info->decoded_pktq;
    struct PacketQueue * demuxed_vpktq = info->demuxed_vpktq;
    struct PacketQueue * demuxed_apktq = info->demuxed_apktq;
    struct ChNode ch = info->ch;
    int vstream_idx = info->vstream_idx;
    int astream_idx = info->astream_idx;

    threads_initialized += 1;

    uint32_t * msg;

    int start_time = format_ctx->streams[vstream_idx]->start_time;
    int seek_time = start_time - 1;


    while (!quit) {
        struct Message msg = ch_receive(ch);

        switch (msg.type) {
            case MSG_NONE: break;
            case MSG_SEEK: {
                seek_time = msg.needed_frame.timestamp;

                int ret;
                if ((ret = avformat_seek_file(
                    format_ctx, vstream_idx,
                    start_time , seek_time,
                    seek_time, 0
                ))) {
                    fprintf(stderr, "Error Seeking Packet: %s\n", av_err2str(ret));
                    break; /* break or continue??? */
                }


                pktq_drain(demuxed_vpktq, decoded_pktq);
            }
        }
        
        AVPacket * pkt = dequeue_pkt(decoded_pktq);

        if (pkt) {
            av_packet_unref(pkt);

            int ret;
            if ((ret = av_read_frame(format_ctx, pkt))) {
                /* the packet must be recycled into the queue if we aren't decoding it */

                /* maybe a problem here. when this errors (and stops queueing) decoding thread hangs and
                 * may miss messages or prevent the program from closing. TODO: impliment timeout for 
                 * dequeue_pkt(); */
                fprintf(stderr, "Demuxing Error: %s\n", av_err2str(ret));
                queue_pkt(decoded_pktq, pkt);
            } else if (pkt->stream_index == vstream_idx) {
                queue_pkt(demuxed_vpktq, pkt);
            } else
                queue_pkt(decoded_pktq, pkt);
        }
    }
    return 0;
}

int audio_thread(void * data) {
    struct AudioInfo * info = data;
    AVCodecContext * codec_ctx = info->codec_ctx;
    struct PacketQueue * demuxed_pktq = info->demuxed_pktq;
    struct PacketQueue * decoded_pktq = info->decoded_pktq;

    threads_initialized += 1;

    uint8_t * audio_buf = NULL;
    int audio_buf_len = 0;
    
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

#define MAX_DECODE_SEEK_FRAMES 100
    struct VideoInfo * info = data;
    AVCodecContext * codec_ctx = info->codec_ctx;
    struct PacketQueue * demuxed_pktq = info->demuxed_pktq;
    struct PacketQueue * decoded_pktq = info->decoded_pktq;
    struct ChNode ch = info->ch;

    threads_initialized += 1;

    AVFrame * frame = av_frame_alloc();

    struct VFrameConverter frame_conv = make_frame_converter(
        codec_ctx, AV_PIX_FMT_RGB24, get_texture_pitch(SDL_PIXELFORMAT_RGB24, codec_ctx->width)
    );

    while (!quit) {
        AVPacket * pkt;
        struct Message msg = ch_receive(ch);

        switch (msg.type) {
            case MSG_NONE: usleep(10); break;
            case MSG_SEEK: case MSG_GET_NEXT_FRAME:
                bool send = false;
                for (int i = 0; i < MAX_DECODE_SEEK_FRAMES; i++) {
                    pkt = dequeue_pkt(demuxed_pktq);

                    int ret;
                    if ((ret = decode_frame(codec_ctx, pkt, frame))) {
                        fprintf(stderr, "Decoding Error: %s\n", av_err2str(ret));
                        break;
                    }
                    queue_pkt(decoded_pktq, pkt);


                    if (msg.type == MSG_GET_NEXT_FRAME) {
                        send = true;
                        break;
                    }

                    int64_t nts = msg.needed_frame.timestamp;

                    if ((frame->pts + frame->duration >= nts) && (frame->pts <= nts)) {
                        send = true;
                        break;
                    }
                }

                if (send) {
                    convert_frame(&frame_conv, frame, msg.needed_frame.pixels);

                    ch_send(ch, (struct Message) {
                        msg.type == MSG_GET_NEXT_FRAME ? MSG_NEXT_FRAME_READY : MSG_SEEKED_FRAME_READY, 
                        .got_frame = { 
                            .pts = frame->pts,
                            .duration = frame->duration
                        }
                    });
                }

                break;
        }
    }
    return 0;
}
