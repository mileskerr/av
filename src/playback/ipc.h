#pragma once
#include "../av.h"

enum MessageType {
    MSG_NONE,
    MSG_NEXT_FRAME_READY,
    MSG_SEEKED_FRAME_READY,
    MSG_ADVANCE_FRAME,
    MSG_SEEK,
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

struct MessageQueue;

/*
struct MessageQueue create_message_queue(void);
struct Message msgq_receive(struct MessageQueue * msgq);
struct Message msgq_wait_receive(struct MessageQueue * msgq);
void msgq_send(struct MessageQueue * msgq, struct Message msg);
void msgq_print(struct MessageQueue * msgq);
*/

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
