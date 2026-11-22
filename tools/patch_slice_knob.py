"""Make the host slice length tunable so device settings can be reproduced."""
import io

PATH = "emu/main.cpp"

OLD = "static const int      SLICE_CYCLES = 2000;"
NEW = """static const int      SLICE_CYCLES = [] {
    const char *e = std::getenv("GPCE4_SLICE");
    return e ? std::atoi(e) : 2000;
}();"""


def main():
    s = io.open(PATH, encoding="utf-8").read()
    if "GPCE4_SLICE" in s:
        print("already tunable")
        return 0
    if OLD not in s:
        raise SystemExit("slice anchor missing")
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s.replace(OLD, NEW, 1))
    print("slice length now reads GPCE4_SLICE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
