"""WAV statistics for docs/AUDIO.md: size, duration, frames, peak, RMS, silence.

    C:\\Users\\DuWeisi\\py311\\python.exe dbgtest\\audio\\wav_stats.py dbgtest\\audio\\tour.wav
"""
import struct
import sys
import wave


def main(path):
    with open(path, "rb") as f:
        raw = f.read()
    size = len(raw)

    with wave.open(path, "rb") as w:
        ch, width, rate, frames = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        data = w.readframes(frames)

    assert width == 2, width
    samples = struct.unpack("<%dh" % (len(data) // 2), data)

    peak = max(abs(s) for s in samples) if samples else 0
    sumsq = sum(s * s for s in samples)
    rms = (sumsq / len(samples)) ** 0.5 if samples else 0.0
    nonzero = sum(1 for s in samples if s)

    print("file        : %s" % path)
    print("bytes       : %d (%.2f MiB)" % (size, size / 1048576.0))
    print("channels    : %d, %d-bit, %d Hz" % (ch, width * 8, rate))
    print("frames      : %d -> %.3f s" % (frames, frames / float(rate) if rate else 0))
    print("peak        : %d (%.2f%% FS)" % (peak, peak * 100.0 / 32768.0))
    print("rms         : %.1f (%.2f%% FS, %.1f dBFS)" % (
        rms, rms * 100.0 / 32768.0,
        20.0 * __import__("math").log10(rms / 32768.0) if rms > 0 else float("-inf")))
    print("nonzero     : %d of %d samples" % (nonzero, len(samples)))
    print("verdict     : %s" % ("SILENT (all samples zero)" if peak == 0 and nonzero == 0 else "has signal"))
    print("data_bytes  : %d (header says %d)" % (
        size - 44, struct.unpack("<I", raw[40:44])[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "dbgtest/audio/tour.wav"))
