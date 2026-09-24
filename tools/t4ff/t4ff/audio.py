"""Audio conversion: PC WAV/xWMA -> Xbox 360 XMA2.

Streamed sounds are stored next to the converted fastfile as ``sounds/<path>.xma``
(CoD Xe redirects ``D:\\sounds\\`` requests of the active usermap there). These
files use a small container recovered from CoD Xenon's converted maps:

    0x00  'SDNS'
    0x04  u32 0
    0x08  u32 sample rate
    0x0C  u32 channel count
    0x10  u32 sample count (multiple of 512, the XMA frame size)
    0x14  u32 size of the XMA data
    ...   zero padding up to 0x1000
    0x1000 XMA2 packets (2048 bytes each)

All values are big endian.

There is no open source XMA encoder: encoding uses ``xma2encode.exe`` from the
Xbox 360 XDK (also shipped with the GDK and the XAudio2 desktop samples).
Decoding for verification uses FFmpeg (``pip install imageio-ffmpeg``).
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from typing import List, Optional, Tuple

import numpy as np

SDNS_MAGIC = b"SDNS"
SDNS_HEADER_SIZE = 0x1000
XMA_PACKET_SIZE = 2048
XMA_FRAME_SAMPLES = 512
WAVE_FORMAT_XMA2 = 0x166


class AudioError(Exception):
    pass


@dataclass
class Pcm:
    rate: int
    samples: np.ndarray  # int16, shape (frames, channels)

    @property
    def channels(self) -> int:
        return self.samples.shape[1]

    @property
    def frames(self) -> int:
        return self.samples.shape[0]

    @property
    def seconds(self) -> float:
        return self.frames / self.rate if self.rate else 0.0


@dataclass
class XmaStream:
    rate: int
    channels: int
    samples: int
    data: bytes  # XMA2 packets

    @property
    def packets(self) -> int:
        return len(self.data) // XMA_PACKET_SIZE


# ---------------------------------------------------------------------------
# RIFF helpers


def riff_chunks(data: bytes) -> List[Tuple[bytes, bytes]]:
    if data[:4] != b"RIFF":
        raise AudioError("not a RIFF file")
    chunks = []
    pos = 12
    while pos + 8 <= len(data):
        cid = data[pos : pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        chunks.append((cid, data[pos + 8 : pos + 8 + size]))
        pos += 8 + size + (size & 1)
    return chunks


def read_wav(data: bytes) -> Pcm:
    """Decode a PCM WAV (8/16/24/32 bit integer or 32 bit float). Other codecs go through FFmpeg."""
    chunks = dict(riff_chunks(data))
    fmt = chunks.get(b"fmt ")
    pcm = chunks.get(b"data")
    if fmt is None or pcm is None or data[8:12] != b"WAVE":
        return decode_with_ffmpeg(data)
    tag, channels, rate, _, block_align, bits = struct.unpack_from("<HHIIHH", fmt)
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = struct.unpack_from("<H", fmt, 24)[0]
    if tag == 1 and bits == 8:
        s = (np.frombuffer(pcm, dtype=np.uint8).astype(np.int16) - 128) << 8
    elif tag == 1 and bits == 16:
        s = np.frombuffer(pcm[: len(pcm) // 2 * 2], dtype="<i2")
    elif tag == 1 and bits == 24:
        raw = np.frombuffer(pcm[: len(pcm) // 3 * 3], dtype=np.uint8).reshape(-1, 3)
        s = ((raw[:, 2].astype(np.int32) << 24 | raw[:, 1].astype(np.int32) << 16 | raw[:, 0].astype(np.int32) << 8) >> 16).astype(np.int16)
    elif tag == 1 and bits == 32:
        s = (np.frombuffer(pcm, dtype="<i4") >> 16).astype(np.int16)
    elif tag == 3 and bits == 32:
        s = (np.clip(np.frombuffer(pcm, dtype="<f4"), -1, 1) * 32767).astype(np.int16)
    else:
        return decode_with_ffmpeg(data)
    frames = len(s) // channels
    return Pcm(rate, s[: frames * channels].reshape(frames, channels).astype(np.int16))


def write_wav(pcm: Pcm) -> bytes:
    body = pcm.samples.astype("<i2").tobytes()
    fmt = struct.pack("<HHIIHH", 1, pcm.channels, pcm.rate, pcm.rate * pcm.channels * 2, pcm.channels * 2, 16)
    return b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(body)) + b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(body)) + body


# ---------------------------------------------------------------------------
# FFmpeg


def ffmpeg_exe() -> Optional[str]:
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    try:
        import imageio_ffmpeg

        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        return None


def decode_with_ffmpeg(data: bytes, channels: Optional[int] = None) -> Pcm:
    exe = ffmpeg_exe()
    if exe is None:
        raise AudioError("FFmpeg is required to decode this sound (pip install imageio-ffmpeg)")
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "in.bin")
        with open(src, "wb") as f:
            f.write(data)
        probe = subprocess.run([exe, "-hide_banner", "-i", src], capture_output=True, text=True)
        rate, chans = 44100, 1
        for line in probe.stderr.splitlines():
            if "Audio:" in line:
                parts = [p.strip() for p in line.split(",")]
                for part in parts:
                    if part.endswith(" Hz"):
                        rate = int(part.split()[0])
                    elif part in ("mono",):
                        chans = 1
                    elif part in ("stereo",):
                        chans = 2
                    elif part.endswith(" channels"):
                        chans = int(part.split()[0])
                break
        chans = channels or chans
        out = subprocess.run([exe, "-hide_banner", "-loglevel", "error", "-i", src, "-f", "s16le", "-ac", str(chans), "-"], capture_output=True)
        if out.returncode != 0:
            raise AudioError(f"FFmpeg failed: {out.stderr.decode(errors='replace').strip()}")
    s = np.frombuffer(out.stdout, dtype="<i2")
    frames = len(s) // chans
    return Pcm(rate, s[: frames * chans].reshape(frames, chans).copy())


# ---------------------------------------------------------------------------
# Memory reduction


def downmix_mono(pcm: Pcm) -> Pcm:
    if pcm.channels == 1:
        return pcm
    mono = pcm.samples.astype(np.int32).mean(axis=1)
    return Pcm(pcm.rate, np.clip(mono, -32768, 32767).astype(np.int16)[:, None])


def resample(pcm: Pcm, rate: int) -> Pcm:
    """Band limited resampling (windowed sinc)."""
    if rate >= pcm.rate or pcm.frames == 0:
        return pcm
    ratio = rate / pcm.rate
    out_frames = int(pcm.frames * ratio)
    taps = 32
    cutoff = ratio * 0.95
    t = np.arange(out_frames) / ratio
    base = np.floor(t).astype(np.int64)
    frac = t - base
    src = pcm.samples.astype(np.float64)
    padded = np.pad(src, ((taps, taps + 1), (0, 0)))
    out = np.zeros((out_frames, pcm.channels))
    for k in range(-taps + 1, taps + 1):
        x = k - frac
        w = cutoff * np.sinc(cutoff * x) * (0.5 + 0.5 * np.cos(np.pi * x / taps))
        out += padded[base + k + taps] * w[:, None]
    return Pcm(rate, np.clip(np.round(out), -32768, 32767).astype(np.int16))


# ---------------------------------------------------------------------------
# XMA containers


def read_sdns(data: bytes) -> XmaStream:
    if data[:4] != SDNS_MAGIC:
        raise AudioError("not an SDNS stream")
    _, rate, channels, samples, size = struct.unpack_from(">5I", data, 4)
    return XmaStream(rate, channels, samples, bytes(data[SDNS_HEADER_SIZE : SDNS_HEADER_SIZE + size]))


def write_sdns(stream: XmaStream) -> bytes:
    header = SDNS_MAGIC + struct.pack(">5I", 0, stream.rate, stream.channels, stream.samples, len(stream.data))
    return header + bytes(SDNS_HEADER_SIZE - len(header)) + stream.data


def xma2_wav(stream: XmaStream) -> bytes:
    """Standard RIFF XMA2 file (as produced by xma2encode), e.g. for decoding with FFmpeg."""
    block_count = max(1, stream.packets)
    fmt = struct.pack(
        "<HHIIHHHHIIIIIIIBBH",
        WAVE_FORMAT_XMA2,
        stream.channels,
        stream.rate,
        stream.rate * stream.channels * 2,
        XMA_PACKET_SIZE,
        16,
        34,
        1,  # NumStreams
        0x4 if stream.channels == 1 else 0x3,  # ChannelMask
        stream.samples,  # SamplesEncoded
        XMA_PACKET_SIZE * block_count,  # BytesPerBlock
        0,  # PlayBegin
        stream.samples,  # PlayLength
        0,
        0,
        0,  # LoopCount
        4,  # EncoderVersion
        block_count,  # BlockCount
    )
    body = stream.data
    size = 4 + 8 + len(fmt) + 8 + len(body)
    return b"RIFF" + struct.pack("<I", size) + b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(body)) + body


def read_xma2_wav(data: bytes) -> XmaStream:
    """Parse the output of xma2encode (RIFF with an XMA2 fmt chunk or an 'XMA2' chunk)."""
    chunks = riff_chunks(data)
    fmt = None
    xma2 = None
    body = None
    for cid, chunk in chunks:
        if cid == b"fmt ":
            fmt = chunk
        elif cid == b"XMA2":
            xma2 = chunk
        elif cid == b"data":
            body = chunk
    if body is None:
        raise AudioError("XMA file without data chunk")
    if fmt is not None and struct.unpack_from("<H", fmt)[0] == WAVE_FORMAT_XMA2:
        _, channels, rate = struct.unpack_from("<HHI", fmt)
        samples = struct.unpack_from("<I", fmt, 24)[0]
        num_streams = struct.unpack_from("<H", fmt, 18)[0]
        if num_streams != 1:
            raise AudioError("multi stream XMA is not supported, encode mono or stereo sounds")
        return XmaStream(rate, channels, samples, body)
    if xma2 is not None:
        # XMA2WAVEFORMAT chunk (big endian)
        version, streams, _, loop_count, _, _, _, samples_encoded, rate, _, _, block_size, _, channels = struct.unpack_from(">BBBBIIIIIIIIHH", xma2 + bytes(64), 0)
        return XmaStream(rate, channels, samples_encoded, body)
    raise AudioError("not an XMA2 file")


# ---------------------------------------------------------------------------
# Encoding


class XmaEncoder:
    """Runs xma2encode.exe (directly on Windows, through wine elsewhere)."""

    def __init__(self, path: Optional[str] = None, quality: int = 60):
        self.path = path or os.environ.get("XMA2ENCODE") or shutil.which("xma2encode") or shutil.which("xma2encode.exe")
        if self.path is None:
            xedk = os.environ.get("XEDK")
            if xedk:
                candidate = os.path.join(xedk, "bin", "win32", "xma2encode.exe")
                if os.path.exists(candidate):
                    self.path = candidate
        self.quality = quality

    @property
    def available(self) -> bool:
        return self.path is not None and os.path.exists(self.path)

    def encode(self, pcm: Pcm) -> XmaStream:
        if not self.available:
            raise AudioError("xma2encode.exe not found (use --xma-encoder or set XMA2ENCODE / XEDK)")
        if pcm.channels > 2:
            pcm = downmix_mono(pcm)
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "in.wav")
            dst = os.path.join(tmp, "out.xma")
            with open(src, "wb") as f:
                f.write(write_wav(pcm))
            cmd = [self.path, src, "/TargetFile", dst, "/Quality", str(self.quality)]
            if os.name != "nt" and self.path.lower().endswith(".exe"):
                wine = shutil.which("wine") or shutil.which("wine64")
                if wine is None:
                    raise AudioError("xma2encode.exe needs wine on this platform")
                cmd = [wine] + cmd
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode != 0 or not os.path.exists(dst):
                raise AudioError(f"xma2encode failed: {result.stdout.strip()} {result.stderr.strip()}")
            with open(dst, "rb") as f:
                stream = read_xma2_wav(f.read())
        # The SDNS sample count is the number of XMA frames in the packets times 512.
        stream.samples = xma_frame_count(stream.data) * XMA_FRAME_SAMPLES
        return stream


def xma_frame_count(data: bytes) -> int:
    """Number of XMA frames, from the frame count field of every packet header."""
    return sum(struct.unpack_from(">I", data, o)[0] >> 26 for o in range(0, len(data) - 3, XMA_PACKET_SIZE))


def decode_xma(stream: XmaStream) -> Pcm:
    return decode_with_ffmpeg(xma2_wav(stream), channels=stream.channels)


def correlation(a: Pcm, b: Pcm) -> float:
    """Normalised correlation of two mono-mixed signals (best lag within +-4096 frames)."""
    x = downmix_mono(a).samples[:, 0].astype(np.float64)
    y = downmix_mono(b).samples[:, 0].astype(np.float64)
    n = min(len(x), len(y), 1 << 18)
    best = 0.0
    for lag in range(-4096, 4097, 64):
        xs = x[max(0, lag) : max(0, lag) + n]
        ys = y[max(0, -lag) : max(0, -lag) + n]
        m = min(len(xs), len(ys))
        if m < 1024:
            continue
        xs, ys = xs[:m], ys[:m]
        denom = np.sqrt((xs * xs).sum() * (ys * ys).sum())
        if denom:
            best = max(best, float((xs * ys).sum() / denom))
    return best


# ---------------------------------------------------------------------------
# Streamed sounds


AUDIO_EXTENSIONS = (".wav", ".mp3", ".ogg", ".flac")


def streamed_sound_target(rel: str) -> str:
    """'sound/eggs/para_egg.wav' -> 'sounds/eggs/para_egg.xma'."""
    parts = rel.replace("\\", "/").split("/")
    if parts and parts[0].lower() == "sound":
        parts = parts[1:]
    stem = os.path.splitext("/".join(parts))[0]
    return "sounds/" + stem + ".xma"


def convert_streamed_sounds(library, out_dir: str, encoder: XmaEncoder, max_rate: int = 0, mono: bool = False, log=print) -> dict:
    """Encode every sound/ file of the .iwd library to sounds/<path>.xma (SDNS) in ``out_dir``."""
    names = [n for n in library.names("sound/") if n.endswith(AUDIO_EXTENSIONS)]
    stats = {"sounds": len(names), "converted": 0, "failed": 0, "input_bytes": 0, "output_bytes": 0}
    if not names:
        return stats
    if not encoder.available:
        log(f"warning: {len(names)} streamed sounds need xma2encode.exe (Xbox 360 XDK), skipped. Use --xma-encoder.")
        stats["failed"] = len(names)
        return stats
    for name in names:
        data = library.read(name)
        stats["input_bytes"] += len(data)
        try:
            pcm = read_wav(data) if name.endswith(".wav") else decode_with_ffmpeg(data)
            if mono:
                pcm = downmix_mono(pcm)
            if max_rate:
                pcm = resample(pcm, max_rate)
            stream = encoder.encode(pcm)
        except AudioError as e:
            log(f"warning: {name}: {e}")
            stats["failed"] += 1
            continue
        target = os.path.join(out_dir, *streamed_sound_target(name).split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        out = write_sdns(stream)
        with open(target, "wb") as f:
            f.write(out)
        stats["converted"] += 1
        stats["output_bytes"] += len(out)
    return stats
