#pragma once
#include "../av.h"

enum MessageType {
    /* all */
    MSG_NONE,

    /* main -> manage */
    MSG_ADVANCE_FRAME,

    /* manage -> demux */
    MSG_DEMUX_PKT,

    /* manage -> vdec, manage -> adec */
    MSG_DECODE_FRAME,

    /* demux -> manage */
    MSG_VIDEO_PKT_READY,
    MSG_AUDIO_PKT_READY,
    MSG_NO_PKT_READY,
    
    /* vdec -> manage */
    MSG_VIDEO_FRAME_READY,
    MSG_NO_VIDEO_FRAME_READY,
};


struct Message {
    uint64_t type;
    union {
        AVPacket * pkt; /* MSG_VIDEO_PKT_READY */
        AVFrame * frame; /* MSG_VIDEO_FRAME_READY */
    };
};


/* one side of a two-way communication channel.
 * get the other side with ch_remote_node()
 * it gets passed by value a lot cause its
 * basically a pointer */
struct MessageQueue;

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
