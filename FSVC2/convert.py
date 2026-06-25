import cv2
import numpy as np
import os
import struct

def int_to_visual_hex_bytes(val):
    """Converts an integer like 480 into literal hex bytes b'\\x04\\x80'"""
    # Ensure it's padded to exactly 4 digits (e.g., 480 -> "0480")
    s = f"{val:04d}"
    return bytes.fromhex(s)

def encode_video_to_fsvc2(video_path, output_filename, width=480, height=512, target_fps=20):
    if not os.path.exists(video_path):
        print(f"Error: Target video path '{video_path}' does not exist.")
        return

    cap = cv2.VideoCapture(video_path)
    orig_fps = cap.get(cv2.CAP_PROP_FPS) or target_fps
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    frame_interval = max(1, round(orig_fps / target_fps))
    delay = int(1000 / target_fps)

    # Master Opcodes
    OP_START = 0xF0
    OP_FRAME_START = 0xF1
    OP_FRAME_END = 0xF2
    OP_END = 0xFF

    print(f"Encoding '{video_path}' to '{output_filename}'...")
    print(f"Targeting: {width}x{height} @ {target_fps} FPS (Interval: Every {frame_interval} frames)")

    with open(output_filename, 'wb') as f:
        # --- WRITE 16-BYTE HEADER ---
        f.write(b"fsvc2")                               # Magic (5B)
        f.write(int_to_visual_hex_bytes(width))         # Width (2B Visual Hex)
        f.write(int_to_visual_hex_bytes(height))        # Height (2B Visual Hex)
        f.write(struct.pack("<IB", total_frames // frame_interval, delay))  # Total Frames (4B) + Delay (1B)
        f.write(b"\x00\x00")                            # Padding to hit offset 0x10

        # --- WRITE 48-BYTE METADATA SECTOR ---
        meta_text = "FreeBBN Standard Video Container v2"
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
                # Resize and map to 5-bit depth per channel
                resized = cv2.resize(frame, (width, height))
                rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
                rgb_safe = (rgb >> 3) & 0x1F

                f.write(struct.pack("B", OP_FRAME_START))

                if prev_frame is None:
                    # Keyframe: Write every pixel
                    ys, xs = np.mgrid[0:height, 0:width]
                    ys, xs = ys.flatten(), xs.flatten()
                    rs = rgb_safe[:, :, 0].flatten()
                    gs = rgb_safe[:, :, 1].flatten()
                    bs = rgb_safe[:, :, 2].flatten()
                    
                    for i in range(len(xs)):
                        f.write(int_to_visual_hex_bytes(xs[i])) # X (2B)
                        f.write(int_to_visual_hex_bytes(ys[i])) # Y (2B)
                        f.write(struct.pack("<BBB", rs[i], gs[i], bs[i])) # RGB (3B)
                else:
                    # Delta Frame: Only write changed pixels
                    diff_mask = np.any(rgb_safe != prev_frame, axis=2)
                    ys, xs = np.where(diff_mask)
                    changed_pixels = rgb_safe[ys, xs]
                    
                    for i in range(len(xs)):
                        f.write(int_to_visual_hex_bytes(xs[i])) # X (2B)
                        f.write(int_to_visual_hex_bytes(ys[i])) # Y (2B)
                        f.write(struct.pack("<BBB", changed_pixels[i,0], changed_pixels[i,1], changed_pixels[i,2])) # RGB (3B)

                f.write(struct.pack("B", OP_FRAME_END))
                encoded_count += 1
                prev_frame = rgb_safe
                print(f"Encoding Progress: Frame {encoded_count}", end="\r")

            frame_idx += 1

        # Final Master Termination Token
        f.write(struct.pack("B", OP_END))

    cap.release()
    print(f"\nSuccessfully compiled '{output_filename}' ({os.path.getsize(output_filename)/(1024*1024):.2f} MB)")

if __name__ == "__main__":
    encode_video_to_fsvc2("video.mp4", "video.fsvc2")
