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

#define FIXED_AUDIO_SIZE 1102
#define TARGET_FPS       20
#define TARGET_DELAY     50

// Inline function to quickly write integer values to 2-byte BCD / Visual Hex
// Clean, safe inline function with direct indexing
static inline void int_to_visual_hex_bytes(int val, uint8_t* dest) {
    if (val < 0) val = 0;
    if (val > 9999) val = 9999;

    char s[16]; // Extra padding to satisfy static bounds analysis
    snprintf(s, sizeof(s), "%04d", val);
    
    dest[0] = ((s[0] - '0') << 4) | (s[1] - '0');
    dest[1] = ((s[2] - '0') << 4) | (s[3] - '0');
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <input_video.mp4> <output_video.fsvc3> [width] [height]\n", argv[0]);
        return 1;
    }

    const char* input_filename = argv[1];
    const char* output_filename = argv[2];
    int width = (argc > 3) ? atoi(argv[3]) : 1920;
    int height = (argc > 4) ? atoi(argv[4]) : 1080;

    // 1. Open Output Binary File
    FILE* out = fopen(output_filename, "wb");
    if (!out) {
        printf("Error: Could not open output file %s\n", output_filename);
        return 1;
    }

    // 2. Prepare Command Pipes to leverage FFmpeg for raw, synchronized byte streams
    char ffmpeg_video_cmd[512];
    char ffmpeg_audio_cmd[512];

    // Force FFmpeg to output raw RGB24 frames scaled directly to target dimensions at 20 FPS
    snprintf(ffmpeg_video_cmd, sizeof(ffmpeg_video_cmd),
             "ffmpeg -loglevel quiet -i \"%s\" -vf \"fps=%d,scale=%d:%d\" -f rawvideo -pix_fmt rgb24 -",
             input_filename, TARGET_FPS, width, height);

    // Force FFmpeg to output raw Unsigned 8-bit Mono audio matching the 22050Hz target clock
    snprintf(ffmpeg_audio_cmd, sizeof(ffmpeg_audio_cmd),
             "ffmpeg -loglevel quiet -i \"%s\" -f u8 -acodec pcm_u8 -ac 1 -ar 22050 -",
             input_filename);

    FILE* pipe_video = popen(ffmpeg_video_cmd, "r");
    FILE* pipe_audio = popen(ffmpeg_audio_cmd, "r");

    if (!pipe_video || !pipe_audio) {
        printf("Error: Failed to launch FFmpeg hardware pipeline engines.\n");
        if (pipe_video) pclose(pipe_video);
        if (pipe_audio) pclose(pipe_audio);
        fclose(out);
        return 1;
    }

printf("[FreeBBN Native C Encoder Core Initialized]\n");
    printf("Targeting: %dx%d @ %d FPS (Strict %dms blocks)\n\n", width, height, TARGET_FPS, TARGET_DELAY);

    // 3. Allocate Streaming Working Memory (Minimal Footprint)
    size_t frame_pixel_count = width * height;
    uint8_t* current_rgb = (uint8_t*)malloc(frame_pixel_count * 3);
    uint8_t* prev_rgb_safe = (uint8_t*)calloc(frame_pixel_count * 3, sizeof(uint8_t));
    uint8_t* audio_buffer = (uint8_t*)malloc(FIXED_AUDIO_SIZE);

    // Write Container Headers First to preserve standard layout offsets
    uint8_t w_hex[2], h_hex[2];
    int_to_visual_hex_bytes(width, w_hex);
    int_to_visual_hex_bytes(height, h_hex);
    uint8_t delay_byte = (uint8_t)TARGET_DELAY;
    uint8_t padding[2] = {0x00, 0x00};
    char meta_text[48] = "FreeBBN Standard Video Container v3";

    fwrite("fsvc3", 1, 5, out);
    fwrite(w_hex, 1, 2, out);
    fwrite(h_hex, 1, 2, out);
    
    // Placeholder for total frames count (we will seek back and update this at the end)
    long total_frames_offset = ftell(out);
    uint32_t encoded_count = 0;
    fwrite(&encoded_count, sizeof(uint32_t), 1, out);
    
    fwrite(&delay_byte, sizeof(uint8_t), 1, out);
    fwrite(padding, 1, 2, out);
    fwrite(meta_text, 1, 48, out);
    
    // Master offset anchor verification tag at 0x40
    uint8_t start_token = OP_START;
    fwrite(&start_token, 1, 1, out);

    int is_first_frame = 1;
    uint8_t coord_buffer[2];

    // 4. Stream Processing State Machine Loop (Direct I/O)
    while (1) {
        // Read raw synchronized video frame from FFmpeg output stream
        size_t video_bytes_read = fread(current_rgb, 1, frame_pixel_count * 3, pipe_video);
        if (video_bytes_read < frame_pixel_count * 3) {
            break; // Stream termination encountered natively
        }

        // --- A. WRITE AUDIO CHUNK DIRECTLY ---
        size_t audio_bytes_read = fread(audio_buffer, 1, FIXED_AUDIO_SIZE, pipe_audio);
        if (audio_bytes_read < FIXED_AUDIO_SIZE) {
            // Fill remaining tail buffer with silent unsigned 8-bit PCM baseline data (0x80)
            memset(audio_buffer + audio_bytes_read, 0x80, FIXED_AUDIO_SIZE - audio_bytes_read);
        }

        // Clip values to prevent accidental injection of high-level control opcodes
        for (int i = 0; i < FIXED_AUDIO_SIZE; i++) {
            if (audio_buffer[i] > 0xEF) audio_buffer[i] = 0xEF;
        }

        fputc(OP_AUDIO_START, out);
        fwrite(audio_buffer, 1, FIXED_AUDIO_SIZE, out);
        fputc(OP_AUDIO_END, out);

        // --- B. WRITE DELTA VIDEO PACKETS DIRECTLY ---
        fputc(OP_FRAME_START, out);

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                size_t idx = (y * width + x) * 3;

                // Scale down 24-bit depth to standard PS2 compatible 15-bit internal layout ranges
                uint8_t r_safe = (current_rgb[idx]     >> 3) & 0x1F;
                uint8_t g_safe = (current_rgb[idx + 1] >> 3) & 0x1F;
                uint8_t b_safe = (current_rgb[idx + 2] >> 3) & 0x1F;

                // Evaluate delta state vs previous reference matrix
                if (is_first_frame || 
                    r_safe != prev_rgb_safe[idx] || 
                    g_safe != prev_rgb_safe[idx + 1] || 
                    b_safe != prev_rgb_safe[idx + 2]) {

                    // Pack visual hex coordinate markers directly to disk
                    int_to_visual_hex_bytes(x, coord_buffer);
                    fwrite(coord_buffer, 1, 2, out);
                    int_to_visual_hex_bytes(y, coord_buffer);
                    fwrite(coord_buffer, 1, 2, out);

                    // Append sub-channel pixel records
                    fputc(r_safe, out);
                    fputc(g_safe, out);
                    fputc(b_safe, out);

                    // Update local frame memory map references
                    prev_rgb_safe[idx]     = r_safe;
                    prev_rgb_safe[idx + 1] = g_safe;
                    prev_rgb_safe[idx + 2] = b_safe;
                }
            }
        }

        fputc(OP_FRAME_END, out);
        is_first_frame = 0;
        encoded_count++;

        printf("Compiling Stream Frame Vector: %d\r", encoded_count);
        fflush(stdout);
    }

    // Write final termination token
    fputc(OP_END, out);

    // Seek back to the header and write the true total frames count
    fseek(out, total_frames_offset, SEEK_SET);
    fwrite(&encoded_count, sizeof(uint32_t), 1, out);

    // 5. Resource Deallocation
    free(current_rgb);
    free(prev_rgb_safe);
    free(audio_buffer);

    pclose(pipe_video);
    pclose(pipe_audio);
    fclose(out);

    printf("\nSuccessfully generated '%s'\n", output_filename);
    return 0;
}
