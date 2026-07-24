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

#define BCD_TO_INT(b) ( \
    ((b[0] >> 4) * 1000) + \
    ((b[0] & 0x0F) * 100) + \
    ((b[1] >> 4) * 10) + \
    ((b[1] & 0x0F)) \
)

int main(int argc, char* argv[]) {
    const char* filename = "video.fsvc5";
    if (argc > 1) filename = argv[1];

    FILE* f = fopen(filename, "rb");
    if (!f) {
        printf("Error: Could not open file %s\n", filename);
        return 1;
    }

    // 1. Verify FSVC5 Magic ID
    uint8_t magic[5];
    fread(magic, 1, 5, f);
    if (memcmp(magic, "fsvc5", 5) != 0) {
        printf("Error: Magic ID mismatch!\n");
        fclose(f);
        return 1;
    }

    // 2. Decode Resolution
    uint8_t w_bytes[2]; uint8_t h_bytes[2];
    fread(w_bytes, 1, 2, f); fread(h_bytes, 1, 2, f);
    int width = BCD_TO_INT(w_bytes);
    int height = BCD_TO_INT(h_bytes);

    // 3. Read Metadata headers
    uint32_t total_frames; uint8_t delay;
    fread(&total_frames, sizeof(uint32_t), 1, f);
    fread(&delay, sizeof(uint8_t), 1, f);

    uint8_t audio_config = fgetc(f);
    uint8_t layout_byte = fgetc(f);

    int channels = (audio_config >> 4) & 0x0F;
    uint8_t sr_index = audio_config & 0x0F;
    int sample_rate = 44100;
    if (sr_index == 1)      sample_rate = 8000;
    else if (sr_index == 6) sample_rate = 44100;
    else if (sr_index == 15) sample_rate = 48000;

    size_t audio_chunk_size = (sample_rate / 20) * channels;

    // Skip the 16 bytes of metadata 1 to reach the thumbnail offset precisely at 0x20
    fseek(f, 16, SEEK_CUR);

    // --- GRAB THE THUMBNAIL FROM THE FILE ---
    uint8_t raw_thumb_data[4096];
    fread(raw_thumb_data, 1, 4096, f);

    // Hard seek down to the stream payload anchor
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
        height = 400; // A nice square window to frame the art nicely
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

    SDL_Window* window = SDL_CreateWindow("FreeBBN FSVC5 Player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    
    // Video Track Texture
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
    uint32_t* pixel_buffer = (uint32_t*)calloc(width * height, sizeof(uint32_t));

    // --- CREATE THE STATIC THUMBNAIL TEXTURE ---
    SDL_Texture* thumb_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STATIC, 64, 64);
    uint32_t thumb_pixels[4096];
    for(int i = 0; i < 4096; i++) {
        // Simple mock color generation based on the index value.
        // For a real app, you'd match this against an official 256-color look-up table palette.
        uint8_t intensity = raw_thumb_data[i]; 
        thumb_pixels[i] = (intensity << 16) | (intensity << 8) | intensity; // Grayscale mapping
    }
    SDL_UpdateTexture(thumb_texture, NULL, thumb_pixels, 64 * sizeof(uint32_t));

    // Define a centered 256x256 display target rectangle for our 64x64 art chunk
    SDL_Rect thumb_rect = { (width - 256) / 2, (height - 256) / 2, 256, 256 };

    size_t max_frame_size = (width * height * 7) + audio_chunk_size + 256; 
    uint8_t* chunk_buffer = (uint8_t*)malloc(max_frame_size);
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

            // --- AUDIO-ONLY RENDERING PATH (Art + Waveform Overlay) ---
            if (layout_byte == 0xF0) {
                // 1. Clear the canvas to a dark background
                SDL_SetRenderDrawColor(renderer, 0x1A, 0x1A, 0x1A, 0xFF);
                SDL_RenderClear(renderer);
                
                // 2. Draw the embedded thumbnail art centered on screen
                SDL_RenderCopy(renderer, thumb_texture, NULL, &thumb_rect);
                
                // 3. Render the oscilloscope wave over it
                // We draw it as a series of connected points or vertical green lines
                SDL_SetRenderDrawColor(renderer, 0x00, 0xFF, 0x00, 0xFF); // Bright green wave
                
                int prev_x = 0;
                int prev_y = height / 2; // Default starting point at vertical center
                
                for (int x = 0; x < width && x < (int)audio_chunk_size; x++) {
                    // Extract the raw PCM sample (-128 to shift unsigned 0-255 into signed space)
                    int val = chunk_buffer[x] - 128; 
                    
                    // Map the wave's vertical position. 
                    // Lowering the vertical amplitude range slightly (e.g., height / 4) 
                    // can prevent the wave from completely drowning out your thumbnail image.
                    int y = (height / 2) + (val * (height / 4) / 128);
                    
                    if (y >= 0 && y < height) {
                        // Option A: Draw a solid line connecting the previous point to this one
                        if (x > 0) {
                            SDL_RenderDrawLine(renderer, prev_x, prev_y, x, y);
                        }
                        // Option B: If you prefer the old single-pixel style, uncomment the next line:
                        // SDL_RenderDrawPoint(renderer, x, y);
                    }
                    
                    prev_x = x;
                    prev_y = y;
                }
                
                // 4. Present the composited frame to the display
                SDL_RenderPresent(renderer);
                
                // Keep audio and visualizer clocks synced precisely
                uint32_t queued_bytes = SDL_GetQueuedAudioSize(audio_device);
                uint32_t audio_ms_left = (queued_bytes / channels) * 1000 / sample_rate;
                if (audio_ms_left > delay) {
                    SDL_Delay(audio_ms_left - delay);
                }
                continue;
            }
            if (fread(&opcode, 1, 1, f) != 1) break;
        }

        if (opcode == OP_FRAME_START) {
            // (Keep standard video delta vector decoding here for video files...)
            long current_pos = ftell(f);
            size_t bytes_read = fread(chunk_buffer, 1, max_frame_size, f);
            size_t p = 0;
            while (p < bytes_read) {
                uint8_t marker = chunk_buffer[p++];
                if (marker == OP_FRAME_END || marker == OP_END) {
                    fseek(f, current_pos + p, SEEK_SET); break;
                } else {
                    p += 6;
                    int x = BCD_TO_INT((&chunk_buffer[p - 7]));
                    int y = BCD_TO_INT((&chunk_buffer[p - 5]));
                    uint32_t r = chunk_buffer[p - 3] << 3;
                    uint32_t g = chunk_buffer[p - 2] << 3;
                    uint32_t b = chunk_buffer[p - 1] << 3;
                    if (x < width && y < height) pixel_buffer[y * width + x] = (r << 16) | (g << 8) | b;
                }
            }
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
