#include "parallel.h"
#include "utils.h"
#include <SDL2/SDL_mutex.h>
#include <SDL2/SDL_thread.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>

extern bool quit;

int threads_initialized = 0;

#define PACKET_QUEUE_SIZE 16
#define FRAME_QUEUE_SIZE 16

struct PacketQueue {
    AVPacket * data[PACKET_QUEUE_SIZE];
    SDL_semaphore * capacity;
    SDL_mutex * mutex;
    int front_idx;
};

struct FrameQueue {
    AVFrame * data[FRAME_QUEUE_SIZE];
    int capacity;
    int front_idx;
};

struct FrameQueue create_frame_queue(void) { return (struct FrameQueue) {0}; }

static void destroy_frame_queue( struct FrameQueue * frameq) {
    for (int i = 0; i < frameq->capacity; i++) {
        int idx = (i + frameq->front_idx) % FRAME_QUEUE_SIZE;
        av_frame_free(&frameq->data[idx]);
    }
}

static void queue_frame(struct FrameQueue * frameq, AVFrame * frame) {
    int back = (frameq->front_idx + frameq->capacity) % PACKET_QUEUE_SIZE;
    frameq->data[back] = frame;
    frameq->capacity++;
}

static AVFrame * dequeue_frame(struct FrameQueue * frameq) {
    if (!frameq->capacity) return NULL;
    frameq->capacity--;
    frameq->front_idx = (frameq->front_idx + 1) % FRAME_QUEUE_SIZE;
    return frameq->data[frameq->front_idx];
}

struct PacketQueue create_packet_queue(void) {
    return (struct PacketQueue) {
        .capacity = SDL_CreateSemaphore(0),
        .mutex = SDL_CreateMutex()
    };
}

static void destroy_packet_queue(struct PacketQueue * pktq) {
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

static int pktq_cap(struct PacketQueue * pktq) { return SDL_SemValue(pktq->capacity); }

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


int manager_thread(void * data) {
    struct ManagerInfo in = *(struct ManagerInfo * )data;

    threads_initialized += 1;

    #define PREFETCH_FRAMES 3


    struct PacketQueue pktq = create_packet_queue();
    struct FrameQueue frameq = create_frame_queue();

    int packets_requested, frames_requested;
    packets_requested = frames_requested = 0;

    while (!quit) {
        /* for seeking, make a state machine */
    
        while (
            (packets_requested + pktq_cap(&pktq)) < PREFETCH_FRAMES
        ) {
            ch_send(in.ch_demux,
                (struct Message) {
                    .type = MSG_DEMUX_PKT
                }
            );
            packets_requested++;
        }

        while (
            ((frames_requested + frameq.capacity) < PREFETCH_FRAMES) && pktq_cap(&pktq)
        ) {
            ch_send(in.ch_vdec, 
                (struct Message) {
                    .type = MSG_DECODE_FRAME,
                    .pkt = dequeue_pkt(&pktq)
                }
            );
            frames_requested++;
        }
        
        struct Message msg;
 
        msg = ch_receive(in.ch_demux);

        switch (msg.type) {
            case MSG_VIDEO_PKT_READY:
                queue_pkt(&pktq, msg.pkt);
            case MSG_NO_PKT_READY:
                packets_requested--;
                break;
        }

        msg = ch_receive(in.ch_vdec);

        switch (msg.type) {
            case MSG_VIDEO_FRAME_READY:
                queue_frame(&frameq, msg.frame);
            case MSG_NO_VIDEO_FRAME_READY:
                frames_requested--;
                break;
        }

        msg = ch_receive(in.ch);

        switch (msg.type) {
            case MSG_ADVANCE_FRAME:
                SDL_LockMutex(in.current_frame_mutex);

                if (frameq.capacity) {
                    //if (frameq.data[frameq.front_idx] != *in.current_frame_ptr) {
                    if (*in.current_frame_ptr)
                        av_frame_free(in.current_frame_ptr);
                    //}
                    *in.current_frame_ptr = dequeue_frame(&frameq);
                }

                SDL_UnlockMutex(in.current_frame_mutex);
                break;
        }

        usleep(10);
    }
    return 0;
}


int demuxing_thread(void * data) {
    struct DemuxInfo in = *(struct DemuxInfo *) data;
    
    threads_initialized++;

    while (!quit) {
        struct Message msg = ch_receive(in.ch);

        switch (msg.type) {
            case MSG_DEMUX_PKT:
                AVPacket * pkt = av_packet_alloc();
                int ret;
                if ((ret = av_read_frame(in.format_ctx, pkt))) {
                    fprintf(stderr, "Demuxing Error: %s\n", av_err2str(ret));
                    goto no_packet;
                }
                if (pkt->stream_index == in.vstream_idx) {
                    ch_send(
                        in.ch,
                        (struct Message) {
                            .type = MSG_VIDEO_PKT_READY,
                            .pkt = pkt
                        }
                    );
                    break;
                }
                no_packet:
                ch_send(
                    in.ch,
                    (struct Message) { .type = MSG_NO_PKT_READY }
                );
                break;
        }
    }
    return 0;
}

#define MAX_DECODE_SEEK_FRAMES 100

int video_thread(void * data) {
    struct VideoInfo in = *(struct VideoInfo *) data;

    threads_initialized++;

    while (!quit) {
        struct Message msg = ch_receive(in.ch);

        switch (msg.type) {
            case MSG_DECODE_FRAME:
                AVFrame * frame = av_frame_alloc();
                int ret;
                if ((ret = decode_frame(in.codec_ctx, msg.pkt, frame))) {
                    fprintf(stderr, "Decoding Error: %s\n", av_err2str(ret));
                    goto no_frame;
                }
                ch_send(in.ch,
                    (struct Message) {
                        MSG_VIDEO_FRAME_READY,
                        .frame = frame
                    }
                );
                break;
                no_frame:
                ch_send(in.ch,
                    (struct Message) { .type = MSG_NO_VIDEO_FRAME_READY }
                );
                break;
        }
    }
    return 0;
}
/*
int audio_thread(void * data) {
    struct AudioInfo * info = data;
    AVCodecContext * codec_ctx = info->codec_ctx;

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
             * this should work fine assuming most packets are
             * about the same size, which is potentially true
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
*/
