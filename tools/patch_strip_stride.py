"""Size the strip buffer from the row stride actually used.

Each source row is written twice on the panel (vertical 2x before, and still
2x of a 96-row image at kScale=1 means 96x96 pixels), so a source row occupies
kWinW * kScale * 2 * 2 bytes in the strip regardless of kScale.  The old formula
only multiplied by kScale once, which was correct by accident at kScale=2 and a
two-times overflow at kScale=1.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"


def main():
    s = io.open(PATH, encoding="utf-8").read()

    if "kSrcRowStride" in s:
        print("stride already fixed")
        return 0

    old_decl = "static uint8_t s_strip[kWinW * kScale * kChunkRows * kScale * 2] __attribute__((aligned(4)));"
    new_decl = ("static constexpr int kRowBytes = kWinW * kScale * 2;      // one panel row\n"
                "static constexpr int kSrcRowStride = kRowBytes * kScale;    // one guest row, scaled\n"
                "static uint8_t s_strip[kSrcRowStride * kChunkRows] __attribute__((aligned(4)));")
    if old_decl not in s:
        raise SystemExit("strip declaration anchor missing")
    s = s.replace(old_decl, new_decl, 1)

    old_dst = "            uint8_t *dst = s_strip + r * kWinW * kScale * kScale * 2;"
    new_dst = "            uint8_t *dst = s_strip + r * kSrcRowStride;"
    if old_dst not in s:
        raise SystemExit("dst anchor missing")
    s = s.replace(old_dst, new_dst, 1)

    old_dup = "            memcpy(dst + kWinW * kScale * 2, dst, kWinW * kScale * 2);"
    if old_dup not in s:
        raise SystemExit("duplication anchor missing")
    lines = ["            for (int dy = 0; dy < kScale; dy++)",
             "                memcpy(dst + (dy + 1) * kRowBytes, dst, kRowBytes);"]
    s = s.replace(old_dup, "\n".join(lines), 1)

    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("strip sized by kSrcRowStride, rows duplicated kScale times")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
