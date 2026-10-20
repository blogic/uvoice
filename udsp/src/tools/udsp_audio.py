# SPDX-License-Identifier: LGPL-2.1-only
#
# udsp - software voice DSP for FXS lines
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
"""Audio helpers for versioned and headerless udsp line captures."""

import math
import struct
import wave

SAMPLE_RATE = 8000
CAPTURE_MAGIC = b"UDSPLCAP"
CAPTURE_VERSION = 1
CAPTURE_FORMAT_PCMU = 0
CAPTURE_HEADER_BYTES = 16
ULAW_BIAS = 0x84
ULAW_CLIP = 32635
FULL_SCALE = 32768.0


def ulaw_decode_byte(value):
    """One G.711 mu-law byte to a 16 bit linear sample (ITU-T G.711)."""
    value = ~value & 0xFF
    sign = value & 0x80
    exponent = (value >> 4) & 0x07
    mantissa = value & 0x0F
    sample = (((mantissa << 3) + ULAW_BIAS) << exponent) - ULAW_BIAS
    return -sample if sign else sample


ULAW_TABLE = [ulaw_decode_byte(v) for v in range(256)]


def ulaw_decode(data):
    """Raw mu-law bytes to a list of 16 bit linear samples."""
    return [ULAW_TABLE[b] for b in data]


def ulaw_encode_sample(sample):
    """One 16 bit linear sample to a G.711 mu-law byte (ITU-T G.711)."""
    sign = 0
    if sample < 0:
        sample = -sample
        sign = 0x80
    if sample > ULAW_CLIP:
        sample = ULAW_CLIP
    sample += ULAW_BIAS
    exponent = 7
    mask = 0x4000
    while exponent > 0 and not sample & mask:
        exponent -= 1
        mask >>= 1
    mantissa = (sample >> (exponent + 3)) & 0x0F
    return ~(sign | (exponent << 4) | mantissa) & 0xFF


def ulaw_encode(samples):
    return bytes(ulaw_encode_sample(int(s)) for s in samples)


def wav_write(path, samples, sample_rate=SAMPLE_RATE):
    """Write 16 bit mono PCM at sample_rate."""
    with wave.open(path, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(sample_rate)
        out.writeframes(struct.pack("<%dh" % len(samples), *samples))


def wav_read(path):
    """Read mono 16 bit PCM and return (samples, sample_rate)."""
    with wave.open(path, "rb") as src:
        if src.getnchannels() != 1 or src.getsampwidth() != 2:
            raise ValueError("%s: need mono 16 bit PCM" % path)
        sample_rate = src.getframerate()
        if sample_rate <= 0:
            raise ValueError("%s: invalid sample rate %d" % (path, sample_rate))
        count = src.getnframes()
        samples = list(struct.unpack("<%dh" % count, src.readframes(count)))
        return samples, sample_rate


def capture_read(path):
    """Read a version 1 capture or a headerless PCMU capture."""
    with open(path, "rb") as src:
        data = src.read()
    if not data.startswith(CAPTURE_MAGIC):
        return ulaw_decode(data), SAMPLE_RATE
    if len(data) < CAPTURE_HEADER_BYTES:
        raise ValueError("%s: truncated capture header" % path)
    magic, version, payload_format, header_bytes, sample_rate = \
        struct.unpack("<8sBBHI", data[:CAPTURE_HEADER_BYTES])
    if magic != CAPTURE_MAGIC:
        raise ValueError("%s: invalid capture magic" % path)
    if version != CAPTURE_VERSION:
        raise ValueError("%s: unsupported capture version %d" % (path, version))
    if payload_format != CAPTURE_FORMAT_PCMU:
        raise ValueError("%s: unsupported payload format %d" %
                         (path, payload_format))
    if header_bytes != CAPTURE_HEADER_BYTES:
        raise ValueError("%s: invalid header size %d" % (path, header_bytes))
    if sample_rate <= 0:
        raise ValueError("%s: invalid sample rate %d" % (path, sample_rate))
    return ulaw_decode(data[header_bytes:]), sample_rate


def audio_load(path, force_ulaw=False):
    """Return (samples, sample_rate) from WAV or a line capture."""
    if not force_ulaw and path.lower().endswith(".wav"):
        return wav_read(path)
    return capture_read(path)


def goertzel(frame, freq_hz, sample_rate=SAMPLE_RATE):
    """Amplitude of one frequency in a frame, in sample units (a full scale
    sine reads 32767)."""
    coeff = 2.0 * math.cos(2.0 * math.pi * freq_hz / sample_rate)
    s1 = s2 = 0.0
    for value in frame:
        s0 = value + coeff * s1 - s2
        s2 = s1
        s1 = s0
    power = s1 * s1 + s2 * s2 - coeff * s1 * s2
    return math.sqrt(max(power, 0.0)) / (len(frame) / 2.0)


def rms(frame):
    if not frame:
        return 0.0
    return math.sqrt(sum(v * v for v in frame) / len(frame))


def dbfs(amplitude):
    if amplitude <= 0.0:
        return -99.0
    return 20.0 * math.log10(amplitude / FULL_SCALE)


def tone(freq_hz, count, amplitude, phase=0.0, sample_rate=SAMPLE_RATE):
    """A sine of count samples; returns (samples, phase) so bursts chain."""
    step = 2.0 * math.pi * freq_hz / sample_rate
    out = []
    for _ in range(count):
        out.append(int(round(amplitude * math.sin(phase))))
        phase += step
        if phase >= 2.0 * math.pi:
            phase -= 2.0 * math.pi
    return out, phase
