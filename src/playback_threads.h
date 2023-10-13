#pragma once
#include "av.h"
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

/* buffer for storing frames as SDL_Texture:s 
 *
 * - video thread writes texture data to pixel_buf.    <-----------------+
 * - main thread waits for video to finish if necessary (hopefully not)  |
 * - main thread swaps the textures, points pixel_buf to the data of     |
 *   next_frame and signals to video thread that it can begin decoding   |
     the next frame                                                      |
 * - main thread reads video data from current_frame as... --------------+
 * 
 * mutex only guards next_frame, next_timestamp, and frame_needed. video thread 
 * should lock mutex the whole time it is decoding, and main thread should lock
 * it only while swapping the frames */
struct FrameBuffer {
    int duration, next_duration;
    SDL_Texture * frame, * next_frame;
    uint8_t * pixel_buf;
    bool frame_needed;
    SDL_mutex * mutex;
};
struct FrameBuffer create_framebuffer(
    SDL_Renderer * renderer, SDL_PixelFormatEnum pixel_format,
    uint32_t width, uint32_t height
);
/* swaps the buffers and signals that a new frame should be aquired.
 * fb->current_frame is ok to use until this function is called again on fb. */
void framebuffer_swap(struct FrameBuffer * fb);

struct DemuxInfo {
    AVFormatContext * format_ctx;
    struct PacketQueue * demuxed_vpktq;
    struct PacketQueue * demuxed_apktq;
    struct PacketQueue * decoded_pktq;
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
  struct FrameBuffer * fb;
};
int video_thread(void *);
