#include "av.h"
#include "drw.h"
#include <SDL2/SDL_audio.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

#define TO_CLOCKS(SECONDS) ((clock_t) (((float) SECONDS) * ((float) CLOCKS_PER_SEC)))
const clock_t REFRESH_TIME = TO_CLOCKS(0.02);

#define SDL_AUDIO_FMT AUDIO_S16SYS
#define SDL_AUDIO_SAMPLES 1024


/* data used to convert frames to a common format
 * sws_context is not used for scaling, only format conversion.
 * any scaling is done using SDL on the gpu */
struct VFrameConverter {
    struct SwsContext * sws_context;
    int linesize;
};


static AVCodecContext * open_codec_context(AVFormatContext * format_ctx, int stream_idx) {
    const AVCodecParameters * codecpar = format_ctx->streams[stream_idx]->codecpar;
    const AVCodec * codec = avcodec_find_decoder(codecpar->codec_id);
    if (codec == NULL) return NULL;
    AVCodecContext * codec_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codec_ctx, codecpar))
        return NULL;
    if (avcodec_open2(codec_ctx, codec, NULL))
        return NULL;
    return codec_ctx;
}

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

static void destroy_frame_converter(struct VFrameConverter * frame_conv) {
    sws_freeContext(frame_conv->sws_context);
}

/* decode a packet containing a single frame and store it in frame_out.
 * returns 0 on success, AVERROR(EAGAIN) if needs more data, AVERROR_EOF,
 * on end of file, nonzero error code on decode error. */
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


static int get_texture_pitch(SDL_Texture * texture) {
    int w;

    uint32_t format;
    SDL_QueryTexture(texture, &format, NULL, &w, NULL);
    return (w * SDL_BYTESPERPIXEL(format) + 3) & ~3;
}


struct PlayerState {
    int window_w, window_h;
    double cur_timestamp;
    bool playing;
};


struct VideoInfo {
    int w, h;
    double duration, start_time, avg_fps;
};


enum Action {
    SEEK_FWD = 1 << 1,
    SEEK_BACK = 1 << 2,
    WINDOW_RESIZE = 1 << 3,
    PLAY_PAUSE = 1 << 4,
    QUIT = 1 << 5,
};


static enum Action handle_event(
    SDL_Event * event,
    struct VideoInfo * vid,
    struct PlayerState * state,
    const struct Layout * layout
) {
    int nkeys;
    const uint8_t * keys = SDL_GetKeyboardState(&nkeys);

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    switch (event->type) {
        case SDL_MOUSEWHEEL:
            if (SDL_PointInRect(&(SDL_Point){ mouse_x, mouse_y }, &layout->timeline_rect)) {
                double seek_by;
                double new_ts;
                if (keys[SDL_SCANCODE_LSHIFT])
                    seek_by = event->wheel.y * 1.0;
                else if (keys[SDL_SCANCODE_LCTRL])
                    seek_by = event->wheel.y * 1.0/vid->avg_fps;
                else
                    seek_by = event->wheel.y * 10.0/vid->avg_fps;

                new_ts = state->cur_timestamp + seek_by;

                if (new_ts < vid->start_time) {
                    state->cur_timestamp = vid->start_time;
                    return SEEK_BACK;
                } else if (new_ts > vid->duration) {
                    state->cur_timestamp = vid->duration;
                    return SEEK_FWD;
                } else {
                    state->cur_timestamp = new_ts;
                    if (seek_by > 0)
                        return SEEK_FWD;
                    else
                        return SEEK_BACK;
                }
            }
            break;

        case SDL_KEYDOWN:
            switch (event->key.keysym.sym) {
                case SDLK_ESCAPE:
                    return QUIT;
                case SDLK_SPACE:
                    state->playing = !state->playing;
                    return PLAY_PAUSE;
            }
            break;

        case SDL_QUIT:
            return QUIT;

        case SDL_WINDOWEVENT:
            if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                state->window_w = event->window.data1;
                state->window_h = event->window.data2;
                return WINDOW_RESIZE;
            }
            break;
    }
    return 0;
}


static double avr_to_dbl(AVRational rational) {
    return ((double) rational.num) / ((double) rational.den);
}

static TTF_Font * default_font(int size) {
    return TTF_OpenFont("fonts/RobotoMono-Regular.ttf", size);
}

/* get default viewer state. */
static struct PlayerState default_state(void) {
    return (struct PlayerState) {
        .window_h = 1000,
        .window_w = 1000,
        .playing = false,
    };
}

static AVChannelLayout nb_ch_to_av_ch_layout(int n) {
    switch (n) {
        case 1: default:
            return (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        case 2:
            return (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        case 4:
            return (AVChannelLayout)AV_CHANNEL_LAYOUT_QUAD;
        case 6:
            return (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1;
    }
}

static enum AVSampleFormat sample_fmt_sdl_to_av(int sdl_fmt) {
    switch (sdl_fmt) {
        case AUDIO_U8: return AV_SAMPLE_FMT_U8;
        case AUDIO_S16SYS:
            return AV_SAMPLE_FMT_S16;
        case AUDIO_S32SYS:
            return AV_SAMPLE_FMT_S32;
        case AUDIO_F32SYS:
            return AV_SAMPLE_FMT_FLT;
        default: return -1;
    }    
}

int main(int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr, "provide filename\n");
        return -1;
    }
    char * filename = argv[1];

    /* get codecs and format */
    
    AVFormatContext * format_ctx = NULL;
    if (avformat_open_input(&format_ctx, filename, NULL, NULL)) {
        fprintf(stderr, "failed to open `%s`", filename);
        return -1;
    }

    avformat_find_stream_info(format_ctx, NULL);

    int vstream_idx = 
        av_find_best_stream(
            format_ctx, AVMEDIA_TYPE_VIDEO, -1,
            -1, NULL, 0
        );

    int astream_idx =
        av_find_best_stream(
            format_ctx, AVMEDIA_TYPE_AUDIO, -1,
            -1, NULL, 0
        );

    if (vstream_idx < 0) {
        fprintf(stderr, "failed to find video stream");
        return -1;
    }

    /* if there's no audio stream, no problem. */
    AVStream * astream;
    AVCodecContext * acodec_ctx;
    if (astream_idx < 0) {
        astream = NULL;
        acodec_ctx = NULL;
    } else {
        astream = format_ctx->streams[astream_idx];
        acodec_ctx = open_codec_context(format_ctx, astream_idx);
        if (acodec_ctx == NULL) {
            fprintf(stderr, "unsupported audio codec");
            astream = NULL;
            astream_idx = -1;
        }
    }

    AVStream * vstream = format_ctx->streams[vstream_idx];

    AVCodecContext * vcodec_ctx =
        open_codec_context(format_ctx, vstream_idx);


    if (vcodec_ctx == NULL) {
        fprintf(stderr, "unsupported video codec");
        return -1;
    }


    /* create packet and frame buffers, then decode first frame */

    AVPacket * pkt = av_packet_alloc();
    AVFrame * frame = av_frame_alloc();

    if (!pkt) { fprintf(stderr, "coudn't allocate packet"); return -1; }
    if (!frame) { fprintf(stderr, "coudn't allocate frame"); return -1; }


    /* initialize SDL */

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 1;

    SDL_Window * window;
    SDL_Renderer * renderer;
    SDL_Texture * video_tex;

    SDL_CreateWindowAndRenderer(
        1000, 1000, 0,
        &window, &renderer
    );
    if ((window == NULL) || (renderer == NULL)) {
        fprintf(stderr, "failed to open window");
        return -1;
    }

    video_tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        vcodec_ctx->width, vcodec_ctx->height
    );
    if (video_tex == NULL) return -1;


    int num_adev = SDL_GetNumAudioDevices(0);
    printf("%d\n",num_adev);
    for (int i = 0; i < num_adev; i++) {
        printf("%d: %s\n", i, SDL_GetAudioDeviceName(i, 0));
    }


    SDL_AudioSpec aspec;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(
        0, 0,
        &(SDL_AudioSpec) {
            .freq = acodec_ctx->sample_rate,
            .format = SDL_AUDIO_FMT,
            .channels = acodec_ctx->ch_layout.nb_channels,
            .silence = 0,
            .samples = SDL_AUDIO_SAMPLES,
            .callback = NULL,
            .userdata = NULL,
        },
        &aspec, 0
    );

    SDL_PauseAudioDevice(adev, 0);

    TTF_Init();
    TTF_Font * font = default_font(13);




    /* misc initializations */
    
    struct VFrameConverter frame_conv = make_frame_converter(
        vcodec_ctx, AV_PIX_FMT_RGB24, get_texture_pitch(video_tex)
    );


    struct SwrContext * swr_ctx = NULL;
    AVChannelLayout ch_layout = nb_ch_to_av_ch_layout(aspec.channels);
    if (acodec_ctx) {
        swr_alloc_set_opts2(
            &swr_ctx,
            &ch_layout,
            sample_fmt_sdl_to_av(aspec.format),
            aspec.freq,
            &acodec_ctx->ch_layout,
            acodec_ctx->sample_fmt,
            acodec_ctx->sample_rate,
            0,
            NULL
        );
        if (swr_init(swr_ctx)) {
            fprintf(stderr, "failed to create audio resampling context");
            avcodec_free_context(&acodec_ctx);
        };
    }

    struct ColorScheme colors = default_colors();
    struct PlayerState player_state = default_state();
    SDL_Event event;
    double time_base = avr_to_dbl(vstream->time_base);
    clock_t next_frame_time = 0;

    #define TIMELINE_HEIGHT 38
    #define PROGRESS_HEIGHT 20

    struct Layout layout = get_layout(
        player_state.window_w, player_state.window_h,
        vcodec_ctx->height, vcodec_ctx->width, TIMELINE_HEIGHT, PROGRESS_HEIGHT
    );

    struct VideoInfo vid_info = {
        .w = frame->width, .h = frame->height,
        .duration = ((double) vstream->duration) * time_base,
        .start_time = ((double) vstream->start_time) * time_base,
        .avg_fps = avr_to_dbl(vstream->avg_frame_rate)
    };

    uint8_t * audio_buf = NULL;
    size_t audio_buf_len = 0;

    /* main loop */
    for (;;) {
        clock_t start_time = clock();
        enum Action action = 0;
        bool redraw, newframe;
        redraw = newframe = false;

        /* handle events */
        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            action |= handle_event(
                &event, &vid_info, &player_state, &layout
            );
        }

        SDL_Point mouse_pt = {};
        uint32_t mouse = SDL_GetMouseState(&mouse_pt.x, &mouse_pt.y);
        if (
            (mouse & SDL_BUTTON(1)) &&
            SDL_PointInRect(&mouse_pt, &layout.progress_rect)
        ) {
            double position = 
                (double)(mouse_pt.x - layout.progress_rect.x)/
                (double)(layout.progress_rect.w);
            double ts = position * vid_info.duration;

            if (ts > player_state.cur_timestamp)
                action |= SEEK_FWD;
            else
                action |= SEEK_BACK;

            player_state.cur_timestamp = ts;
        }

        if (action & QUIT)
            goto quit;

        /* handle seeking */

        if (action & (SEEK_FWD | SEEK_BACK)) {
            newframe = true;
            AVRational fps = vstream->avg_frame_rate;
            uint64_t ts = 
                player_state.cur_timestamp / time_base;
            uint64_t max_ts = 
                ts + avr_to_dbl((AVRational){ fps.den, fps.num }) / time_base;
            int flags;
            if (action & SEEK_BACK)
                flags = AVSEEK_FLAG_BACKWARD;
            else
                flags = 0;
            avformat_seek_file(
                format_ctx,
                vstream_idx,
                -0xffffff, ts, max_ts,
                flags
            );
        }

        if (action & PLAY_PAUSE)
            next_frame_time = clock();

        int t;
        if (
            player_state.playing && 
            ((t = clock()) > next_frame_time)
        ) newframe = true;

        

        /* demux and decode */

        if  (newframe) {
            /* seeking can only take us to keyframes, so we may
             * need to read a few frames before we get to the desired
             * timestamp. if the video is playing normally loop
             * should not repeat */
            bool reached_end = false;
            int vdecode_res = -1;
            do {
                for(;;) {
                    if (av_read_frame(format_ctx, pkt)) {
                        /* in case the duration estimate is wrong and we reach EOF without
                         * passing cur_timestamp, don't keep reading packets forever */
                        return -1;
                    }
                    if (pkt->stream_index == vstream_idx) {
                        vdecode_res = decode_frame(vcodec_ctx, pkt, frame);
                        break;
                    }
                    else if (pkt->stream_index == astream_idx) {
                        if (!player_state.playing) continue;
                        int ret;
                        if ((ret = decode_frame(acodec_ctx, pkt, frame))) {
                            printf("%s\n", av_err2str(ret)); continue;
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
                    av_packet_unref(pkt);
                }


            } while(
                !reached_end &&
                (pkt->dts < player_state.cur_timestamp/time_base - 1.0/vid_info.avg_fps)
            );

            switch (vdecode_res) {
                case 0: {
                    uint8_t * pixels = NULL;
                    int _;
                    SDL_LockTexture(video_tex, NULL, (void **) &pixels, &_);

                    convert_frame(&frame_conv, frame, pixels);

                    SDL_UnlockTexture(video_tex);
                    redraw = true;
                    next_frame_time += ((double)pkt->duration * time_base) * CLOCKS_PER_SEC;
                    int delay = clock() - next_frame_time;
                    if (delay > 0)
                        fprintf(stderr, "behind by %lf\n", (double)delay/CLOCKS_PER_SEC);
                    player_state.cur_timestamp = (double)pkt->dts * time_base;
                    break;
                }
                case AVERROR(EOF): /* EOF */
                    player_state.cur_timestamp = vid_info.duration;
                    player_state.playing = false;
                    break;
                default: /* Decode Error */
                    return -1;
            }
        }

        if (action & WINDOW_RESIZE)
            redraw = true;

        /* draw screen */
        if (redraw) {
            layout = get_layout(
                player_state.window_w, player_state.window_h,
                frame->height, frame->width, TIMELINE_HEIGHT, PROGRESS_HEIGHT
            );
            draw_background(renderer, &colors);
            SDL_RenderCopy(renderer, video_tex, NULL, &layout.viewer_rect);
            draw_progress(
                renderer,
                layout.progress_rect, 
                player_state.cur_timestamp,
                vid_info.duration,
                &colors
            );
            draw_timeline(
                renderer,
                font,
                layout.timeline_rect,
                vid_info.start_time,
                player_state.cur_timestamp,
                vid_info.duration,
                &colors
            );
            SDL_RenderPresent(renderer);
        }
    
        while (REFRESH_TIME - (clock() - start_time) > 0);
    }
    quit:

    /* clean up */
    destroy_frame_converter(&frame_conv);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&frame);
    avformat_close_input(&format_ctx);
    avcodec_free_context(&vcodec_ctx);
    SDL_DestroyTexture(video_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

