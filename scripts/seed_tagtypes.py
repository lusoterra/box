#!/usr/bin/env python3
"""Build a pre-seeded tag -> category map for box.

Pulls the most-used tags from the Danbooru tag index (ordered by post
count, same category numbering the app already uses: 0=general 1=artist
3=copyright 4=character 5=meta) and writes a TSV seed file:

    tag<TAB>category

Tag usage is Zipf-distributed: the top ~20k tags cover the vast bulk of
real occurrences, so a first scan categorizes from cache instead of
hundreds of rate-limited API requests. The app merges the seed into
tag_types.db on first run (INSERT OR IGNORE: user-fetched corrections
always win) and still fetches the long tail live.

Usage:
    scripts/seed_tagtypes.py [--tags 20000] [--out assets/tagtypes-seed.tsv]

Be polite: ~2 anonymous requests/second with a descriptive UA.
"""
import argparse
import json
import sys
import time
import urllib.parse
import urllib.request

API = "https://danbooru.donmai.us/tags.json"
UA = "box-seed/0.1 (one-time tag seed builder)"
PER_PAGE = 200  # API max per index page


def fetch_page(page, delay):
    qs = urllib.parse.urlencode(
        {"limit": PER_PAGE, "page": page, "search[order]": "count"})
    req = urllib.request.Request(API + "?" + qs, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            if r.status != 200:
                return None
            return json.loads(r.read().decode("utf-8"))
    except Exception as e:  # network hiccup: stop, keep what we have
        print(f"page {page}: {e}", file=sys.stderr)
        return None
    finally:
        time.sleep(delay)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tags", type=int, default=20000)
    ap.add_argument("--out", default="assets/tagtypes-seed.tsv")
    ap.add_argument("--delay", type=float, default=0.5)
    args = ap.parse_args()

    seen = {}
    page = 1
    while len(seen) < args.tags:
        rows = fetch_page(page, args.delay)
        if not rows:
            break
        for rec in rows:
            name = (rec.get("name") or "").strip().lower()
            try:
                cat = int(rec.get("category"))
            except (TypeError, ValueError):
                continue
            if name and name not in seen:
                seen[name] = cat
                if len(seen) >= args.tags:
                    break
        print(f"page {page}: {len(seen)} tags", file=sys.stderr)
        page += 1
        if page > 10000:  # sanity cap
            break

    with open(args.out, "w", encoding="utf-8") as f:
        for name in sorted(seen):
            f.write(f"{name}\t{seen[name]}\n")
    print(f"wrote {len(seen)} tags to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
