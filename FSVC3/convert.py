import cv2
import numpy as np
import os
import struct
import subprocess

def int_to_visual_hex_bytes(val):
    """Converts an integer like 480 into literal hex bytes b'\\x04\\x80'"""
    s = f"{val:04d}"
    return bytes.fromhex(s)

def encode_freebbn_stream(video_path, output_filename, width=480, height=512, target_fps=20):
    if not os.path.exists(video_path):
        print(f"Error: Target video path '{video_path}' does not exist.")
        return

    # --- 1. EXTRACT 8-BIT UNSIGNED PCM ---
    sample_rate = 22050
    audio_temp = "temp_u8.raw"
    print("Extracting lossless 8-bit PCM audio track...")
    subprocess.run([
        'ffmpeg', '-y', '-i', video_path, 
        '-f', 'u8', '-acodec', 'pcm_u8', 
        '-ac', '1', '-ar', str(sample_rate), audio_temp
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Fixed 1102 bytes per 50ms block (20 FPS)
    FIXED_AUDIO_SIZE = 1102

    cap = cv2.VideoCapture(video_path)
    orig_fps = cap.get(cv2.CAP_PROP_FPS) or target_fps
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    frame_interval = max(1, round(orig_fps / target_fps))
    delay = int(1000 / target_fps)

    # FreeBBN Master Opcodes
    OP_START = 0xF0
    OP_FRAME_START = 0xF1
    OP_FRAME_END = 0xF2
    OP_AUDIO_START = 0xF3
    OP_AUDIO_END = 0xF4
    OP_END = 0xFF

    print(f"Encoding '{video_path}' to '{output_filename}'...")
    print(f"Targeting: {width}x{height} @ {target_fps} FPS with Audio Locked at {FIXED_AUDIO_SIZE} Bytes")

    with open(output_filename, 'wb') as f, open(audio_temp, 'rb') as fa:
        # --- WRITE 16-BYTE HEADER ---
        f.write(b"fsvc3")                               # Magic (5B)
        f.write(int_to_visual_hex_bytes(width))         # Width (2B Visual Hex)
        f.write(int_to_visual_hex_bytes(height))        # Height (2B Visual Hex)
        f.write(struct.pack("<IB", total_frames // frame_interval, delay))  # Total Frames (4B) + Delay (1B)
        f.write(b"\x00\x00")                            # Padding to hit offset 0x10

        # --- WRITE 48-BYTE METADATA SECTOR ---
        meta_text = "FreeBBN Standard Video Container v3"
        f.write(meta_text.encode('ascii').ljust(48, b"\x00"))

        # --- ANCHOR FILE ENTRY POINT AT 0x40 ---
        f.write(struct.pack("B", OP_START))

        prev_frame = None
        frame_idx = 0
        encoded_count = 0

        while True:
            ret, frame = cap.read()
            if not ret:
                break

            if frame_idx % frame_interval == 0:
                # --- A. PACK AUDIO FOR THIS BLOCK ---
                audio_chunk = fa.read(FIXED_AUDIO_SIZE)
                
                # Use .copy() here to make the NumPy array writable!
                audio_np = np.frombuffer(audio_chunk, dtype=np.uint8).copy() if audio_chunk else np.array([], dtype=np.uint8)
                
                # Pad out with silence (0x80) if video outlasts audio track
                if len(audio_np) < FIXED_AUDIO_SIZE:
                    padding = np.full((FIXED_AUDIO_SIZE - len(audio_np)), 0x80, dtype=np.uint8)
                    audio_np = np.concatenate((audio_np, padding))
                
                # Apply your absolute 0xEF ceiling clamp (This will now execute flawlessly!)
                audio_np[audio_np > 0xEF] = 0xEF

                f.write(struct.pack("B", OP_AUDIO_START))
                f.write(audio_np.tobytes())
                f.write(struct.pack("B", OP_AUDIO_END))

                # --- B. PACK DELTA VIDEO FRAME ---
                resized = cv2.resize(frame, (width, height))
                rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
                
                # Note: keeping your original 5-bit shift layout here to match your colors
                rgb_safe = (rgb >> 3) & 0x1F

                f.write(struct.pack("B", OP_FRAME_START))

                if prev_frame is None:
                    ys, xs = np.mgrid[0:height, 0:width]
                    ys, xs = ys.flatten(), xs.flatten()
                    rs = rgb_safe[:, :, 0].flatten()
                    gs = rgb_safe[:, :, 1].flatten()
                    bs = rgb_safe[:, :, 2].flatten()
                    
                    for i in range(len(xs)):
                        f.write(int_to_visual_hex_bytes(xs[i]))
                        f.write(int_to_visual_hex_bytes(ys[i]))
                        f.write(struct.pack("<BBB", rs[i], gs[i], bs[i]))
                else:
                    diff_mask = np.any(rgb_safe != prev_frame, axis=2)
                    ys, xs = np.where(diff_mask)
                    changed_pixels = rgb_safe[ys, xs]
                    
                    for i in range(len(xs)):
                        f.write(int_to_visual_hex_bytes(xs[i]))
                        f.write(int_to_visual_hex_bytes(ys[i]))
                        f.write(struct.pack("<BBB", changed_pixels[i,0], changed_pixels[i,1], changed_pixels[i,2]))

                f.write(struct.pack("B", OP_FRAME_END))
                encoded_count += 1
                prev_frame = rgb_safe
                print(f"Encoding Progress: Frame {encoded_count}", end="\r")

            frame_idx += 1

        f.write(struct.pack("B", OP_END))

    cap.release()
    if os.path.exists(audio_temp):
        os.remove(audio_temp)
    print(f"\nSuccessfully compiled '{output_filename}' ({os.path.getsize(output_filename)/(1024*1024):.2f} MB)")

if __name__ == "__main__":
    encode_freebbn_stream("video.mp4", "video.fsvc3")
