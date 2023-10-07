#include "av.h"
#include "decode.h"
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_pixels.h>
#include <SDL2/SDL_render.h>

SDL_Window * window = NULL;
SDL_Renderer * renderer = NULL;
SDL_Texture * video_tex = NULL;

Movie * movie;

static int save_frame(FILE * file, AVFrame * frame, int width, int height) {
    uint32_t bytes_written = 0;
    bytes_written += fprintf(file, "P6\n%d %d\n255\n", width, height);
    for(int y = 0; y < height; y++)
        bytes_written += fwrite(frame->data[0]+y*frame->linesize[0], 1, width*3, file);
    return bytes_written;
}

void cleanup_sdl(void) {
    SDL_DestroyTexture(video_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

int init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO))
        return -1;

    SDL_CreateWindowAndRenderer(
        640, 480, 0,
        &window, &renderer
    );
    if ((window == NULL) || (renderer == NULL))
        return -1;

    video_tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        640, 480
    );

    if (video_tex == NULL)
        return -1;

	SDL_SetWindowTitle(window, "av");
    SDL_SetRenderDrawColor(renderer, 0x0, 0x0, 0x0, 0xff);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    return 0;
}

int main(int argc, char * argv[]) {

    if (argc < 2) {
        fprintf(stderr, "provide filename\n");
    return -1;
    }
    char * filename = argv[1];

    if (open_movie(filename, &movie)) {
        fprintf(stderr, "error decoding file");
        return -1; 
    }

    if (init_sdl()) {
        fprintf(stderr, "error drawing to screen");
        return -1;
    };


    uint8_t * pixels = NULL;
    int pitch;
    SDL_LockTexture(video_tex, NULL, (void **) &pixels, &pitch);

    get_frame(movie, pixels, pitch);

    SDL_UnlockTexture(video_tex);


    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, video_tex, NULL, NULL);
    SDL_RenderPresent(renderer);

    sleep(1);

    close_movie(movie);

    //cleanup_sdl();


    return 0;
}
