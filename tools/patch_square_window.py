"""Make the drawn window square: duplicate every source row vertically.

The strip builder wrote each guest pixel twice horizontally but left the row
count untouched, so the panel received 192x96 of content stretched into a
192x96 region - the guest picture looked like a wide rectangle.  Each source row
now occupies two panel rows, and the draw call asks for rows * kScale.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"


def main():
    s = io.open(PATH, encoding="utf-8").read()

    if "kWinW * kScale * kChunkRows * kScale * 2" in s:
        print("row duplication already present")
        return 0

    old_size = "static uint8_t s_strip[kWinW * kScale * kChunkRows * 2] __attribute__((aligned(4)));"
    new_size = "static uint8_t s_strip[kWinW * kScale * kChunkRows * kScale * 2] __attribute__((aligned(4)));"
    if old_size not in s:
        raise SystemExit("strip declaration anchor missing")
    s = s.replace(old_size, new_size, 1)

    old_dst = "            uint8_t *dst = s_strip + r * kWinW * kScale * 2;"
    new_dst = "            uint8_t *dst = s_strip + r * kWinW * kScale * kScale * 2;"
    if old_dst not in s:
        raise SystemExit("dst anchor missing")
    s = s.replace(old_dst, new_dst, 1)

    old_tail = "            }\n        }\n        s_have_prev = true;"
    new_tail = ("            }\n"
                "            // vertical 2x: the same panel row twice\n"
                "            memcpy(dst + kWinW * kScale * 2, dst, kWinW * kScale * 2);\n"
                "        }\n"
                "        s_have_prev = true;")
    if old_tail not in s:
        raise SystemExit("row tail anchor missing")
    s = s.replace(old_tail, new_tail, 1)

    old_draw = "display_draw_bitmap(kLeft, kTop + y0, kWinW * kScale, rows, s_strip)"
    new_draw = "display_draw_bitmap(kLeft, kTop + y0 * kScale, kWinW * kScale, rows * kScale, s_strip)"
    if old_draw not in s:
        raise SystemExit("draw anchor missing")
    s = s.replace(old_draw, new_draw, 1)

    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("window is now square: rows duplicated, draw height scaled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
