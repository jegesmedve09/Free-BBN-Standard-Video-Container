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

// --- Binary Delta Pixel Struct (6 Bytes total) ---
#pragma pack(push, 1)
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t color; // RGB565
} DeltaPixel;
#pragma pack(pop)

// --- Fast RGB888 to RGB565 conversion helper ---
static inline uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <input_file> <output.fsvc5> [width] [height] [fps] [channels] [samplerate] [layout]\n", argv[0]);
        return 1;
    }

    const char* input_filename = argv[1];
    const char* output_filename = argv[2];
    uint16_t width = (argc > 3) ? (uint16_t)atoi(argv[3]) : 1920;
    uint16_t height = (argc > 4) ? (uint16_t)atoi(argv[4]) : 1080;
    
    // --- NEW: Dynamic FPS Argument (Defaults to 20 FPS) ---
    int target_fps = (argc > 5) ? atoi(argv[5]) : 20;
    if (target_fps < 1) target_fps = 1;
    if (target_fps > 255) target_fps = 255; // Fit inside 8-bit limit

    int channels = (argc > 6) ? atoi(argv[6]) : 2;      
    int sample_rate = (argc > 7) ? atoi(argv[7]) : 44100; 
    const char* layout_str = (argc > 8) ? argv[8] : "both";

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
    
    // Dynamic audio chunk sizing based on specified FPS
    size_t audio_chunk_size = (sample_rate / target_fps) * channels;

    FILE* out = fopen(output_filename, "wb");
    if (!out) {
        printf("Error: Could not open output file %s\n", output_filename);
        return 1;
    }

    // --- SMART THUMBNAIL HIERARCHY PIPELINE ---
    uint8_t final_thumb[4096];
    memset(final_thumb, 0x3A, 4096); 
    
    char thumb_cmd[512] = {0};
    int extracted_successfully = 0;

    if (layout_byte == 0xFF || layout_byte == 0x0F) {
        snprintf(thumb_cmd, sizeof(thumb_cmd),
                 "ffmpeg -loglevel quiet -i \"%s\" -ss 00:00:05 -vframes 1 -vf \"scale=64:64\" -f rawvideo -pix_fmt gray -", 
                 input_filename);
        FILE* pipe_thumb = popen(thumb_cmd, "r");
        if (pipe_thumb) {
            size_t bytes_read = fread(final_thumb, 1, 4096, pipe_thumb);
            pclose(pipe_thumb);
            if (bytes_read == 4096) extracted_successfully = 1;
        }

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
        snprintf(thumb_cmd, sizeof(thumb_cmd),
                 "ffmpeg -loglevel quiet -i \"%s\" -an -ss 00:00:05 -vframes 1 -vf \"scale=64:64\" -f rawvideo -pix_fmt gray -", 
                 input_filename);
        FILE* pipe_thumb = popen(thumb_cmd, "r");
        if (pipe_thumb) {
            size_t bytes_read = fread(final_thumb, 1, 4096, pipe_thumb);
            pclose(pipe_thumb);
            if (bytes_read == 4096) extracted_successfully = 1;
        }

        if (!extracted_successfully) {
            snprintf(thumb_cmd, sizeof(thumb_cmd),
                     "ffmpeg -loglevel quiet -i \"%s\" -an -vframes 1 -vf \"scale=64:64\" -f rawvideo -pix_fmt gray -", 
                     input_filename);
            pipe_thumb = popen(thumb_cmd, "r");
            if (pipe_thumb) {
                size_t bytes_read = fread(final_thumb, 1, 4096, pipe_thumb);
                pclose(pipe_thumb);
                if (bytes_read == 4096) extracted_successfully = 1;
            }
        }

        if (!extracted_successfully) {
            FILE* raw_check = fopen("thumbnail.raw", "rb");
            if (!raw_check) raw_check = fopen("thumbnail.data", "rb");
            if (raw_check) {
                size_t bytes_read = fread(final_thumb, 1, 4096, raw_check);
                fclose(raw_check);
                if (bytes_read == 4096) extracted_successfully = 1;
            }
        }
    }

    // --- MAIN STREAM PIPES ---
    char ffmpeg_video_cmd[512] = {0};
    char ffmpeg_audio_cmd[512] = {0};
    FILE* pipe_video = NULL;
    FILE* pipe_audio = NULL;

    if (layout_byte == 0xFF || layout_byte == 0x0F) {
        snprintf(ffmpeg_video_cmd, sizeof(ffmpeg_video_cmd),
                 "ffmpeg -loglevel quiet -i \"%s\" -vf \"fps=%d,scale=%d:%d\" -f rawvideo -pix_fmt rgb24 -",
                 input_filename, target_fps, width, height);
        pipe_video = popen(ffmpeg_video_cmd, "r");
    }
    
    if (layout_byte == 0xFF || layout_byte == 0xF0) {
        snprintf(ffmpeg_audio_cmd, sizeof(ffmpeg_audio_cmd),
                 "ffmpeg -loglevel quiet -i \"%s\" -f u8 -acodec pcm_u8 -ac %d -ar %d -",
                 input_filename, channels, sample_rate);
        pipe_audio = popen(ffmpeg_audio_cmd, "r");
    }

    printf("[FreeBBN Sonic Video Core v6 Initialized]\n");
    printf("Config: %dx%d @ %d FPS | Audio: %d Channels at %dHz\n\n", width, height, target_fps, channels, sample_rate);

    size_t frame_pixel_count = width * height;
    uint8_t* current_rgb = (uint8_t*)malloc(frame_pixel_count * 3);
    
    uint16_t* prev_565 = (uint16_t*)calloc(frame_pixel_count, sizeof(uint16_t));
    uint8_t* audio_buffer = (uint8_t*)malloc(audio_chunk_size);
    DeltaPixel* delta_buf = (DeltaPixel*)malloc(frame_pixel_count * sizeof(DeltaPixel));

    // Calculate delay in milliseconds from target_fps
    uint8_t delay_byte = (uint8_t)(1000 / target_fps);

    // --- WRITE COMPLIANT BINARY HEADER BLOCK ---
    fwrite("fsvc6", 1, 5, out);                       // Magic ID
    fwrite(&width, sizeof(uint16_t), 1, out);          // Binary Width (2B)
    fwrite(&height, sizeof(uint16_t), 1, out);         // Binary Height (2B)
    
    long total_frames_offset = ftell(out);            
    uint32_t encoded_count = 0;
    fwrite(&encoded_count, sizeof(uint32_t), 1, out); 
    
    fwrite(&delay_byte, sizeof(uint8_t), 1, out); 
    fwrite(&audio_config_byte, 1, 1, out);     
    fwrite(&layout_byte, 1, 1, out);           

    // --- EMBED RAW FPS AT OFFSET 0x10 ---
    char meta_1[16] = {0};
    meta_1[0] = (uint8_t)target_fps; // Byte 0x10 stores actual raw FPS integer!
    strncpy(meta_1 + 1, "empty_strg", 14); // Remaining 15 bytes reserved for string ID
    fwrite(meta_1, 1, 16, out);

    // Write 4096-byte 64x64 Thumbnail Block
    fwrite(final_thumb, 1, 4096, out);

    char meta_2[224] = {0};
    strncpy(meta_2, "Title: Asset Build; Core: FreeBBN Engine v5;", sizeof(meta_2) - 1);
    fwrite(meta_2, 1, 224, out);

    uint8_t start_token = OP_START;           
    fwrite(&start_token, 1, 1, out);

    int is_first_frame = 1;

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
            fputc(OP_AUDIO_START, out);
            fwrite(audio_buffer, 1, audio_chunk_size, out);
            fputc(OP_AUDIO_END, out);
        }

        if (has_video_stream) {
            uint32_t delta_count = 0;

            for (uint16_t y = 0; y < height; y++) {
                for (uint16_t x = 0; x < width; x++) {
                    size_t idx = y * width + x;
                    size_t rgb_idx = idx * 3;

                    uint16_t c565 = rgb888_to_rgb565(
                        current_rgb[rgb_idx], 
                        current_rgb[rgb_idx + 1], 
                        current_rgb[rgb_idx + 2]
                    );

                    if (is_first_frame || c565 != prev_565[idx]) {
                        delta_buf[delta_count].x = x;
                        delta_buf[delta_count].y = y;
                        delta_buf[delta_count].color = c565;
                        delta_count++;
                        prev_565[idx] = c565;
                    }
                }
            }

            fputc(OP_FRAME_START, out);
            fwrite(&delta_count, sizeof(uint32_t), 1, out);
            fwrite(delta_buf, sizeof(DeltaPixel), delta_count, out);
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

    free(current_rgb); free(prev_565); free(audio_buffer); free(delta_buf);
    if (pipe_video) pclose(pipe_video);
    if (pipe_audio) pclose(pipe_audio);
    fclose(out);

    printf("\nSuccessfully generated Optimized Binary FSVC6 container with auto-embedded art.\n");
    return 0;
}
