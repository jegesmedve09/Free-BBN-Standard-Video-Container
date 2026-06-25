#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <SDL2/SDL.h>

// Master Control Opcodes
#define OP_START       0xF0
#define OP_FRAME_START 0xF1
#define OP_FRAME_END   0xF2
#define OP_AUDIO_START 0xF3
#define OP_AUDIO_END   0xF4
#define OP_END         0xFF

#define FIXED_AUDIO_SIZE 1102

// Helper macro to instantly decode a 2-byte BCD/Visual Hex value to an integer
#define BCD_TO_INT(b) ( \
    ((b[0] >> 4) * 1000) + \
    ((b[0] & 0x0F) * 100) + \
    ((b[1] >> 4) * 10) + \
    ((b[1] & 0x0F)) \
)

int main(int argc, char* argv[]) {
    const char* filename = "video.fsvc3";
    if (argc > 1) {
        filename = argv[1];
    }

    FILE* f = fopen(filename, "rb");
    if (!f) {
        printf("Error: Could not open file %s\n", filename);
        return 1;
    }

    // 1. Verify Magic ID
    uint8_t magic[5];
    fread(magic, 1, 5, f);
    if (memcmp(magic, "fsvc3", 5) != 0) {
        printf("Error: Magic ID mismatch!\n");
        fclose(f);
        return 1;
    }

    // 2. Decode Resolution (Visual Hex / BCD)
    uint8_t w_bytes[2];
    uint8_t h_bytes[2];
    fread(w_bytes, 1, 2, f);
    fread(h_bytes, 1, 2, f);
    
    int width = BCD_TO_INT(w_bytes);
    int height = BCD_TO_INT(h_bytes);

    // 3. Read Remaining Header Configurations
    uint32_t total_frames;
    uint8_t delay;
    fread(&total_frames, sizeof(uint32_t), 1, f);
    fread(&delay, sizeof(uint8_t), 1, f);

    // Skip padding (2 bytes) and metadata block (48 bytes) to land exactly at 0x40
    fseek(f, 2 + 48, SEEK_CUR);

    // 4. Verify Master Token Anchor at 0x40
    uint8_t start_token;
    fread(&start_token, 1, 1, f);
    if (start_token != OP_START) {
        printf("Error: Missing master start token (0xF0) at offset 0x40.\n");
        fclose(f);
        return 1;
    }

    printf("[FreeBBN Native Engine Initialized]\n");
    printf("Dimensions: %dx%d\nTarget Delay: %dms\n\n", width, height, delay);

    // 5. Initialize SDL2 Video and Audio Systems
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        printf("SDL Init Error: %s\n", SDL_GetError());
        fclose(f);
        return 1;
    }

    // --- CONFIGURE NATIVE UNSIGNED 8-BIT AUDIO DRIVER ---
    SDL_AudioSpec audio_spec;
    SDL_zero(audio_spec);
    audio_spec.freq = 22050;          // 22050Hz Sample Rate
    audio_spec.format = AUDIO_U8;     // Pure Unsigned 8-bit PCM (Perfect for PS2 SPU2 alignment)
    audio_spec.channels = 1;          // Mono
    audio_spec.samples = 1024;

    SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(NULL, 0, &audio_spec, NULL, 0);
    if (audio_device == 0) {
        printf("SDL Audio Device Error: %s\n", SDL_GetError());
        SDL_Quit();
        fclose(f);
        return 1;
    }
    SDL_PauseAudioDevice(audio_device, 0); // Activate the audio pipeline

    SDL_Window* window = SDL_CreateWindow("FreeBBN Standard Video Container v3 Player", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* texture = SDL_CreateTexture(renderer, 
        SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);

    uint32_t* pixel_buffer = (uint32_t*)calloc(width * height, sizeof(uint32_t));

    size_t max_frame_size = (width * height * 7) + 100; 
    uint8_t* chunk_buffer = (uint8_t*)malloc(max_frame_size);

    int current_frame = 0;
    int is_playing = 1;
    SDL_Event event;

    uint32_t start_playback_time = SDL_GetTicks();

    // 6. Main Interleaved State Machine Loop
while (is_playing) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT || 
               (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                is_playing = 0;
            }
        }

        uint8_t opcode;
        if (fread(&opcode, 1, 1, f) != 1) break;

        // --- STEP 1: CONVERTED LAYOUT ALWAYS STARTS WITH THE AUDIO CHUNK ---
        if (opcode == OP_AUDIO_START) {
            // Grab the exact 1102 bytes for this frame chunk
            fread(chunk_buffer, 1, FIXED_AUDIO_SIZE, f);
            
            // Queue it to the audio device hardware buffer immediately
            SDL_QueueAudio(audio_device, chunk_buffer, FIXED_AUDIO_SIZE);

            // Swallow the trailing 0xF4 audio end marker
            uint8_t audio_end;
            fread(&audio_end, 1, 1, f);

            // --- STEP 2: LOOK AHEAD IMMEDIATELY FOR THE TIED VIDEO FRAME ---
            uint8_t next_opcode;
            if (fread(&next_opcode, 1, 1, f) == 1 && next_opcode == OP_FRAME_START) {
                
                long current_pos = ftell(f);
                size_t bytes_read = fread(chunk_buffer, 1, max_frame_size, f);
                
                size_t p = 0;
                while (p < bytes_read) {
                    uint8_t marker = chunk_buffer[p++];

                    if (marker == OP_FRAME_END) {
                        fseek(f, current_pos + p, SEEK_SET);
                        break;
                    } else if (marker == OP_END) {
                        is_playing = 0;
                        fseek(f, current_pos + p, SEEK_SET);
                        break;
                    } else {
                        if (p - 1 + 7 > bytes_read) {
                            is_playing = 0;
                            break;
                        }

                        uint8_t* record = &chunk_buffer[p - 1]; 
                        p += 6; 

                        int x = BCD_TO_INT((&record[0]));
                        int y = BCD_TO_INT((&record[2]));
                        
                        uint32_t r = record[4] << 3;
                        uint32_t g = record[5] << 3;
                        uint32_t b = record[6] << 3;

                        if (x < width && y < height) {
                            pixel_buffer[y * width + x] = (r << 16) | (g << 8) | b;
                        }
                    }
                }

                // Push the decoded video pixels to the display
                SDL_UpdateTexture(texture, NULL, pixel_buffer, width * sizeof(uint32_t));
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);

                current_frame++;
                printf("Playing Frame: %d/%d\r", current_frame, total_frames);
                fflush(stdout);

                // --- STEP 3: LOCK THE TIED PAIR TO UNIFORM REAL-TIME TARGET ---
                uint32_t target_time = start_playback_time + (current_frame * delay);
                uint32_t current_time = SDL_GetTicks();

                if (current_time < target_time) {
                    SDL_Delay(target_time - current_time);
                }
            } else {
                // If there wasn't a frame following the audio chunk, rewind the look-ahead check
                fseek(f, -1, SEEK_CUR);
            }
        } 
        else if (opcode == OP_END) {
            break;
        }
    }

    printf("\nPlayback ended cleanly via termination token.\n");

    // Clean up
    free(chunk_buffer);
    free(pixel_buffer);
    SDL_CloseAudioDevice(audio_device);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    fclose(f);
    return 0;
}
