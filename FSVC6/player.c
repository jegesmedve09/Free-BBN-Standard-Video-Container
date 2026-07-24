#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <SDL2/SDL.h>

#define OP_START       0xF0
#define OP_FRAME_START 0xF1
#define OP_FRAME_END   0xF2
#define OP_AUDIO_START 0xF3
#define OP_AUDIO_END   0xF4
#define OP_END         0xFF

// --- MATCH ENCODER STRUCT ---
#pragma pack(push, 1)
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t color; // RGB565
} DeltaPixel;
#pragma pack(pop)

// --- Helper: Convert RGB565 back to RGB888 for SDL ---
static inline void rgb565_to_rgb888(uint16_t color, uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = (color >> 11) << 3;
    *g = ((color >> 5) & 0x3F) << 2;
    *b = (color & 0x1F) << 3;
}

int main(int argc, char* argv[]) {
    const char* filename = "output.fsvc5";
    if (argc > 1) filename = argv[1];

    FILE* f = fopen(filename, "rb");
    if (!f) {
        printf("Error: Could not open file %s\n", filename);
        return 1;
    }

    // 1. Verify FSVC6 Magic ID
    uint8_t magic[5];
    fread(magic, 1, 5, f);
    if (memcmp(magic, "fsvc6", 5) != 0) {
        printf("Error: Magic ID mismatch! This is not a valid FSVC6 file.\n");
        fclose(f);
        return 1;
    }

    // 2. Decode Resolution (Direct uint16_t, no BCD string manipulation needed)
    uint16_t width = 0; 
    uint16_t height = 0;
    fread(&width, sizeof(uint16_t), 1, f); 
    fread(&height, sizeof(uint16_t), 1, f);

    // 3. Read Core Headers
    uint32_t total_frames; 
    uint8_t delay;
    fread(&total_frames, sizeof(uint32_t), 1, f);
    fread(&delay, sizeof(uint8_t), 1, f);

    uint8_t audio_config = fgetc(f);
    uint8_t layout_byte = fgetc(f);

    // 4. Read Dynamic FPS mapped at offset 0x10
    uint8_t target_fps = fgetc(f);
    if (target_fps == 0) target_fps = 20; // Fallback failsafe

    // Skip the remaining 15 bytes of meta_1 string
    fseek(f, 15, SEEK_CUR);

    int channels = (audio_config >> 4) & 0x0F;
    uint8_t sr_index = audio_config & 0x0F;
    int sample_rate = 44100;
    if (sr_index == 1)      sample_rate = 8000;
    else if (sr_index == 6) sample_rate = 44100;
    else if (sr_index == 15) sample_rate = 48000;

    // Use dynamic FPS for audio chunk sizing instead of hardcoded 20
    size_t audio_chunk_size = (sample_rate / target_fps) * channels;

    printf("Playing %dx%d @ %d FPS | %dHz Audio\n", width, height, target_fps, sample_rate);

    // --- GRAB THE THUMBNAIL FROM 0x20 ---
    uint8_t raw_thumb_data[4096];
    fread(raw_thumb_data, 1, 4096, f);

    // Hard seek down to the stream payload anchor at 0x1100 (Bypassing meta_2)
    fseek(f, 0x1100, SEEK_SET);
    uint8_t start_token = fgetc(f);
    if (start_token != OP_START) {
        printf("Error: Broken structural alignment anchor at 0x1100.\n");
        fclose(f);
        return 1;
    }

    // Window layout management for Audio-Only mode
    if (layout_byte == 0xF0) { 
        width = 400; 
        height = 400; 
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) return 1;

    SDL_AudioDeviceID audio_device = 0;
    if (layout_byte == 0xFF || layout_byte == 0xF0) {
        SDL_AudioSpec audio_spec; SDL_zero(audio_spec);
        audio_spec.freq = sample_rate; audio_spec.format = AUDIO_U8; 
        audio_spec.channels = channels; audio_spec.samples = 1024;
        audio_device = SDL_OpenAudioDevice(NULL, 0, &audio_spec, NULL, 0);
        if (audio_device) SDL_PauseAudioDevice(audio_device, 0); 
    }

    SDL_Window* window = SDL_CreateWindow("FSVC6 Player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
    uint32_t* pixel_buffer = (uint32_t*)calloc(width * height, sizeof(uint32_t));

    // --- CREATE THE STATIC THUMBNAIL TEXTURE ---
    SDL_Texture* thumb_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STATIC, 64, 64);
    uint32_t thumb_pixels[4096];
    for(int i = 0; i < 4096; i++) {
        uint8_t intensity = raw_thumb_data[i]; 
        thumb_pixels[i] = (intensity << 16) | (intensity << 8) | intensity; 
    }
    SDL_UpdateTexture(thumb_texture, NULL, thumb_pixels, 64 * sizeof(uint32_t));

    SDL_Rect thumb_rect = { (width - 256) / 2, (height - 256) / 2, 256, 256 };

    uint8_t* chunk_buffer = (uint8_t*)malloc(audio_chunk_size);
    int is_playing = 1; SDL_Event event;

    while (is_playing) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) is_playing = 0;
        }

        uint8_t opcode;
        if (fread(&opcode, 1, 1, f) != 1) break;

        if (opcode == OP_AUDIO_START) {
            fread(chunk_buffer, 1, audio_chunk_size, f);
            if (audio_device) SDL_QueueAudio(audio_device, chunk_buffer, audio_chunk_size);
            uint8_t audio_end; fread(&audio_end, 1, 1, f);

            // --- AUDIO-ONLY RENDERING PATH ---
            if (layout_byte == 0xF0) {
                SDL_SetRenderDrawColor(renderer, 0x1A, 0x1A, 0x1A, 0xFF);
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, thumb_texture, NULL, &thumb_rect);
                SDL_SetRenderDrawColor(renderer, 0x00, 0xFF, 0x00, 0xFF); 
                
                int prev_x = 0;
                int prev_y = height / 2; 
                
                for (int x = 0; x < width && x < (int)audio_chunk_size; x++) {
                    int val = chunk_buffer[x] - 128; 
                    int y = (height / 2) + (val * (height / 4) / 128);
                    
                    if (y >= 0 && y < height) {
                        if (x > 0) SDL_RenderDrawLine(renderer, prev_x, prev_y, x, y);
                    }
                    prev_x = x; prev_y = y;
                }
                
                SDL_RenderPresent(renderer);
                
                uint32_t queued_bytes = SDL_GetQueuedAudioSize(audio_device);
                uint32_t audio_ms_left = (queued_bytes / channels) * 1000 / sample_rate;
                if (audio_ms_left > delay) SDL_Delay(audio_ms_left - delay);
                continue;
            }
            if (fread(&opcode, 1, 1, f) != 1) break;
        }

        if (opcode == OP_FRAME_START) {
            uint32_t delta_count;
            if (fread(&delta_count, sizeof(uint32_t), 1, f) != 1) break;

            if (delta_count > 0) {
                // Bulk read the DeltaPixels exactly like they were written
                DeltaPixel* deltas = (DeltaPixel*)malloc(delta_count * sizeof(DeltaPixel));
                fread(deltas, sizeof(DeltaPixel), delta_count, f);

                for (uint32_t i = 0; i < delta_count; i++) {
                    uint16_t x = deltas[i].x;
                    uint16_t y = deltas[i].y;
                    
                    if (x < width && y < height) {
                        uint8_t r, g, b;
                        rgb565_to_rgb888(deltas[i].color, &r, &g, &b);
                        pixel_buffer[y * width + x] = (r << 16) | (g << 8) | b;
                    }
                }
                free(deltas);
            }

            uint8_t frame_end; 
            fread(&frame_end, 1, 1, f); // Clear OP_FRAME_END token

            SDL_UpdateTexture(texture, NULL, pixel_buffer, width * sizeof(uint32_t));
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);

            if (audio_device) {
                uint32_t queued_bytes = SDL_GetQueuedAudioSize(audio_device);
                uint32_t audio_ms_left = (queued_bytes / channels) * 1000 / sample_rate;
                if (audio_ms_left > delay) SDL_Delay(audio_ms_left - delay);
            } else {
                SDL_Delay(delay);
            }
        } else if (opcode == OP_END) break;
    }

    free(chunk_buffer); free(pixel_buffer);
    if (audio_device) SDL_CloseAudioDevice(audio_device);
    SDL_DestroyTexture(thumb_texture); SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    SDL_Quit(); fclose(f);
    
    return 0;
}
