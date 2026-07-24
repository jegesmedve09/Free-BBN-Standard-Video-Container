#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// Master Control Opcodes
#define OP_START        0xF0
#define OP_FRAME_START  0xF1
#define OP_FRAME_END    0xF2
#define OP_AUDIO_START  0xF3
#define OP_AUDIO_END    0xF4
#define OP_END          0xFF

#define TARGET_FPS       20
#define TARGET_DELAY     50

static inline void int_to_visual_hex_bytes(int val, uint8_t* dest) {
    if (val < 0) val = 0;
    if (val > 9999) val = 9999;
    char s[16];
    snprintf(s, sizeof(s), "%04d", val);
    dest[0] = ((s[0] - '0') << 4) | (s[1] - '0');
    dest[1] = ((s[2] - '0') << 4) | (s[3] - '0');
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <input_file> <output.fsvc5> [width] [height] [channels] [samplerate] [layout]\n", argv[0]);
        return 1;
    }

    const char* input_filename = argv[1];
    const char* output_filename = argv[2];
    int width = (argc > 3) ? atoi(argv[3]) : 1920;
    int height = (argc > 4) ? atoi(argv[4]) : 1080;
    int channels = (argc > 5) ? atoi(argv[5]) : 2;      
    int sample_rate = (argc > 6) ? atoi(argv[6]) : 44100; 
    const char* layout_str = (argc > 7) ? argv[7] : "both";

    if (channels < 1 || channels > 15) channels = 2;

    uint8_t layout_byte = 0xFF; 
    if (strcmp(layout_str, "audio") == 0) layout_byte = 0xF0;
    if (strcmp(layout_str, "video") == 0) layout_byte = 0x0F;

    uint8_t sr_nibble = 0x0;
    if (sample_rate == 8000)       sr_nibble = 0x1;
    else if (sample_rate == 44100) sr_nibble = 0x6;
    else if (sample_rate == 48000) sr_nibble = 0xF;
    else { sample_rate = 44100; sr_nibble = 0x6; }

    uint8_t audio_config_byte = (uint8_t)((channels << 4) | sr_nibble);
    size_t audio_chunk_size = (sample_rate / TARGET_FPS) * channels;

    FILE* out = fopen(output_filename, "wb");
    if (!out) {
        printf("Error: Could not open output file %s\n", output_filename);
        return 1;
    }

    // --- SMART THUMBNAIL HIERARCHY PIPELINE ---
    uint8_t final_thumb[4096];
    memset(final_thumb, 0x3A, 4096); // Level 3 Safety Net: flat gray canvas
    
    char thumb_cmd[512] = {0};
    int extracted_successfully = 0;

    if (layout_byte == 0xFF || layout_byte == 0x0F) {
        // ====================================================================
        // VIDEO / BOTH MODE: Always prioritize video stream frames
        // ====================================================================
        // Try extracting from 5 seconds deep to avoid black frames
        snprintf(thumb_cmd, sizeof(thumb_cmd),
                 "ffmpeg -loglevel quiet -i \"%s\" -ss 00:00:05 -vframes 1 -vf \"scale=64:64\" -f rawvideo -pix_fmt gray -", 
                 input_filename);
        
        FILE* pipe_thumb = popen(thumb_cmd, "r");
        if (pipe_thumb) {
            size_t bytes_read = fread(final_thumb, 1, 4096, pipe_thumb);
            pclose(pipe_thumb);
            if (bytes_read == 4096) extracted_successfully = 1;
        }

        // Retry from absolute beginning (00:00:00) if video is under 5 seconds long
        if (!extracted_successfully) {
            snprintf(thumb_cmd, sizeof(thumb_cmd),
                     "ffmpeg -loglevel quiet -i \"%s\" -vframes 1 -vf \"scale=64:64\" -f rawvideo -pix_fmt gray -", 
                     input_filename);
            pipe_thumb = popen(thumb_cmd, "r");
            if (pipe_thumb) {
                size_t bytes_read = fread(final_thumb, 1, 4096, pipe_thumb);
                pclose(pipe_thumb);
                if (bytes_read == 4096) extracted_successfully = 1;
            }
        }
    } 
    else if (layout_byte == 0xF0) {
        // ====================================================================
        // AUDIO ONLY MODE: Extract from video containers (MP4) vs native Art (MP3)
        // ====================================================================
        // Priority 1: Try treating it like a video source first and skip 5 seconds in
        snprintf(thumb_cmd, sizeof(thumb_cmd),
                 "ffmpeg -loglevel quiet -i \"%s\" -an -ss 00:00:05 -vframes 1 -vf \"scale=64:64\" -f rawvideo -pix_fmt gray -", 
                 input_filename);
        
        FILE* pipe_thumb = popen(thumb_cmd, "r");
        if (pipe_thumb) {
            size_t bytes_read = fread(final_thumb, 1, 4096, pipe_thumb);
            pclose(pipe_thumb);
            if (bytes_read == 4096) {
                extracted_successfully = 1;
                printf("[FSVC5 Encoder] Extracted thumbnail from video timeline (skipped intro extremes).\n");
            }
        }

        // Priority 2: Try pulling raw embedded album artwork from frame 0 (MP3 metadata)
        if (!extracted_successfully) {
            snprintf(thumb_cmd, sizeof(thumb_cmd),
                     "ffmpeg -loglevel quiet -i \"%s\" -an -vframes 1 -vf \"scale=64:64\" -f rawvideo -pix_fmt gray -", 
                     input_filename);
            
            pipe_thumb = popen(thumb_cmd, "r");
            if (pipe_thumb) {
                size_t bytes_read = fread(final_thumb, 1, 4096, pipe_thumb);
                pclose(pipe_thumb);
                if (bytes_read == 4096) {
                    extracted_successfully = 1;
                    printf("[FSVC5 Encoder] Successfully extracted embedded audio album art.\n");
                }
            }
        }

        // FALLBACK: Audio has no embedded artwork. Check disk for your custom asset!
        if (!extracted_successfully) {
            FILE* raw_check = fopen("thumbnail.raw", "rb");
            if (!raw_check) raw_check = fopen("thumbnail.data", "rb");
            
            if (raw_check) {
                size_t bytes_read = fread(final_thumb, 1, 4096, raw_check);
                fclose(raw_check);
                if (bytes_read == 4096) {
                    extracted_successfully = 1;
                    printf("[FSVC5 Encoder] Audio has no embedded art. Injected fallback asset instead.\n");
                } else {
                    printf("[FSVC5 Encoder] Warning: Custom RAW file corrupted (%zu bytes). Skipping...\n", bytes_read);
                }
            }
        }
    }

    if (!extracted_successfully) {
        printf("[FSVC5 Encoder] Notice: No stream graphic found. Initialized flat retro gray canvas.\n");
    }

    // --- MAIN STREAM PIPES ---
    char ffmpeg_video_cmd[512] = {0};
    char ffmpeg_audio_cmd[512] = {0};
    FILE* pipe_video = NULL;
    FILE* pipe_audio = NULL;

    if (layout_byte == 0xFF || layout_byte == 0x0F) {
        snprintf(ffmpeg_video_cmd, sizeof(ffmpeg_video_cmd),
                 "ffmpeg -loglevel quiet -i \"%s\" -vf \"fps=%d,scale=%d:%d\" -f rawvideo -pix_fmt rgb24 -",
                 input_filename, TARGET_FPS, width, height);
        pipe_video = popen(ffmpeg_video_cmd, "r");
    }
    
    if (layout_byte == 0xFF || layout_byte == 0xF0) {
        snprintf(ffmpeg_audio_cmd, sizeof(ffmpeg_audio_cmd),
                 "ffmpeg -loglevel quiet -i \"%s\" -f u8 -acodec pcm_u8 -ac %d -ar %d -",
                 input_filename, channels, sample_rate);
        pipe_audio = popen(ffmpeg_audio_cmd, "r");
    }

    printf("[FreeBBN Sonic Video Core v5 Initialized]\n");
    printf("Config: %dx%d @ %d FPS | Audio: %d Channels at %dHz\n\n", width, height, TARGET_FPS, channels, sample_rate);

    size_t frame_pixel_count = width * height;
    uint8_t* current_rgb = (uint8_t*)malloc(frame_pixel_count * 3);
    uint8_t* prev_rgb_safe = (uint8_t*)calloc(frame_pixel_count * 3, sizeof(uint8_t));
    uint8_t* audio_buffer = (uint8_t*)malloc(audio_chunk_size);

    uint8_t w_hex[2], h_hex[2];
    int_to_visual_hex_bytes(width, w_hex);
    int_to_visual_hex_bytes(height, h_hex);
    uint8_t delay_byte = (uint8_t)TARGET_DELAY;

    // --- WRITE COMPLIANT HEADER BLOCK ---
    fwrite("fsvc5", 1, 5, out);               // 0x00 - 0x04
    fwrite(w_hex, 1, 2, out);                 // 0x05 - 0x06
    fwrite(h_hex, 1, 2, out);                 // 0x07 - 0x08
    
    long total_frames_offset = ftell(out);    // 0x09
    uint32_t encoded_count = 0;
    fwrite(&encoded_count, sizeof(uint32_t), 1, out); 
    
    fwrite(&delay_byte, sizeof(uint8_t), 1, out); // 0x0D
    fwrite(&audio_config_byte, 1, 1, out);     // 0x0E 
    fwrite(&layout_byte, 1, 1, out);           // 0x0F

    // 0x10 - 0x1F (Metadata space 1)
    char meta_1[16] = "FSVC5_AUTO_THMB";
    fwrite(meta_1, 1, 16, out);

    // 0x20 - 0x101F (4096 bytes 64x64 thumbnail block)
    fwrite(final_thumb, 1, 4096, out);

    // 0x1020 - 0x10FF (Metadata space 2)
    char meta_2[224] = {0};
    strncpy(meta_2, "Title: Asset Build; Core: FreeBBN Engine v5;", sizeof(meta_2) - 1);
    fwrite(meta_2, 1, 224, out);

    // 0x1100 Master Anchor Target
    uint8_t start_token = OP_START;           
    fwrite(&start_token, 1, 1, out);

    int is_first_frame = 1;
    uint8_t coord_buffer[2];

    while (1) {
        int has_video_stream = (layout_byte == 0xFF || layout_byte == 0x0F);
        int has_audio_stream = (layout_byte == 0xFF || layout_byte == 0x0F || layout_byte == 0xF0);

        if (has_video_stream) {
            size_t vr = fread(current_rgb, 1, frame_pixel_count * 3, pipe_video);
            if (vr < frame_pixel_count * 3) break; 
        }

        if (has_audio_stream && pipe_audio) {
            size_t ar = fread(audio_buffer, 1, audio_chunk_size, pipe_audio);
            if (ar < audio_chunk_size) {
                if (layout_byte == 0xF0 && ar == 0) break; 
                memset(audio_buffer + ar, 0x80, audio_chunk_size - ar);
            }
            for (size_t i = 0; i < audio_chunk_size; i++) {
                if (audio_buffer[i] > 0xEF) audio_buffer[i] = 0xEF;
            }
            fputc(OP_AUDIO_START, out);
            fwrite(audio_buffer, 1, audio_chunk_size, out);
            fputc(OP_AUDIO_END, out);
        }

        if (has_video_stream) {
            fputc(OP_FRAME_START, out);
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    size_t idx = (y * width + x) * 3;
                    uint8_t r_safe = (current_rgb[idx]     >> 3) & 0x1F;
                    uint8_t g_safe = (current_rgb[idx + 1] >> 3) & 0x1F;
                    uint8_t b_safe = (current_rgb[idx + 2] >> 3) & 0x1F;

                    // Clamping protection guardrail: Never step into opcode structural triggers (>=0xF0)
                    if (r_safe > 0x1E) r_safe = 0x1E;
                    if (g_safe > 0x1E) g_safe = 0x1E;
                    if (b_safe > 0x1E) b_safe = 0x1E;

                    if (is_first_frame || r_safe != prev_rgb_safe[idx] || g_safe != prev_rgb_safe[idx + 1] || b_safe != prev_rgb_safe[idx + 2]) {
                        int_to_visual_hex_bytes(x, coord_buffer); fwrite(coord_buffer, 1, 2, out);
                        int_to_visual_hex_bytes(y, coord_buffer); fwrite(coord_buffer, 1, 2, out);
                        fputc(r_safe, out); fputc(g_safe, out); fputc(b_safe, out);
                        prev_rgb_safe[idx] = r_safe; prev_rgb_safe[idx + 1] = g_safe; prev_rgb_safe[idx + 2] = b_safe;
                    }
                }
            }
            fputc(OP_FRAME_END, out);
            is_first_frame = 0;
        }

        encoded_count++;
        if (layout_byte == 0xF0) printf("Processing Audio Blocks: %d\r", encoded_count);
        else printf("Compiling Stream Frame Vector: %d\r", encoded_count);
        fflush(stdout);
        
        if (layout_byte == 0xF0 && feof(pipe_audio)) break; 
    }

    fputc(OP_END, out);
    fseek(out, total_frames_offset, SEEK_SET);
    fwrite(&encoded_count, sizeof(uint32_t), 1, out);

    free(current_rgb); free(prev_rgb_safe); free(audio_buffer);
    if (pipe_video) pclose(pipe_video);
    if (pipe_audio) pclose(pipe_audio);
    fclose(out);

    printf("\nSuccessfully generated FSVC5 Standard compliance container with auto-embedded art.\n");
    return 0;
}
