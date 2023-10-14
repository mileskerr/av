#pragma once
#include "../av.h"

#define PACKET_QUEUE_SIZE 16

#define SDL_AUDIO_FMT AUDIO_S16SYS
#define SDL_AUDIO_SAMPLES 1024

/* stores packets in a circular buffer. does not perform bounds check
 *
 * we use two packet queues for each stream. 
 * the demuxer gets packets from the decoded queue and overwrites them with
 * new data, placing them in the demuxed queue. the decoder decodes packets 
 * from the demuxed queue and places them back in the decoded queue. this 
 * method avoids allocations and will never overflow the buffer */
struct PacketQueue {
    AVPacket * data[PACKET_QUEUE_SIZE];
    SDL_semaphore * capacity;
    SDL_mutex * mutex;
    int front_idx;
};
struct PacketQueue create_packet_queue(void);
void destroy_packet_queue(struct PacketQueue * pktq);
void pktq_fill(struct PacketQueue * pktq);
struct MessageQueue create_message_queue(void);
void msgq_receive(struct MessageQueue * msgq, void ** content);
void msgq_send(struct MessageQueue * msgq, void * content);

enum MsgCode {
/*  PLAY = true,
    PAUSE = false, */

    /* from main thread */
    SEEK,
    NEXT_FRAME,
    PREV_FRAME,

    /* to main thread */
    TS_CHANGED,
};

struct MessageQueue {
    int count;
    SDL_mutex * mutex;
    struct Message * first;
    struct Message * last;
};

struct Message {
    void * content;
    struct Message * next;
};

struct DemuxInfo {
    AVFormatContext * format_ctx;
    struct PacketQueue * demuxed_vpktq;
    struct PacketQueue * demuxed_apktq;
    struct PacketQueue * decoded_pktq;
    struct MessageQueue * msgq_in;
    struct MessageQueue * msgq_out;
    int vstream_idx;
    int astream_idx;
};
int demuxing_thread(void *);

struct AudioInfo {
    AVCodecContext * codec_ctx;
    struct PacketQueue * demuxed_pktq;
    struct PacketQueue * decoded_pktq;
};
int audio_thread(void *);

struct VideoInfo {
    AVCodecContext * codec_ctx;
    struct PacketQueue * demuxed_pktq;
    struct PacketQueue * decoded_pktq;
    struct MessageQueue * msgq_in;
    struct MessageQueue * msgq_out;
    struct FrameBuffer * fb;
};
int video_thread(void *);

