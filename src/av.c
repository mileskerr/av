#include "av.h"
#include "decode.h"
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_pixels.h>
#include <SDL2/SDL_render.h>

SDL_Window * window;
SDL_Renderer * renderer;
SDL_Texture * video_tex;


static int save_frame(FILE * file, AVFrame * frame, int width, int height) {
    uint32_t bytes_written = 0;
    bytes_written += fprintf(file, "P6\n%d %d\n255\n", width, height);
    for(int y = 0; y < height; y++)
        bytes_written += fwrite(frame->data[0]+y*frame->linesize[0], 1, width*3, file);
    return bytes_written;
}

int init_sdl(void) {
    //TODO: handle errors
    SDL_Init(SDL_INIT_VIDEO);
    SDL_CreateWindowAndRenderer(
        640, 480, 0,
        &window, &renderer
    );
    video_tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        640, 480
    );
	SDL_SetWindowTitle(window, "av");
    SDL_SetRenderDrawColor(renderer, 0x0, 0x0, 0x0, 0xff);
    SDL_RenderClear(renderer);

    return 0;
}

void gui_get_video_size(int * w, int * h) {
    SDL_QueryTexture(
        video_tex, NULL, NULL,
        w, h
    );    
}

int gui_set_video_frame(void * pixels) {
    int w;
    gui_get_video_size(&w, NULL);
    //TODO: handle errors
    SDL_UpdateTexture(
        video_tex, NULL, pixels,
        w
    );
    return 0;
}

void gui_present(void) {
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, video_tex, NULL, NULL);
    SDL_RenderPresent(renderer);
}

int main(int argc, char * argv[]) {

    if (argc < 2) {
        fprintf(stderr, "provide filename\n");
    return -1;
    }
    char * filename = argv[1];

    TRY(init_dec(filename));

    init_sdl();

    uint8_t * pixels;
    int pitch, w, h;
    SDL_QueryTexture(video_tex, NULL, NULL, &w, &h);
    SDL_LockTexture(video_tex, NULL, (void **) &pixels, &pitch);

    dec_get_pixels(pixels, w, h, pitch);

    SDL_UnlockTexture(video_tex);


    gui_present();

    sleep(10);    

    return 0;
}
