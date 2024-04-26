#include "ipc.h"

struct QueuedMessage {
    struct Message msg;
    struct QueuedMessage * next;
};

struct MessageQueue {
    SDL_mutex * mutex;
    SDL_semaphore * count;
    struct QueuedMessage * first;
    struct QueuedMessage * last;
};

struct ChNode {
    struct MessageQueue * msgq_in;
    struct MessageQueue * msgq_out;
};

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
