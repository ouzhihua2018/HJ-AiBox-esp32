#!/usr/bin/env python3
"""估算源音频转为 P3 后的大致体积（与 convert_audio_to_p3.py 同参数：16kHz mono, 60ms/帧, Opus APPLICATION_AUDIO）。"""
import argparse
import os
import struct
import sys

# 由现有 assets 统计：约 100~140 字节/帧（含 4 字节头）
BYTES_PER_FRAME_TYPICAL = 120
FRAME_MS = 60


def estimate_from_duration_sec(seconds: float) -> int:
    frames = int(seconds * 1000 / FRAME_MS)
    return frames * BYTES_PER_FRAME_TYPICAL


def analyze_p3(path: str) -> dict:
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    frames = 0
    payload = 0
    while pos < len(data):
        if pos + 4 > len(data):
            break
        ps = struct.unpack(">H", data[pos + 2 : pos + 4])[0]
        pos += 4 + ps
        frames += 1
        payload += ps
    return {
        "bytes": len(data),
        "frames": frames,
        "duration_sec": frames * FRAME_MS / 1000.0,
        "avg_frame_bytes": (len(data) / frames) if frames else 0,
        "avg_payload": (payload / frames) if frames else 0,
    }


def main():
    parser = argparse.ArgumentParser(description="Estimate P3 size from source audio duration or file size")
    parser.add_argument("source", nargs="?", help="Source audio path (.mp3/.wav) or existing .p3")
    parser.add_argument("-d", "--duration-sec", type=float, help="Duration in seconds (if no file)")
    parser.add_argument("-s", "--source-mib", type=float, help="Source file size in MiB (rough: MP3 ~1MB/min @128kbps)")
    args = parser.parse_args()

    if args.source and os.path.isfile(args.source):
        if args.source.endswith(".p3"):
            info = analyze_p3(args.source)
            print(f"P3 file: {args.source}")
            print(f"  size: {info['bytes']:,} bytes ({info['bytes']/1024/1024:.2f} MiB)")
            print(f"  frames: {info['frames']}, duration: {info['duration_sec']:.1f}s")
            print(f"  avg frame: {info['avg_frame_bytes']:.0f} bytes")
            return
        try:
            import librosa
        except ImportError:
            print("Install librosa to probe audio duration from file", file=sys.stderr)
            sys.exit(1)
        dur = librosa.get_duration(path=args.source)
        est = estimate_from_duration_sec(dur)
        print(f"Source: {args.source}, duration: {dur:.1f}s")
        print(f"Estimated P3: ~{est:,} bytes ({est/1024/1024:.2f} MiB)")
        print(f"  (using ~{BYTES_PER_FRAME_TYPICAL} B/frame)")
        return

    if args.duration_sec:
        est = estimate_from_duration_sec(args.duration_sec)
        print(f"Duration: {args.duration_sec:.1f}s -> estimated P3 ~{est:,} bytes ({est/1024/1024:.2f} MiB)")
        return

    if args.source_mib:
        # 128kbps MP3 粗算: 1 MiB ≈ 65.5 s
        sec = args.source_mib * 1024 * 1024 / (128 * 1024 / 8)
        est = estimate_from_duration_sec(sec)
        print(f"Source ~{args.source_mib} MiB @128kbps MP3 -> ~{sec:.0f}s audio")
        print(f"Estimated P3: ~{est:,} bytes ({est/1024/1024:.2f} MiB)")
        print("App partition (ota_0): 6 MiB — firmware + .rodata; 1~2 MiB P3 通常可接受，请编译后看 map。")
        return

    parser.print_help()


if __name__ == "__main__":
    main()
