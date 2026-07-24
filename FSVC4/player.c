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

// Helper macro to instantly decode a 2-byte BCD/Visual Hex value to an integer
#define BCD_TO_INT(b) ( \
    ((b[0] >> 4) * 1000) + \
    ((b[0] & 0x0F) * 100) + \
    ((b[1] >> 4) * 10) + \
    ((b[1] & 0x0F)) \
)

int main(int argc, char* argv[]) {
    const char* filename = "video.fsvc4";
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
    if (memcmp(magic, "fsvc4", 5) != 0) {
        printf("Error: Magic ID mismatch! Target system needs valid fsvc4 data files.\n");
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

    // DYNAMIC PARSING: Extract configuration flags at 0x0E and 0x0F
    uint8_t audio_config = fgetc(f);
    uint8_t layout_byte = fgetc(f);

    int channels = (audio_config >> 4) & 0x0F;
    uint8_t sr_index = audio_config & 0x0F;
    int sample_rate = 44100;
    
    if (sr_index == 1)      sample_rate = 8000;
    else if (sr_index == 2) sample_rate = 11025;
    else if (sr_index == 3) sample_rate = 16000;
    else if (sr_index == 4) sample_rate = 22050;
    else if (sr_index == 5) sample_rate = 32000;
    else if (sr_index == 6) sample_rate = 44100;
    else if (sr_index == 15) sample_rate = 48000; // 0xF mapped to 48kHz

    // Calculate exact dynamic buffer tracking size per 50ms block
    size_t audio_chunk_size = (sample_rate / 20) * channels;

    // Skip the 48-byte text description to land exactly at 0x40
    fseek(f, 48, SEEK_CUR);

    // 4. Verify Master Token Anchor at 0x40
    uint8_t start_token = fgetc(f);
    if (start_token != OP_START) {
        printf("Error: Broken pipeline structural alignment anchor at 0x40.\n");
        fclose(f);
        return 1;
    }

    printf("[FreeBBN Sonic Decoder v4 Core Active]\n");
    printf("Resolution: %dx%d | Layout: %s\n", width, height, 
           (layout_byte == 0xF0) ? "AUDIO ONLY" : (layout_byte == 0x0F) ? "VIDEO ONLY" : "INTERLEAVED AV");
    printf("Audio Engine Pipeline: %d Channels @ %dHz (%zu bytes/block)\n\n", channels, sample_rate, audio_chunk_size);

    // 5. Initialize SDL2 Video and Audio Systems
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        printf("SDL Init Error: %s\n", SDL_GetError());
        fclose(f);
        return 1;
    }

    // Configure the dynamic multitrack audio system
    SDL_AudioDeviceID audio_device = 0;
    if (layout_byte == 0xFF || layout_byte == 0xF0) {
        SDL_AudioSpec audio_spec;
        SDL_zero(audio_spec);
        audio_spec.freq = sample_rate;
        audio_spec.format = AUDIO_U8; 
        audio_spec.channels = channels; 
        audio_spec.samples = 1024;

        audio_device = SDL_OpenAudioDevice(NULL, 0, &audio_spec, NULL, 0);
        if (audio_device == 0) {
            printf("SDL Audio Device Error: %s\n", SDL_GetError());
            SDL_Quit();
            fclose(f);
            return 1;
        }
        SDL_PauseAudioDevice(audio_device, 0); // Activate the audio pipeline
    }

    // Manage dimensions for audio-only tracking layout structures
    if (layout_byte == 0xF0) { 
        width = 640; 
        height = 180; 
    }

    SDL_Window* window = SDL_CreateWindow("FreeBBN Sonic Video Container v4 Player", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* texture = SDL_CreateTexture(renderer, 
        SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);

    uint32_t* pixel_buffer = (uint32_t*)calloc(width * height, sizeof(uint32_t));
    size_t max_frame_size = (width * height * 7) + audio_chunk_size + 256; 
    uint8_t* chunk_buffer = (uint8_t*)malloc(max_frame_size);

    int current_frame = 0;
    int is_playing = 1;
    SDL_Event event;

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

        // --- STEP 1: PARSE AND QUEUE AUDIO CHUNKS ---
        if (opcode == OP_AUDIO_START) {
            fread(chunk_buffer, 1, audio_chunk_size, f);
            if (audio_device) {
                SDL_QueueAudio(audio_device, chunk_buffer, audio_chunk_size);
            }
            uint8_t audio_end;
            fread(&audio_end, 1, 1, f);

            // Audio-only interactive visualizer engine link
            if (layout_byte == 0xF0) {
                memset(pixel_buffer, 0, width * height * sizeof(uint32_t));
                for (int x = 0; x < width && x < (int)audio_chunk_size; x++) {
                    int val = chunk_buffer[x] - 128; 
                    int y = (height / 2) + (val * height / 256);
                    if (y >= 0 && y < height) pixel_buffer[y * width + x] = 0x00FF00;
                }
                SDL_UpdateTexture(texture, NULL, pixel_buffer, width * sizeof(uint32_t));
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
                
                current_frame++;
                
                // Track visualizer timing via remaining audio bytes
                uint32_t queued_bytes = SDL_GetQueuedAudioSize(audio_device);
                uint32_t audio_ms_left = (queued_bytes / channels) * 1000 / sample_rate;
                if (audio_ms_left > delay) {
                    SDL_Delay(audio_ms_left - delay);
                }
                continue;
            }
            
            // Advance forward to handle matching interleaved video target vectors
            if (fread(&opcode, 1, 1, f) != 1) break;
        }

        // --- STEP 2: PARSE DELTA VIDEO PACKETS ---
        if (opcode == OP_FRAME_START) {
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
            printf("Playing Block Vector: %d/%d\r", current_frame, total_frames);
            fflush(stdout);

            // --- STEP 3: THE SMOOTH AUDIO-BUFFERED SYNC ENGINE ---
            if (audio_device) {
                // Ask the audio hardware how much sound is waiting to be played
                uint32_t queued_bytes = SDL_GetQueuedAudioSize(audio_device);
                
                // Convert those bytes back into milliseconds of execution time left
                uint32_t audio_ms_left = (queued_bytes / channels) * 1000 / sample_rate;

                // If we have more than the frame target delay (50ms) queued, the CPU is running too fast.
                // We sleep exactly long enough to keep video locked behind the audio hardware clock.
                if (audio_ms_left > delay) {
                    SDL_Delay(audio_ms_left - delay);
                }
                // If audio_ms_left is LESS than 50ms, it means a heavy frame (like 1600x1200) 
                // slowed down the CPU. We bypass the delay entirely so the engine can immediately 
                // spin to the next frame and catch up without stuttering the audio.
            } else {
                // Fallback for video-only mode
                SDL_Delay(delay);
            }
        } 
        else if (opcode == OP_END) {
            break;
        }
    }

    printf("\nPlayback ended cleanly via termination token.\n");

    // Clean up resources safely
    free(chunk_buffer);
    free(pixel_buffer);
    if (audio_device) {
        SDL_CloseAudioDevice(audio_device);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    fclose(f);
    return 0;
}
