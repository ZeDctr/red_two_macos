#!/usr/bin/env python3
"""Extract Factor 5 .movie files from a Rogue Leader (GSWE64) disc image to MP4.

Usage:
  python3 tools/extract_movies.py <disc.rvz|.iso> [out_dir] [--only name1,name2]
"""
import ctypes
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
if not os.environ.get("RT_ZSTD_DLL"):
    for p in ("/opt/homebrew/lib/libzstd.dylib", "/usr/local/lib/libzstd.dylib"):
        if os.path.exists(p):
            os.environ["RT_ZSTD_DLL"] = p
            break
import rvz

_ADPCM_C = r"""
#include <stdint.h>
void decode_adpcm_stereo(
    const uint8_t* src0, const uint8_t* src1, int nbytes,
    const int16_t* coef0, const int16_t* coef1,
    int32_t* state0, int32_t* state1,
    int16_t* out_interleaved)
{
    int frames = nbytes / 8;
    int32_t h1_0 = state0[0], h2_0 = state0[1];
    int32_t h1_1 = state1[0], h2_1 = state1[1];
    for (int f = 0; f < frames; f++) {
        const uint8_t* b0 = src0 + f * 8;
        const uint8_t* b1 = src1 + f * 8;
        int sc0 = 1 << (b0[0] & 0xF), ix0 = (b0[0] >> 4) & 7;
        int sc1 = 1 << (b1[0] & 0xF), ix1 = (b1[0] >> 4) & 7;
        int c1_0 = coef0[ix0 * 2], c2_0 = coef0[ix0 * 2 + 1];
        int c1_1 = coef1[ix1 * 2], c2_1 = coef1[ix1 * 2 + 1];
        for (int i = 0; i < 14; i++) {
            uint8_t byte0 = b0[1 + (i >> 1)];
            uint8_t byte1 = b1[1 + (i >> 1)];
            int n0 = (i & 1) ? (byte0 & 0xF) : (byte0 >> 4);
            int n1 = (i & 1) ? (byte1 & 0xF) : (byte1 >> 4);
            if (n0 >= 8) n0 -= 16;
            if (n1 >= 8) n1 -= 16;
            int32_t s0 = (((n0 * sc0) << 11) + 1024 + c1_0 * h1_0 + c2_0 * h2_0) >> 11;
            int32_t s1 = (((n1 * sc1) << 11) + 1024 + c1_1 * h1_1 + c2_1 * h2_1) >> 11;
            if (s0 > 32767) s0 = 32767; else if (s0 < -32768) s0 = -32768;
            if (s1 > 32767) s1 = 32767; else if (s1 < -32768) s1 = -32768;
            h2_0 = h1_0; h1_0 = s0;
            h2_1 = h1_1; h1_1 = s1;
            int idx = (f * 14 + i) * 2;
            out_interleaved[idx] = (int16_t)s0;
            out_interleaved[idx + 1] = (int16_t)s1;
        }
    }
    state0[0] = h1_0; state0[1] = h2_0;
    state1[0] = h1_1; state1[1] = h2_1;
}
"""


def load_adpcm_lib(tmp_dir: str):
    c_path = os.path.join(tmp_dir, "adpcm.c")
    so_path = os.path.join(tmp_dir, "adpcm.dylib")
    with open(c_path, "w") as f:
        f.write(_ADPCM_C)
    subprocess.run(["cc", "-O2", "-shared", "-fPIC", c_path, "-o", so_path], check=True)
    lib = ctypes.CDLL(so_path)
    lib.decode_adpcm_stereo.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int16), ctypes.POINTER(ctypes.c_int16),
        ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32),
        ctypes.POINTER(ctypes.c_int16),
    ]
    return lib


def find_ffmpeg() -> str:
    for p in (shutil.which("ffmpeg"), "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"):
        if p and os.path.exists(p):
            return p
    raise RuntimeError("ffmpeg not found")


def fix_thp_jpeg(v: bytes) -> bytes:
    """Re-stuff 0xFF -> 0xFF 0x00 in JPEG scan data (Nintendo THP / Factor 5 .movie format)."""
    if len(v) < 4 or v[0] != 0xFF or v[1] != 0xD8:
        return v
    i = 2
    n = len(v)
    while i + 3 < n and v[i] == 0xFF:
        m = v[i + 1]
        if m == 0xD9:
            return v[: i + 2]
        ln = (v[i + 2] << 8) | v[i + 3]
        i += 2 + ln
        if m == 0xDA:
            break
    eoi = v.rfind(b"\xff\xd9")
    if eoi < i:
        eoi = n
    return v[:i] + v[i:eoi].replace(b"\xff", b"\xff\x00") + b"\xff\xd9"


def extract_one(disc, path: str, off: int, out_mp4: str, ffmpeg: str, adpcm_lib, tmp_dir: str):
    h = disc.read(off, 0x60)
    w, hgt, fps, max_fsz, nframes, ch, rate, fsz, foff, _ = struct.unpack(">HHfIIHHIII", h[:32])
    mjpeg_path = os.path.join(tmp_dir, "video.mjpeg")
    wav_path = os.path.join(tmp_dir, "audio.wav")

    has_audio = ch == 2 and rate > 0
    if has_audio:
        c0 = (ctypes.c_int16 * 16)(*struct.unpack(">16h", h[0x20:0x40]))
        c1 = (ctypes.c_int16 * 16)(*struct.unpack(">16h", h[0x40:0x60]))
        st0 = (ctypes.c_int32 * 2)(0, 0)
        st1 = (ctypes.c_int32 * 2)(0, 0)
        wf = wave.open(wav_path, "wb")
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(rate)

    pos = off + foff
    cur_sz = fsz
    with open(mjpeg_path, "wb") as vf:
        for _ in range(nframes):
            if cur_sz < 32:
                break
            chunk = disc.read(pos, cur_sz)
            next_sz, vsz, asz = struct.unpack(">III", chunk[:12])
            vdata = fix_thp_jpeg(chunk[32 : 32 + vsz])
            vf.write(vdata)
            if has_audio and asz > 0:
                half = asz // 2
                n_samples = (half // 8) * 14
                pcm = (ctypes.c_int16 * (n_samples * 2))()
                a0 = chunk[32 + vsz : 32 + vsz + half]
                a1 = chunk[32 + vsz + half : 32 + vsz + half * 2]
                adpcm_lib.decode_adpcm_stereo(a0, a1, half, c0, c1, st0, st1, pcm)
                wf.writeframesraw(bytes(pcm))
            pos += cur_sz
            cur_sz = next_sz

    if has_audio:
        wf.close()

    # Use exact NTSC 30000/1001 for 29.97 fps, else exact float
    fps_arg = "30000/1001" if abs(fps - 29.97) < 0.05 else f"{fps:.6f}"
    cmd = [ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-r", fps_arg, "-f", "mjpeg", "-i", mjpeg_path]
    if has_audio:
        cmd += ["-i", wav_path]
    # Factor 5 .movie JPEGs are stored bottom-up (vertically flipped); 640x240 movies (makingof,
    # teaser) are half-height 4:3 and scaled to 640x480 for universal player compatibility.
    filters = ["vflip"]
    if w == 640 and hgt == 240:
        filters.append("scale=640:480:flags=lanczos")
    cmd += ["-vf", ",".join(filters),
        "-c:v", "libx264", "-crf", "18", "-preset", "fast", "-pix_fmt", "yuv420p",
    ]
    if has_audio:
        cmd += ["-c:a", "aac", "-b:a", "192k"]
    cmd += ["-movflags", "+faststart", out_mp4]
    subprocess.run(cmd, check=True)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    disc_path = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) >= 3 and not sys.argv[2].startswith("--") else "extracted_movies"
    only = None
    for i, a in enumerate(sys.argv):
        if a == "--only" and i + 1 < len(sys.argv):
            only = set(x.lower() for x in sys.argv[i + 1].split(","))

    os.makedirs(out_dir, exist_ok=True)
    ffmpeg = find_ffmpeg()
    disc = rvz.Disc(disc_path)
    movies = [(p, off, sz) for p, isdir, off, sz in rvz.fst_entries(disc) if not isdir and p.lower().endswith(".movie")]
    if only:
        movies = [m for m in movies if os.path.splitext(os.path.basename(m[0]))[0].lower() in only]

    with tempfile.TemporaryDirectory(prefix="r2_movies_") as tmp_dir:
        adpcm_lib = load_adpcm_lib(tmp_dir)
        for idx, (path, off, sz) in enumerate(movies, 1):
            base = os.path.splitext(os.path.basename(path))[0]
            out_mp4 = os.path.join(out_dir, base + ".mp4")
            print(f"[{idx}/{len(movies)}] {base}.mp4 ...", flush=True)
            extract_one(disc, path, off, out_mp4, ffmpeg, adpcm_lib, tmp_dir)
    print(f"Done! {len(movies)} video(s) written to {os.path.abspath(out_dir)}")


if __name__ == "__main__":
    main()
