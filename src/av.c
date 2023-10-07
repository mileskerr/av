#include <SDL2/SDL.h>
#include <SDL2/SDL_pixels.h>
#include <SDL2/SDL_render.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include <unistd.h>
#include <stdint.h>


/* data used to convert frames to a common format
 * sws_context is not used for scaling, only format conversion.
 * any scaling is done using SDL on the gpu */
struct FrameConverter {
    struct SwsContext * sws_context;
    int linesize;
};

static AVCodecContext * open_codec_context(AVFormatContext * format_ctx, int stream_idx) {
    const AVCodecParameters * codecpar = format_ctx->streams[stream_idx]->codecpar;
    const AVCodec * codec = avcodec_find_decoder(codecpar->codec_id);
    AVCodecContext * codec_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codec_ctx, codecpar))
        return NULL;
    if (avcodec_open2(codec_ctx, codec, NULL))
        return NULL;
    return codec_ctx;
}

static struct FrameConverter make_frame_converter(
    const AVCodecContext * codec_ctx, const int format, const int linesize
) {
    return (struct FrameConverter) {
        sws_getContext(
            codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
            codec_ctx->width, codec_ctx->height, format,
            SWS_POINT, NULL, NULL,
            NULL
        ),
        linesize
    };
}

static void convert_frame(
    struct FrameConverter * frame_conv, AVFrame * frame, uint8_t * pixels
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

static void destroy_frame_converter(struct FrameConverter * frame_conv) {
    sws_freeContext(frame_conv->sws_context);
}

/* decode a packet containing a single frame and store it in frame_out.
 * returns 0 on success, -1 on failure, -2 on EOF */
static int decode_frame(AVCodecContext * codec_ctx, AVPacket * pkt, AVFrame * frame_out) {
send_packet:
    switch (avcodec_send_packet(codec_ctx, pkt)) {
        case 0: case AVERROR(EAGAIN): case AVERROR_EOF:
            break;
        default:
            return -1;
    }
    switch (avcodec_receive_frame(codec_ctx, frame_out)) {
        case 0:
            break;
        case AVERROR(EAGAIN):
            goto send_packet;
        case AVERROR_EOF:
            return -2;
        default:
            return -1;
    }
    return 0;
}

static int get_texture_pitch(SDL_Texture * texture) {
    int w, pitch;
    uint32_t format;
    SDL_QueryTexture(texture, &format, NULL, &w, NULL);
    return (w * SDL_BYTESPERPIXEL(format) + 3) & ~3;
}

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "provide filename\n");
        return -1;
    }
    char * filename = argv[1];

    /* get codec and format */

    AVFormatContext * format_ctx = NULL;
    if (avformat_open_input(&format_ctx, filename, NULL, NULL))
        return -1;


    int video_stream_idx = 
        av_find_best_stream(
            format_ctx, AVMEDIA_TYPE_VIDEO, -1,
            -1, NULL, 0
        );

    if (video_stream_idx < 0)
        return -1;


    AVCodecContext * codec_ctx =
        open_codec_context(format_ctx, video_stream_idx);

    if (codec_ctx == NULL)
        return -1;


    /* decode first frame */

    AVPacket * pkt = av_packet_alloc();
    AVFrame * frame = av_frame_alloc();

    do {
        av_read_frame(format_ctx, pkt);
    } while ( pkt->stream_index != video_stream_idx);

    decode_frame(codec_ctx, pkt, frame);


    /* display frame to screen */

    if (SDL_Init(SDL_INIT_VIDEO)) return 1;

    SDL_Window * window;
    SDL_Renderer * renderer;
    SDL_Texture * video_tex;

    SDL_CreateWindowAndRenderer(
        1000, 1000, 0,
        &window, &renderer
    );
    if ((window == NULL) || (renderer == NULL))
        return -1;

    video_tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        codec_ctx->width, codec_ctx->height
    );
    if (video_tex == NULL)
        return -1;
    
    struct FrameConverter frame_conv = make_frame_converter(
        codec_ctx, AV_PIX_FMT_RGB24, get_texture_pitch(video_tex)
    );

    uint8_t * pixels = NULL;
    int _;
    SDL_LockTexture(video_tex, NULL, (void **) &pixels, &_);

    convert_frame(&frame_conv, frame, pixels);

    SDL_UnlockTexture(video_tex);

    SDL_RenderCopy(renderer, video_tex, NULL, NULL);
    SDL_RenderPresent(renderer);

    sleep(3);

    /* clean up */
    destroy_frame_converter(&frame_conv);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avformat_close_input(&format_ctx);
    avcodec_free_context(&codec_ctx);
    SDL_DestroyTexture(video_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

