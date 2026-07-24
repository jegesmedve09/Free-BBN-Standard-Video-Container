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
        printf("Usage: %s <input.mp4> <output.fsvc4> [width] [height] [channels(1-15)] [samplerate(8000-48000)] [layout(both/audio/video)]\n", argv[0]);
        return 1;
    }

    const char* input_filename = argv[1];
    const char* output_filename = argv[2];
    int width = (argc > 3) ? atoi(argv[3]) : 1920;
    int height = (argc > 4) ? atoi(argv[4]) : 1080;
    int channels = (argc > 5) ? atoi(argv[5]) : 2;      // Default to Stereo
    int sample_rate = (argc > 6) ? atoi(argv[6]) : 44100; // Default to 44.1kHz
    const char* layout_str = (argc > 7) ? argv[7] : "both";

    if (channels < 1 || channels > 15) channels = 2;

    // Determine content flags
    uint8_t layout_byte = 0xFF; // Both
    if (strcmp(layout_str, "audio") == 0) layout_byte = 0xF0;
    if (strcmp(layout_str, "video") == 0) layout_byte = 0x0F;

    // Resolve Sample Rate Index Nibble
    uint8_t sr_nibble = 0x0;
    if (sample_rate == 8000)   sr_nibble = 0x1;
    else if (sample_rate == 11025) sr_nibble = 0x2;
    else if (sample_rate == 16000) sr_nibble = 0x3;
    else if (sample_rate == 22050) sr_nibble = 0x4;
    else if (sample_rate == 32000) sr_nibble = 0x5;
    else if (sample_rate == 44100) sr_nibble = 0x6;
    else if (sample_rate == 48000) sr_nibble = 0xF;
    else {
        sample_rate = 44100; // Fallback
        sr_nibble = 0x6;
    }

    // Assemble Config Byte: Upper = channels, Lower = samplerate index
    uint8_t audio_config_byte = (uint8_t)((channels << 4) | sr_nibble);

    // Calculate dynamic base audio size per 50ms block (samples per block * channels)
    // Formula: (Sample Rate / FPS) * channels
    size_t audio_chunk_size = (sample_rate / TARGET_FPS) * channels;

    FILE* out = fopen(output_filename, "wb");
    if (!out) {
        printf("Error: Could not open output file %s\n", output_filename);
        return 1;
    }

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

    printf("[FreeBBN Sonic Video Core v4 Initialized]\n");
    printf("Config: %dx%d @ %d FPS | Audio: %d Channels at %dHz | Size: %zu bytes/block\n\n", 
           width, height, TARGET_FPS, channels, sample_rate, audio_chunk_size);

    size_t frame_pixel_count = width * height;
    uint8_t* current_rgb = (uint8_t*)malloc(frame_pixel_count * 3);
    uint8_t* prev_rgb_safe = (uint8_t*)calloc(frame_pixel_count * 3, sizeof(uint8_t));
    uint8_t* audio_buffer = (uint8_t*)malloc(audio_chunk_size);

    uint8_t w_hex[2], h_hex[2];
    int_to_visual_hex_bytes(width, w_hex);
    int_to_visual_hex_bytes(height, h_hex);
    uint8_t delay_byte = (uint8_t)TARGET_DELAY;
    char meta_text[48] = "FreeBBN Sonic Video Container v4";

    // Header layout
    fwrite("fsvc4", 1, 5, out);               // 0x00 - 0x04
    fwrite(w_hex, 1, 2, out);                 // 0x05 - 0x06
    fwrite(h_hex, 1, 2, out);                 // 0x07 - 0x08
    
    long total_frames_offset = ftell(out);    // 0x09
    uint32_t encoded_count = 0;
    fwrite(&encoded_count, sizeof(uint32_t), 1, out); 
    
    fwrite(&delay_byte, sizeof(uint8_t), 1, out); // 0x0D
    fwrite(&audio_config_byte, 1, 1, out);     // 0x0E (DYNAMIC CONFIG)
    fwrite(&layout_byte, 1, 1, out);           // 0x0F (LAYOUT MANIFEST)
    fwrite(meta_text, 1, 48, out);             // 0x10 - 0x3F
    
    uint8_t start_token = OP_START;           // 0x40 Master Anchor
    fwrite(&start_token, 1, 1, out);

    int is_first_frame = 1;
    uint8_t coord_buffer[2];

    while (1) {
        int has_video_stream = (layout_byte == 0xFF || layout_byte == 0x0F);
        int has_audio_stream = (layout_byte == 0xFF || layout_byte == 0xF0);

        if (has_video_stream) {
            size_t vr = fread(current_rgb, 1, frame_pixel_count * 3, pipe_video);
            if (vr < frame_pixel_count * 3) break; 
        }

        // --- A. WRITE DYNAMIC AUDIO IF ENABLED ---
        if (has_audio_stream) {
            size_t ar = fread(audio_buffer, 1, audio_chunk_size, pipe_audio);
            if (ar < audio_chunk_size) {
                if (layout_byte == 0xF0 && ar == 0) break; // Audio-only termination
                memset(audio_buffer + ar, 0x80, audio_chunk_size - ar);
            }

            for (size_t i = 0; i < audio_chunk_size; i++) {
                if (audio_buffer[i] > 0xEF) audio_buffer[i] = 0xEF;
            }

            fputc(OP_AUDIO_START, out);
            fwrite(audio_buffer, 1, audio_chunk_size, out);
            fputc(OP_AUDIO_END, out);
        }

        // --- B. WRITE VIDEO IF ENABLED ---
        if (has_video_stream) {
            fputc(OP_FRAME_START, out);
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    size_t idx = (y * width + x) * 3;
                    uint8_t r_safe = (current_rgb[idx]     >> 3) & 0x1F;
                    uint8_t g_safe = (current_rgb[idx + 1] >> 3) & 0x1F;
                    uint8_t b_safe = (current_rgb[idx + 2] >> 3) & 0x1F;

                    if (is_first_frame || 
                        r_safe != prev_rgb_safe[idx] || 
                        g_safe != prev_rgb_safe[idx + 1] || 
                        b_safe != prev_rgb_safe[idx + 2]) {

                        int_to_visual_hex_bytes(x, coord_buffer);
                        fwrite(coord_buffer, 1, 2, out);
                        int_to_visual_hex_bytes(y, coord_buffer);
                        fwrite(coord_buffer, 1, 2, out);

                        fputc(r_safe, out);
                        fputc(g_safe, out);
                        fputc(b_safe, out);

                        prev_rgb_safe[idx]     = r_safe;
                        prev_rgb_safe[idx + 1] = g_safe;
                        prev_rgb_safe[idx + 2] = b_safe;
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

    printf("\nSuccessfully generated FSVC4 Standard compliance container.\n");
    return 0;
}
