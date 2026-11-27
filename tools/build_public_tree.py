# -*- coding: utf-8 -*-
"""把工作区导出成"公开仓库树"（可重复执行）。

规则照 digimon-passport 的 tools/build_public_tree.py，本工程的差异只有白名单：

  * Bandai 的 ROM 镜像（`roms/*.bin`、`firmware/*/sparse/*.bin`）—— 绝不进
  * 本机绝对路径 —— 脱敏成 `<home>`
  * 内部交接单之外的过程文档（AUDIT / DELIVERY / 里程碑 / dbgtest）—— 不放
  * 构建产物与补丁工具留下的副本（`*.pregion`、`*.bak`、`build/`）—— 不放

用法：

    python tools/build_public_tree.py "D:\\path\\to\\aip-color-v1.17.0-github"

脚本只负责铺结构 + 复制 + 脱敏 + 重算 SHA256SUMS；
README / CHANGELOG / THIRD-PARTY / .gitignore / LICENSE / VERSION 由人写，
脚本不会创建它们（重跑也不会覆盖）。
"""
import hashlib
import os
import shutil
import sys

FW = os.path.join("firmware", "digicore-c3")
FW_TOP = ["CMakeLists.txt", "partitions.csv", "version.txt", "build.ps1"]
FW_TREES = [
    # (相对 firmware 的目录, 跳过的子目录, 跳过的扩展名)
    ("main", ("build", "managed_components", "sparse"), (".pregion", ".bak", ".o", ".bin")),
    ("assets", (), ()),
]

DOCS = ["HANDOFF_2026-11-24.md", "HANDOFF_UI_POSITION.md", "RESEARCH_DMC_MANUAL.md",
        "RESEARCH_SCREEN_AND_ROUNDING.md", "WEB_TOOL.md", "C3_PORT.md", "EMU.md", "AUDIO.md",
        "DMC_CN_GUIDE.md"]

# dmc_web.html is useless on its own: its images live in dmc_assets.js (V2, ~85 KB
# of inline base64), and build_web_assets.py is what regenerates that file.  The
# earlier list shipped only the .html, i.e. a release whose icon/sprite tabs were
# blank (R7 release pass, 2026-11-27).
TOOLS = ["dmc_web.html", "dmc_assets.js", "build_web_assets.py", "mame_shots.lua",
         "gen_brick_a8.py", "make_sparse_rom.py", "aot.py"]

# 本机路径在源码注释里出现过两种写法。  The backslash pattern must be a raw
# string with a single backslash: the previous r"C:\\Users\\..." spelled two and
# never matched, so the desensitisation step silently did nothing.
DESENSITIZE = [
    (r"C:\Users\DuWeisi", "<home>"),
    ("C:/Users/DuWeisi", "<home>"),
]

TEXT_EXT = (".md", ".txt", ".c", ".cpp", ".h", ".inc", ".py", ".ps1", ".csv", ".json",
            ".lua", ".js", ".html", ".css", "")

# The only .bin that belongs in the public tree: the GPCE4 SoC's 64 KiB internal
# mask ROM, which ships with MAME.  The cartridge image and the sparse page store
# are Bandai's and never leave the local release folder.
KEEP_BIN = ("main/assets/",)


def clean_text(text):
    for a, b in DESENSITIZE:
        text = text.replace(a, b)
    return text


def copy_file(src, dst, sanitize=True):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if sanitize and os.path.splitext(src)[1].lower() in TEXT_EXT:
        with open(src, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        new = clean_text(text)
        with open(dst, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(new)
        return new != text
    shutil.copy2(src, dst)
    return False


def main(dest):
    dest = os.path.abspath(dest)
    os.makedirs(dest, exist_ok=True)
    changed, count = [], 0

    for name in FW_TOP:
        src = os.path.join(FW, name)
        if os.path.exists(src):
            if copy_file(src, os.path.join(dest, "firmware", "digicore-c3", name)):
                changed.append(name)
            count += 1

    for sub, skip_dirs, skip_ext in FW_TREES:
        base = os.path.join(FW, sub)
        for root, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in skip_dirs]
            for f in sorted(files):
                rel = os.path.relpath(os.path.join(root, f), FW).replace("\\", "/")
                if any(f.endswith(e) for e in skip_ext) and not any(
                        rel.startswith(k) for k in KEEP_BIN):
                    continue
                src = os.path.join(root, f)
                rel = os.path.relpath(src, FW)
                if copy_file(src, os.path.join(dest, "firmware", "digicore-c3", rel)):
                    changed.append(rel)
                count += 1

    for f in DOCS:
        src = os.path.join("docs", f)
        if os.path.exists(src):
            if copy_file(src, os.path.join(dest, "docs", f)):
                changed.append("docs/" + f)
            count += 1
        else:
            print("  skip missing doc:", f)

    for f in TOOLS:
        src = os.path.join("tools", f)
        if os.path.exists(src):
            if copy_file(src, os.path.join(dest, "tools", f)):
                changed.append("tools/" + f)
            count += 1
        else:
            print("  skip missing tool:", f)

    print("\ncopied %d files to %s" % (count, dest))
    if changed:
        print("desensitized %d files: %s" % (len(changed), ", ".join(changed[:8])))
    sha256sums(dest)


def sha256sums(root):
    rows = []
    for cur, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in (".git",)]
        for f in sorted(files):
            if f == "SHA256SUMS.txt":
                continue
            p = os.path.join(cur, f)
            rel = os.path.relpath(p, root).replace("\\", "/")
            rows.append((rel, hashlib.sha256(open(p, "rb").read()).hexdigest()))
    rows.sort()
    with open(os.path.join(root, "SHA256SUMS.txt"), "w", encoding="utf-8", newline="\n") as fh:
        for rel, h in rows:
            fh.write("%s  %s\n" % (h, rel))
    print("SHA256SUMS.txt: %d entries" % len(rows))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(2)
    main(sys.argv[1])
