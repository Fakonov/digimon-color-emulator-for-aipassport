"""Publish the v1.14.0 release body and assets.

Kept as a file on purpose: passing Chinese through a PowerShell -c command
mangles it (the console code page, not the JSON, is what breaks), so the
strings live in this UTF-8 file and only ASCII (the token) goes through the
environment.

Usage:
    $env:GITHUB_TOKEN = <token>
    python tools/gh_release.py
"""
import io
import json
import os
import urllib.request

REPO = "Fakonov/digimon-color-emulator-for-aipassport"
RELEASE_ID = 459465243
NAME = "v1.14.0 — 流畅性：折叠、自适应预算、切换路径覆盖"
TOKEN = os.environ["GITHUB_TOKEN"]

ASSETS = [
    ("milestones/2026-11-26_v1.14.0_switch/digicore-c3-v1.14.0.bin",
     "digicore-c3-v1.14.0.bin"),
    ("milestones/2026-11-26_v1.14.0_switch/sha256.txt",
     "digicore-c3-v1.14.0.sha256.txt"),
]


def req(url, data=None, method="GET", ctype="application/json"):
    r = urllib.request.Request(url, data=data, method=method)
    r.add_header("Authorization", "Bearer " + TOKEN)
    r.add_header("User-Agent", "dsh")
    r.add_header("Accept", "application/vnd.github+json")
    if data is not None:
        r.add_header("Content-Type", ctype)
    with urllib.request.urlopen(r, timeout=300) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main():
    body = io.open("docs/RELEASE_v1.14.0.md", encoding="utf-8").read()
    payload = json.dumps({"name": NAME, "body": body}, ensure_ascii=False).encode("utf-8")
    r = req("https://api.github.com/repos/%s/releases/%d" % (REPO, RELEASE_ID),
            payload, "PATCH")
    print("patched:", r["html_url"])
    print("name_matches:", r["name"] == NAME)
    print("body_chars:", len(r["body"]))
    print("body_head:", r["body"][:40].replace("\n", " | "))

    existing = {a["name"]: a["size"] for a in r.get("assets", [])}
    for path, name in ASSETS:
        if name in existing:
            print("skip (already uploaded):", name, existing[name])
            continue
        data = open(path, "rb").read()
        url = ("https://uploads.github.com/repos/%s/releases/%d/assets?name=%s"
               % (REPO, RELEASE_ID, name))
        j = req(url, data, "POST", "application/octet-stream")
        print("uploaded:", j["name"], j["size"], "bytes")


if __name__ == "__main__":
    main()
