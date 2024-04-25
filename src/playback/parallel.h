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

enum MessageType {
    MSG_NONE,
    MSG_NEXT_FRAME_READY,
    MSG_SEEKED_FRAME_READY,
    MSG_ADVANCE_FRAME,
    MSG_SEEK,
};

struct MessageQueue {
    SDL_mutex * mutex;
    SDL_semaphore * count;
    struct QueuedMessage * first;
    struct QueuedMessage * last;
};

struct Message {
    uint64_t type;
    union {
        struct {
            int64_t pts;
            int64_t duration;
        } got_frame;
        struct {
            uint8_t * pixels;
            int64_t timestamp;
        } needed_frame;
    };
};

struct QueuedMessage {
    struct Message msg;
    struct QueuedMessage * next;
};

struct MessageQueue create_message_queue(void);
struct Message msgq_receive(struct MessageQueue * msgq);
struct Message msgq_wait_receive(struct MessageQueue * msgq);
void msgq_send(struct MessageQueue * msgq, struct Message msg);
void msgq_print(struct MessageQueue * msgq);

/* one side of a two-way communication channel.
 * get the other side with ch_remote_node()
 * it gets passed by value a lot cause its
 * basically a pointer */
struct ChNode {
    struct MessageQueue * msgq_in;
    struct MessageQueue * msgq_out;
};

struct ChNode create_channel(void);
struct ChNode ch_remote_node(struct ChNode local_node);
struct Message ch_receive(struct ChNode ch);
struct Message ch_wait_receive(struct ChNode ch);
void destroy_channel(struct ChNode node);
void ch_send(struct ChNode ch, struct Message msg);

struct DemuxInfo {
    AVFormatContext * format_ctx;
    struct PacketQueue * demuxed_vpktq;
    struct PacketQueue * demuxed_apktq;
    struct PacketQueue * decoded_pktq;
    struct ChNode ch;
    int vstream_idx;
    int astream_idx;
};
int demuxing_thread(void *);

struct ManagerInfo {
    struct ChNode ch;
    struct ChNode ch_demux;
    struct ChNode ch_vdec;
};
int manager_thread(void *);

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
    struct ChNode ch;
};
int video_thread(void *);

