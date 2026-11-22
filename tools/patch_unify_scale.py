"""Make kScale a single factor for both axes.

The identity path always wrote two panel pixels per guest pixel (a hardcoded
horizontal 2x), while the vertical duplication followed kScale.  At kScale=2 the
two happened to agree; at kScale=1 the picture stayed stretched horizontally and
the duplication wrote one row too far.  Both axes now follow kScale.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"

OLD_WRITE = """            if (kRot == 0) {
                for (int x = 0; x < kWinW; x++) {
                    const uint16_t c = src[x];
                    old[x] = c;
                    const uint16_t sw = (uint16_t)((c >> 8) | (c << 8));   // RGB565 big endian
                    const uint32_t pair = (uint32_t)sw | ((uint32_t)sw << 16);
                    *reinterpret_cast<uint32_t *>(dst + x * 4) = pair;
                }
            } else {"""

NEW_WRITE = """            if (kRot == 0) {
                for (int x = 0; x < kWinW; x++) {
                    const uint16_t c = src[x];
                    old[x] = c;
                    const uint16_t sw = (uint16_t)((c >> 8) | (c << 8));   // RGB565 big endian
                    if (kScale == 2) {
                        const uint32_t pair = (uint32_t)sw | ((uint32_t)sw << 16);
                        *reinterpret_cast<uint32_t *>(dst + x * 4) = pair;
                    } else {
                        *reinterpret_cast<uint16_t *>(dst + x * 2) = sw;
                    }
                }
            } else {"""

OLD_DUP = """            for (int dy = 0; dy < kScale; dy++)
                memcpy(dst + (dy + 1) * kRowBytes, dst, kRowBytes);"""

NEW_DUP = """            for (int dy = 1; dy < kScale; dy++)
                memcpy(dst + dy * kRowBytes, dst, kRowBytes);"""


def main():
    s = io.open(PATH, encoding="utf-8").read()
    if "if (kScale == 2) {" in s:
        print("already unified")
        return 0
    if OLD_WRITE not in s:
        raise SystemExit("write anchor missing")
    s = s.replace(OLD_WRITE, NEW_WRITE, 1)
    if OLD_DUP not in s:
        raise SystemExit("duplication anchor missing")
    s = s.replace(OLD_DUP, NEW_DUP, 1)
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("kScale now drives both axes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
