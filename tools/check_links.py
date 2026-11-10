#!/usr/bin/env python3
"""Check links and anchors in a built Jekyll site.

Usage: tools/check_links.py [_site] [/arrpc]

Catches the usual publishing mistakes: links that ignore the baseurl, links to
pages that were not generated, dead anchors, and unrendered Liquid tags.
"""

from __future__ import annotations

import os
import re
import sys
from urllib.parse import unquote, urlsplit

EXTERNAL = ("http://", "https://", "mailto:", "tel:", "data:", "javascript:", "//")


def html_files(site: str) -> list[str]:
    found = []
    for root, _, files in os.walk(site):
        for name in files:
            if name.endswith(".html"):
                found.append(os.path.join(root, name))
    return sorted(found)


def anchors(html: str) -> set[str]:
    ids = set(re.findall(r'\bid="([^"]+)"', html))
    names = set(re.findall(r'<a[^>]*\bname="([^"]+)"', html))
    return ids | names


def resolve(site: str, page: str, path: str) -> str:
    path = unquote(path)
    if path.startswith("/"):
        return os.path.join(site, path.lstrip("/"))
    return os.path.normpath(os.path.join(os.path.dirname(page), path))


def main(argv: list[str]) -> int:
    site = argv[1] if len(argv) > 1 else "_site"
    baseurl = (argv[2] if len(argv) > 2 else "/arrpc").rstrip("/")

    if not os.path.isdir(site):
        print(f"no built site at {site}", file=sys.stderr)
        return 2

    pages = html_files(site)
    if not pages:
        print(f"no HTML files in {site}", file=sys.stderr)
        return 2

    cache: dict[str, str] = {}
    problems: list[str] = []
    checked = 0

    def load(path: str) -> str:
        if path not in cache:
            with open(path, encoding="utf-8", errors="replace") as handle:
                cache[path] = handle.read()
        return cache[path]

    for page in pages:
        text = load(page)
        rel_page = os.path.relpath(page, site)

        if "{{" in text or "{%" in text:
            leftover = re.search(r"\{\{.*?\}\}|\{%.*?%\}", text)
            problems.append(f"{rel_page}: unrendered Liquid: {leftover.group(0)[:80]}")

        for raw in re.findall(r'\b(?:href|src)="([^"]*)"', text):
            link = raw.strip()
            if not link or link == "#" or link.startswith(EXTERNAL):
                continue
            checked += 1

            parts = urlsplit(link)
            target_path, fragment = parts.path, parts.fragment

            if target_path and target_path.startswith("/"):
                # Absolute links must live under the configured baseurl.
                if target_path == baseurl or target_path.startswith(baseurl + "/"):
                    target_path = target_path[len(baseurl):] or "/"
                else:
                    problems.append(f"{rel_page}: link outside baseurl: {link}")
                    continue

            target = page if not target_path else resolve(site, page, target_path)
            if os.path.isdir(target):
                target = os.path.join(target, "index.html")

            if not os.path.exists(target):
                problems.append(f"{rel_page}: missing target for {link}")
                continue

            if fragment and target.endswith(".html"):
                if fragment not in anchors(load(target)):
                    problems.append(f"{rel_page}: missing anchor #{fragment} in {link}")

    print(f"checked {checked} internal links across {len(pages)} pages")
    if problems:
        print(f"{len(problems)} problem(s):")
        for problem in sorted(set(problems))[:40]:
            print(f"  - {problem}")
        return 1
    print("links and anchors OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
