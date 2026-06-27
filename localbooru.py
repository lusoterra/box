#!/usr/bin/env python3
"""
LocalBooru — gallery-dl companion server
Drop next to (or inside) your gallery-dl folder and run:
    python localbooru.py

No pip required. Python 3.8+ only.

Scans recursively for <name>.ext + <name>.ext.json pairs.
Serves a booru-style browser at http://localhost:5000
"""

import argparse
import json
import mimetypes
import os
import re
import sys
import threading
import time
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

_HTML: bytes = b""

# ─────────────────────────────────────────────
#  Indexer
# ─────────────────────────────────────────────

MEDIA_EXTS = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif", ".mp4", ".webm"}


def scan_root(root: Path) -> list:
    posts = []
    root = root.resolve()
    print(f"[localbooru] Scanning {root} …")
    t0 = time.time()

    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        json_names = {f[:-5] for f in filenames if f.endswith(".json")}
        dirpath_p = Path(dirpath)
        rel_dir = dirpath_p.relative_to(root)
        parts = rel_dir.parts

        for fname in sorted(filenames):
            stem, ext = os.path.splitext(fname)
            if ext.lower() not in MEDIA_EXTS:
                continue
            rel_path = str(rel_dir / fname).replace("\\", "/")

            meta = None
            if fname in json_names:
                try:
                    with open(dirpath_p / (fname + ".json"), "r", encoding="utf-8") as f:
                        meta = json.load(f)
                except Exception:
                    pass

            if meta:
                post = {
                    "file": rel_path,
                    "ext": ext.lower().lstrip("."),
                    "id": meta.get("id", 0),
                    "score": meta.get("score", 0),
                    "rating": meta.get("rating", "general"),
                    "tags": meta.get("tags", ""),
                    "tags_artist": meta.get("tags_artist", ""),
                    "tags_character": meta.get("tags_character", ""),
                    "tags_copyright": meta.get("tags_copyright", ""),
                    "tags_metadata": meta.get("tags_metadata", ""),
                    "tags_general": meta.get("tags_general", ""),
                    "width": meta.get("width", 0),
                    "height": meta.get("height", 0),
                    "source": meta.get("source", ""),
                    "date": meta.get("date", ""),
                    "title": meta.get("title", ""),
                    "category": meta.get("category", parts[0] if parts else ""),
                    "has_meta": True,
                }
            else:
                post = {
                    "file": rel_path,
                    "ext": ext.lower().lstrip("."),
                    "id": 0, "score": 0, "rating": "general",
                    "tags": "", "tags_artist": "", "tags_character": "",
                    "tags_copyright": "", "tags_metadata": "", "tags_general": "",
                    "width": 0, "height": 0, "source": "", "date": "", "title": "",
                    "category": parts[0] if parts else "",
                    "has_meta": False,
                }
            posts.append(post)

    print(f"[localbooru] Found {len(posts)} posts in {time.time()-t0:.1f}s")
    return posts


class Index:
    def __init__(self, root: Path):
        self.root = root
        self.posts: list = []
        self.tag_counts: dict = {"artist": {}, "character": {}, "copyright": {}, "general": {}, "meta": {}}
        self.categories: dict = {}
        self.lock = threading.RLock()
        self._build()

    def _build(self):
        posts = scan_root(self.root)
        tc: dict = {"artist": {}, "character": {}, "copyright": {}, "general": {}, "meta": {}}
        cats: dict = {}

        for p in posts:
            def add(field, bucket, _p=p, _tc=tc):
                for t in _p[field].split():
                    if t: _tc[bucket][t] = _tc[bucket].get(t, 0) + 1
            add("tags_artist", "artist"); add("tags_character", "character")
            add("tags_copyright", "copyright"); add("tags_metadata", "meta")
            add("tags_general", "general")
            cat = p["category"]
            if cat: cats[cat] = cats.get(cat, 0) + 1

        with self.lock:
            self.posts = posts
            self.tag_counts = tc
            self.categories = cats

    def refresh(self):
        print("[localbooru] Re-scanning…")
        self._build()
        print("[localbooru] Re-scan done.")

    def query(self, tags, rating, category, sort, page, per_page):
        with self.lock:
            posts = self.posts

        inc = [t for t in tags if not t.startswith("-")]
        exc = [t[1:] for t in tags if t.startswith("-")]

        results = []
        for p in posts:
            if rating != "all" and p["rating"] != rating: continue
            if category and p["category"] != category: continue
            if inc or exc:
                pt = set(p["tags"].split())
                if not all(t in pt for t in inc): continue
                if any(t in pt for t in exc): continue
            results.append(p)

        if sort == "id-asc": results.sort(key=lambda p: p["id"])
        elif sort == "score-desc": results.sort(key=lambda p: p["score"], reverse=True)
        elif sort == "score-asc": results.sort(key=lambda p: p["score"])
        elif sort == "random":
            import random; random.shuffle(results)
        else: results.sort(key=lambda p: p["id"], reverse=True)

        total = len(results)
        start = (page - 1) * per_page
        return {"total": total, "page": page, "per_page": per_page,
                "posts": results[start:start + per_page]}

    def tag_suggestions(self, prefix, limit=15):
        prefix = prefix.lower()
        combined: dict = {}
        with self.lock:
            for bucket in self.tag_counts.values():
                for tag, count in bucket.items():
                    if prefix in tag:
                        combined[tag] = combined.get(tag, 0) + count
        return [{"tag": t, "count": c} for t, c in sorted(combined.items(), key=lambda x: -x[1])[:limit]]

    def stats(self):
        with self.lock:
            ratings: dict = {}
            for p in self.posts:
                r = p["rating"]; ratings[r] = ratings.get(r, 0) + 1
            top = lambda d, n: sorted(d.items(), key=lambda x: -x[1])[:n]
            return {
                "total": len(self.posts), "ratings": ratings,
                "categories": self.categories,
                "top_artists": top(self.tag_counts["artist"], 30),
                "top_characters": top(self.tag_counts["character"], 30),
                "top_copyrights": top(self.tag_counts["copyright"], 30),
                "top_general": top(self.tag_counts["general"], 50),
            }


# ─────────────────────────────────────────────
#  HTTP Server
# ─────────────────────────────────────────────

CHUNK = 1 << 20


class Handler(BaseHTTPRequestHandler):
    index: "Index"
    root: Path

    def log_message(self, fmt, *args): pass

    def send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def serve_file(self, rel: str):
        try:
            path = (self.root / rel).resolve()
            path.relative_to(self.root)
        except Exception:
            self.send_error(403); return
        if not path.is_file():
            self.send_error(404); return

        size = path.stat().st_size
        mime = mimetypes.guess_type(str(path))[0] or "application/octet-stream"
        range_header = self.headers.get("Range")
        if range_header:
            m = re.match(r"bytes=(\d+)-(\d*)", range_header)
            if m:
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else size - 1
                end = min(end, size - 1)
                length = end - start + 1
                self.send_response(206)
                self.send_header("Content-Type", mime)
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                self.send_header("Content-Length", str(length))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                with open(path, "rb") as f:
                    f.seek(start); remaining = length
                    while remaining:
                        chunk = f.read(min(CHUNK, remaining))
                        if not chunk: break
                        self.wfile.write(chunk); remaining -= len(chunk)
                return

        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(size))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cache-Control", "public, max-age=86400")
        self.end_headers()
        with open(path, "rb") as f:
            while True:
                chunk = f.read(CHUNK)
                if not chunk: break
                self.wfile.write(chunk)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)
        def qp(k, d=""): return qs.get(k, [d])[0]

        if path == "/api/stats":
            self.send_json(self.index.stats())
        elif path == "/api/posts":
            tags_raw = qp("tags")
            tags = [t for t in tags_raw.split() if t] if tags_raw else []
            self.send_json(self.index.query(
                tags=tags, rating=qp("rating", "all"), category=qp("category", ""),
                sort=qp("sort", "id-desc"), page=int(qp("page", "1")),
                per_page=min(int(qp("per_page", "40")), 500),
            ))
        elif path == "/api/suggest":
            self.send_json(self.index.tag_suggestions(qp("q")))
        elif path == "/api/refresh":
            threading.Thread(target=self.index.refresh, daemon=True).start()
            self.send_json({"status": "refreshing"})
        elif path.startswith("/media/"):
            self.serve_file(urllib.parse.unquote(path[7:]))
        elif path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(_HTML)))
            self.end_headers()
            self.wfile.write(_HTML)
        else:
            self.send_error(404)


# ─────────────────────────────────────────────
#  Embedded HTML
# ─────────────────────────────────────────────

_HTML_SOURCE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>LocalBooru</title>
<style>
:root{--bg:#0d0f14;--surface:#161920;--surface2:#1e2130;--border:#2a2f45;--accent:#7c9ef8;--accent2:#a78bfa;--text:#d4d8f0;--text-dim:#7880a0;--text-muted:#4a5068;--radius:6px;--font:'Segoe UI',system-ui,sans-serif}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:var(--font);font-size:13px;min-height:100vh;display:flex;flex-direction:column}

/* Header */
#header{background:var(--surface);border-bottom:1px solid var(--border);padding:0 14px;display:flex;align-items:center;gap:12px;height:48px;position:sticky;top:0;z-index:100;flex-shrink:0}
#logo{font-size:18px;font-weight:700;letter-spacing:-.5px;color:var(--accent);cursor:pointer;white-space:nowrap;user-select:none}
#logo span{color:var(--accent2)}
#search-wrap{flex:1;position:relative;display:flex;max-width:680px}
#search-input{flex:1;background:var(--bg);border:1px solid var(--border);border-radius:var(--radius);color:var(--text);padding:6px 10px;font-size:13px;font-family:var(--font);outline:none;transition:border-color .15s}
#search-input:focus{border-color:var(--accent)}
#search-input::placeholder{color:var(--text-muted)}
#suggestions{position:absolute;top:100%;left:0;right:0;background:var(--surface2);border:1px solid var(--border);border-top:none;border-radius:0 0 var(--radius) var(--radius);z-index:50;max-height:200px;overflow-y:auto;display:none}
.sug-item{padding:5px 10px;cursor:pointer;display:flex;justify-content:space-between;font-size:12px}
.sug-item:hover{background:var(--border)}
.sug-count{color:var(--text-muted);font-size:11px}
.btn{background:var(--surface2);border:1px solid var(--border);border-radius:var(--radius);color:var(--text);padding:5px 11px;font-size:12px;font-family:var(--font);cursor:pointer;white-space:nowrap;transition:background .15s;display:flex;align-items:center;gap:4px}
.btn:hover{background:var(--border)}
.btn.primary{background:var(--accent);border-color:var(--accent);color:#0d0f14;font-weight:600}
.btn.primary:hover{opacity:.9}
#hdr-stat{color:var(--text-muted);font-size:11px;white-space:nowrap}

/* Layout */
#app{display:flex;flex:1;min-height:0;overflow:hidden}

/* Sidebar */
#sidebar{width:210px;flex-shrink:0;background:var(--surface);border-right:1px solid var(--border);overflow-y:auto;padding:10px 6px;display:flex;flex-direction:column;gap:14px}
.sb-section h3{font-size:10px;font-weight:600;letter-spacing:1px;text-transform:uppercase;color:var(--text-muted);padding:0 4px 5px;border-bottom:1px solid var(--border);margin-bottom:5px}
.filter-btn{background:none;border:none;color:var(--text-dim);padding:3px 6px;border-radius:4px;cursor:pointer;text-align:left;font-size:12px;font-family:var(--font);transition:background .1s,color .1s;display:flex;justify-content:space-between;align-items:center;width:100%}
.filter-btn:hover{background:var(--surface2);color:var(--text)}
.filter-btn.active{background:var(--surface2);color:var(--accent)}
.filter-btn .fc{color:var(--text-muted);font-size:11px}
.tag-cloud{display:flex;flex-direction:column;gap:2px}
.sb-tag{display:flex;justify-content:space-between;align-items:center;padding:3px 6px;border-radius:3px;cursor:pointer;font-size:11px;transition:background .1s;gap:4px}
.sb-tag:hover{background:var(--surface2)}
.sb-tag.active{background:var(--surface2);outline:1px solid var(--accent)}
.sb-tag .tn{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sb-tag .tc{color:var(--text-muted);font-size:10px;flex-shrink:0}
.t-artist .tn{color:#d4955a}.t-character .tn{color:#60b880}.t-copyright .tn{color:#b570e0}.t-general .tn{color:#7aaee0}.t-meta .tn{color:#88aa88}
.cat-btn{background:none;border:none;color:var(--text-dim);padding:3px 6px;border-radius:3px;cursor:pointer;text-align:left;font-size:11px;font-family:var(--font);display:flex;justify-content:space-between;width:100%;transition:background .1s}
.cat-btn:hover{background:var(--surface2);color:var(--text)}
.cat-btn.active{background:var(--surface2);color:var(--accent)}

/* Main */
#main{flex:1;min-width:0;display:flex;flex-direction:column;overflow:hidden}
#toolbar{background:var(--surface);border-bottom:1px solid var(--border);padding:6px 14px;display:flex;align-items:center;gap:10px;flex-shrink:0;flex-wrap:wrap;min-height:38px}
#active-tags{display:flex;flex-wrap:wrap;gap:4px;flex:1}
#result-info{color:var(--text-muted);font-size:12px;white-space:nowrap}
.tag-pill{background:rgba(124,158,248,.15);border:1px solid rgba(124,158,248,.4);border-radius:4px;padding:2px 7px;font-size:11px;color:var(--accent);display:flex;align-items:center;gap:4px;cursor:default}
.tag-pill .rm{cursor:pointer;color:var(--text-muted);font-size:14px;line-height:1}
.tag-pill .rm:hover{color:#e05555}
.sort-grp{display:flex;align-items:center;gap:5px;font-size:12px;color:var(--text-dim);white-space:nowrap}
select{background:var(--bg);border:1px solid var(--border);border-radius:var(--radius);color:var(--text);padding:3px 7px;font-size:12px;font-family:var(--font);cursor:pointer;outline:none}
select:focus{border-color:var(--accent)}

/* Virtual grid scroller */
#grid-scroller{flex:1;overflow-y:scroll;overflow-x:hidden;position:relative;contain:strict}
#grid-sizer{position:relative;width:100%}  /* height set by JS */
#grid{position:absolute;top:0;left:0;right:0;display:grid;padding:10px;gap:7px;
  grid-template-columns:repeat(auto-fill,minmax(175px,1fr))}

/* Thumb */
.thumb{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);overflow:hidden;cursor:pointer;transition:border-color .15s,transform .1s;position:relative;contain:layout paint}
.thumb:hover{border-color:var(--accent);transform:translateY(-1px)}
.thumb-img-wrap{width:100%;padding-top:75%;position:relative;background:var(--surface2);overflow:hidden}
.thumb-img-wrap img,.thumb-img-wrap video{position:absolute;inset:0;width:100%;height:100%;object-fit:cover}
.thumb-img-wrap img{opacity:0;transition:opacity .2s}
.thumb-img-wrap img.loaded{opacity:1}
.thumb-rating{position:absolute;top:4px;left:4px;font-size:9px;font-weight:700;padding:1px 4px;border-radius:3px;text-transform:uppercase;letter-spacing:.5px}
.r-general{background:#1a4a1a;color:#6fcc6f}.r-sensitive{background:#4a3a1a;color:#ccaa6f}
.r-questionable{background:#4a2a1a;color:#cc8060}.r-explicit{background:#4a1a1a;color:#cc5555}
.thumb-type{position:absolute;top:4px;right:4px;font-size:9px;padding:1px 4px;border-radius:3px;background:rgba(0,0,0,.7);color:var(--accent);font-weight:600}
.thumb-score{position:absolute;bottom:4px;right:4px;font-size:10px;padding:1px 5px;border-radius:3px;background:rgba(0,0,0,.7);color:var(--text)}
.thumb-score.pos{color:#6fcc6f}.thumb-score.neg{color:#e05555}
.thumb-info{padding:5px 7px;font-size:10px;color:var(--text-muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}

#empty{display:none;flex-direction:column;align-items:center;justify-content:center;gap:10px;color:var(--text-muted);padding:80px 20px;text-align:center;position:absolute;inset:0}
#empty .ico{font-size:48px}

/* Pagination */
#pagination{background:var(--surface);border-top:1px solid var(--border);padding:7px 14px;display:flex;align-items:center;justify-content:center;gap:4px;flex-shrink:0;flex-wrap:wrap}
.page-btn{background:var(--surface2);border:1px solid var(--border);border-radius:4px;color:var(--text-dim);padding:4px 9px;font-size:12px;cursor:pointer;min-width:30px;text-align:center;transition:background .1s;font-family:var(--font)}
.page-btn:hover{background:var(--border);color:var(--text)}
.page-btn.active{background:var(--accent);border-color:var(--accent);color:#0d0f14;font-weight:700}
.page-btn:disabled{opacity:.3;cursor:not-allowed}

/* Lightbox */
#lightbox{position:fixed;inset:0;background:rgba(0,0,0,.92);z-index:300;display:none;flex-direction:row}
#lb-media{flex:1;display:flex;align-items:center;justify-content:center;min-width:0;padding:16px;position:relative}
#lb-media img,#lb-media video{max-width:100%;max-height:100%;object-fit:contain;border-radius:4px}
.lb-nav{position:absolute;top:50%;transform:translateY(-50%);background:rgba(0,0,0,.5);border:1px solid var(--border);border-radius:50%;width:38px;height:38px;font-size:18px;color:var(--text);cursor:pointer;display:flex;align-items:center;justify-content:center;transition:background .15s;z-index:2}
.lb-nav:hover{background:rgba(0,0,0,.85)}
#lb-prev{left:8px}#lb-next{right:8px}
#lb-panel{width:290px;flex-shrink:0;background:var(--surface);border-left:1px solid var(--border);overflow-y:auto;display:flex;flex-direction:column}
#lb-close{position:absolute;top:10px;right:300px;background:rgba(0,0,0,.6);border:1px solid var(--border);border-radius:50%;width:30px;height:30px;font-size:14px;color:var(--text);cursor:pointer;display:flex;align-items:center;justify-content:center;z-index:10}
#lb-close:hover{background:#e05555}
#lb-hdr{padding:12px;border-bottom:1px solid var(--border)}
#lb-id-txt{font-size:11px;color:var(--text-muted)}
#lb-title-txt{font-size:14px;font-weight:600;color:var(--text);margin-top:3px}
#lb-date-txt{font-size:11px;color:var(--text-muted);margin-top:2px}
#lb-path-txt{font-size:10px;color:var(--text-muted);margin-top:4px;word-break:break-all}
#lb-meta-grid{padding:8px 12px;border-bottom:1px solid var(--border);display:grid;grid-template-columns:1fr 1fr;gap:5px}
.lmk{display:flex;flex-direction:column;gap:1px}
.lml{font-size:9px;text-transform:uppercase;letter-spacing:.8px;color:var(--text-muted)}
.lmv{font-size:12px;color:var(--text)}
#lb-tags-sec{padding:8px 12px;flex:1}
#lb-tags-sec h4{font-size:10px;text-transform:uppercase;letter-spacing:.8px;color:var(--text-muted);margin-bottom:7px}
.lb-ts{margin-bottom:8px}.lb-ts-lbl{font-size:10px;color:var(--text-muted);margin-bottom:3px}
.lb-tl{display:flex;flex-wrap:wrap;gap:3px}
.lb-tag{font-size:11px;padding:2px 6px;border-radius:3px;cursor:pointer;border:1px solid transparent;transition:opacity .15s}
.lb-tag:hover{opacity:.75}
.lb-tag.ta{background:rgba(196,124,58,.2);border-color:rgba(196,124,58,.4);color:#d4955a}
.lb-tag.tc{background:rgba(58,138,92,.2);border-color:rgba(58,138,92,.4);color:#60b880}
.lb-tag.tk{background:rgba(138,58,181,.2);border-color:rgba(138,58,181,.4);color:#b570e0}
.lb-tag.tg{background:rgba(74,122,181,.2);border-color:rgba(74,122,181,.4);color:#7aaee0}
.lb-tag.tm{background:rgba(90,122,90,.2);border-color:rgba(90,122,90,.4);color:#88aa88}
#lb-src{padding:0 12px 8px;font-size:10px;color:var(--text-muted);word-break:break-all}
#lb-src a{color:var(--accent);text-decoration:none}
#lb-src a:hover{text-decoration:underline}
#lb-actions{padding:8px 12px;border-top:1px solid var(--border);display:flex;flex-direction:column;gap:5px}

/* Loading */
#loading{position:fixed;inset:0;background:var(--bg);display:flex;flex-direction:column;align-items:center;justify-content:center;gap:14px;z-index:400}
.spinner{width:38px;height:38px;border:3px solid var(--border);border-top-color:var(--accent);border-radius:50%;animation:spin .7s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
#load-txt{color:var(--text-dim);font-size:14px}
::-webkit-scrollbar{width:5px;height:5px}::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:var(--border);border-radius:3px}::-webkit-scrollbar-thumb:hover{background:var(--text-muted)}
</style>
</head>
<body>

<div id="loading"><div class="spinner"></div><div id="load-txt">Loading index…</div></div>

<header id="header" style="display:none">
  <span id="logo" onclick="goHome()">Local<span>Booru</span></span>
  <div id="search-wrap">
    <input type="text" id="search-input" placeholder="Search tags… prefix - to exclude" autocomplete="off" spellcheck="false">
    <div id="suggestions"></div>
  </div>
  <button class="btn primary" onclick="doSearch()">Search</button>
  <button class="btn" onclick="clearSearch()">Clear</button>
  <button class="btn" onclick="refreshIndex()" title="Re-scan folder">↺</button>
  <span id="hdr-stat"></span>
</header>

<div id="app" style="display:none">
  <aside id="sidebar">
    <div class="sb-section">
      <h3>Rating</h3>
      <button class="filter-btn active" data-r="all" onclick="setRating('all')">All <span class="fc" id="rc-all"></span></button>
      <button class="filter-btn" data-r="general" onclick="setRating('general')">General <span class="fc" id="rc-general"></span></button>
      <button class="filter-btn" data-r="sensitive" onclick="setRating('sensitive')">Sensitive <span class="fc" id="rc-sensitive"></span></button>
      <button class="filter-btn" data-r="questionable" onclick="setRating('questionable')">Questionable <span class="fc" id="rc-questionable"></span></button>
      <button class="filter-btn" data-r="explicit" onclick="setRating('explicit')">Explicit <span class="fc" id="rc-explicit"></span></button>
    </div>
    <div class="sb-section">
      <h3>Sources</h3>
      <div id="sb-categories"></div>
    </div>
    <div class="sb-section"><h3>Top Artists</h3><div class="tag-cloud" id="sb-artists"></div></div>
    <div class="sb-section"><h3>Top Characters</h3><div class="tag-cloud" id="sb-chars"></div></div>
    <div class="sb-section"><h3>Top Copyrights</h3><div class="tag-cloud" id="sb-copyrights"></div></div>
    <div class="sb-section"><h3>Top Tags</h3><div class="tag-cloud" id="sb-tags"></div></div>
  </aside>

  <main id="main">
    <div id="toolbar">
      <div id="active-tags"></div>
      <span id="result-info"></span>
      <div class="sort-grp">
        Sort:
        <select id="sort-sel" onchange="setSort(this.value)">
          <option value="id-desc">Newest</option>
          <option value="id-asc">Oldest</option>
          <option value="score-desc">Top Rated</option>
          <option value="score-asc">Low Rated</option>
          <option value="random">Random</option>
        </select>
        Per page:
        <select id="pp-sel" onchange="setPerPage(+this.value)">
          <option value="40" selected>40</option>
          <option value="60">60</option>
          <option value="100">100</option>
          <option value="200">200</option>
        </select>
      </div>
    </div>

    <div id="grid-scroller">
      <div id="grid-sizer">
        <div id="grid"></div>
        <div id="empty"><div class="ico">🔍</div><h3>No results</h3><p>Try different tags or clear filters</p></div>
      </div>
    </div>

    <div id="pagination"></div>
  </main>
</div>

<!-- Lightbox -->
<div id="lightbox">
  <div id="lb-media">
    <button class="lb-nav" id="lb-prev" onclick="lbNav(-1)">‹</button>
    <img id="lb-img" style="display:none" alt="">
    <video id="lb-video" style="display:none" controls loop></video>
    <button class="lb-nav" id="lb-next" onclick="lbNav(1)">›</button>
  </div>
  <button id="lb-close" onclick="closeLb()">✕</button>
  <div id="lb-panel">
    <div id="lb-hdr">
      <div id="lb-id-txt"></div><div id="lb-title-txt"></div>
      <div id="lb-date-txt"></div><div id="lb-path-txt"></div>
    </div>
    <div id="lb-meta-grid">
      <div class="lmk"><div class="lml">Score</div><div class="lmv" id="lb-score"></div></div>
      <div class="lmk"><div class="lml">Rating</div><div class="lmv" id="lb-rating"></div></div>
      <div class="lmk"><div class="lml">Size</div><div class="lmv" id="lb-size"></div></div>
      <div class="lmk"><div class="lml">Type</div><div class="lmv" id="lb-ext"></div></div>
    </div>
    <div id="lb-tags-sec"><h4>Tags</h4></div>
    <div id="lb-src"></div>
    <div id="lb-actions">
      <a id="lb-dl" class="btn" style="text-decoration:none" download>⬇ Download</a>
      <a id="lb-open" class="btn" style="text-decoration:none" target="_blank">🔗 Open in tab</a>
    </div>
  </div>
</div>

<script>
// ── State ──
let stats = {};
let posts = [];          // current page's posts from server
let totalPosts = 0;
let currentPage = 1;
let perPage = 40;
let currentSort = 'id-desc';
let currentRating = 'all';
let activeTags = [];
let activeCategory = '';
let searchQuery = '';
let lbIdx = 0;

// ── Virtual grid state ──
const THUMB_ASPECT = 75 / 100;  // padding-top 75% → h/w ratio
const THUMB_MIN_W = 175;
const THUMB_INFO_H = 27;        // .thumb-info height
const GRID_GAP = 7;
const GRID_PAD = 10;
const OVERSCAN = 3;             // extra rows to render above/below viewport

let colCount = 1;
let thumbW = THUMB_MIN_W;
let thumbH = THUMB_MIN_W * THUMB_ASPECT + THUMB_INFO_H;
let rowH = thumbH + GRID_GAP;
let visibleStart = 0;   // first rendered post index
let visibleEnd = 0;     // last rendered post index (exclusive)
let rafPending = false;

const scroller = document.getElementById('grid-scroller');
const sizer    = document.getElementById('grid-sizer');
const grid     = document.getElementById('grid');

// ── Boot ──
async function boot() {
  const s = await api('/api/stats');
  stats = s;
  document.getElementById('hdr-stat').textContent = `${s.total.toLocaleString()} posts`;
  fillRatingCounts(s.ratings);
  buildSidebar(s);
  document.getElementById('loading').style.display = 'none';
  document.getElementById('header').style.display = 'flex';
  document.getElementById('app').style.display = 'flex';

  measureGrid();
  window.addEventListener('resize', () => { measureGrid(); scheduleRender(); });
  scroller.addEventListener('scroll', onScroll, {passive: true});

  await fetchAndRender();
}

async function api(url) {
  const r = await fetch(url);
  return r.json();
}

// ── Measure grid geometry from real DOM ──
function measureGrid() {
  const scrollerW = scroller.clientWidth - GRID_PAD * 2;
  colCount = Math.max(1, Math.floor((scrollerW + GRID_GAP) / (THUMB_MIN_W + GRID_GAP)));
  thumbW = (scrollerW - (colCount - 1) * GRID_GAP) / colCount;
  thumbH = thumbW * THUMB_ASPECT + THUMB_INFO_H;
  rowH   = thumbH + GRID_GAP;
  grid.style.gridTemplateColumns = `repeat(${colCount}, 1fr)`;
}

// ── Scroll handler — throttled via rAF ──
function onScroll() {
  if (!rafPending) {
    rafPending = true;
    requestAnimationFrame(() => { rafPending = false; scheduleRender(); });
  }
}

function scheduleRender() {
  updateVirtualWindow();
}

// ── Compute which rows are visible and patch the DOM ──
function updateVirtualWindow() {
  if (!posts.length) return;

  const totalRows = Math.ceil(posts.length / colCount);
  const sizerH = totalRows * rowH - GRID_GAP + GRID_PAD * 2;
  sizer.style.height = sizerH + 'px';

  const scrollTop = scroller.scrollTop;
  const viewH     = scroller.clientHeight;

  const firstVisibleRow = Math.max(0, Math.floor((scrollTop - GRID_PAD) / rowH) - OVERSCAN);
  const lastVisibleRow  = Math.min(totalRows - 1,
      Math.ceil((scrollTop + viewH - GRID_PAD) / rowH) + OVERSCAN);

  const newStart = firstVisibleRow * colCount;
  const newEnd   = Math.min(posts.length, (lastVisibleRow + 1) * colCount);

  if (newStart === visibleStart && newEnd === visibleEnd) return;
  visibleStart = newStart;
  visibleEnd   = newEnd;

  // Grid is positioned at the start of the first visible row
  const gridTop = GRID_PAD + firstVisibleRow * rowH;
  grid.style.top = gridTop + 'px';

  renderVisibleThumbs();
}

// ── Render only the visible slice ──
function renderVisibleThumbs() {
  const slice = posts.slice(visibleStart, visibleEnd);
  grid.innerHTML = slice.map((p, i) => thumbHTML(p, visibleStart + i)).join('');

  // Trigger image fade-in after browser paints
  requestAnimationFrame(() => {
    grid.querySelectorAll('img[data-src]').forEach(img => {
      const src = img.dataset.src;
      img.onload = () => img.classList.add('loaded');
      img.onerror = () => img.classList.add('loaded');  // still show placeholder
      img.src = src;
      delete img.dataset.src;
    });
  });
}

function thumbHTML(p, globalIdx) {
  const ext = p.ext;
  const isVid = /mp4|webm/.test(ext);
  const isGif = ext === 'gif';
  const url = '/media/' + encodeURIFilepath(p.file);
  const score = p.score;
  const rS = (p.rating || 'general')[0].toUpperCase();
  const scoreCls = score > 0 ? 'pos' : score < 0 ? 'neg' : '';
  const infoText = p.id ? `#${p.id}` : p.file.split('/').pop();
  const typeBadge = isVid ? '<span class="thumb-type">▶</span>' : isGif ? '<span class="thumb-type">GIF</span>' : '';

  // Videos: use src directly (browser won't load until visible anyway)
  // Images: use data-src, set src after paint
  const media = isVid
    ? `<video src="${url}#t=0.1" muted preload="none"></video>`
    : `<img data-src="${url}" alt="">`;

  return `<div class="thumb" onclick="openLb(${globalIdx})">
    <div class="thumb-img-wrap">
      ${media}
      <span class="thumb-rating r-${p.rating||'general'}">${rS}</span>
      ${typeBadge}
      <span class="thumb-score ${scoreCls}">${score>0?'+':''}${score}</span>
    </div>
    <div class="thumb-info">${infoText}</div>
  </div>`;
}

// ── Fetch from server & reset virtual grid ──
async function fetchAndRender() {
  const params = new URLSearchParams({
    tags: [...activeTags, ...searchQuery.trim().split(/\s+/).filter(Boolean)].join(' '),
    rating: currentRating,
    category: activeCategory,
    sort: currentSort,
    page: currentPage,
    per_page: perPage,
  });

  document.getElementById('result-info').textContent = 'Loading…';
  const data = await api('/api/posts?' + params);
  posts = data.posts;
  totalPosts = data.total;

  const empty = document.getElementById('empty');
  if (!posts.length) {
    grid.innerHTML = '';
    sizer.style.height = '200px';
    empty.style.display = 'flex';
    document.getElementById('result-info').textContent = 'No results';
  } else {
    empty.style.display = 'none';
    scroller.scrollTop = 0;
    visibleStart = visibleEnd = 0;
    measureGrid();
    updateVirtualWindow();
  }

  renderPagination(data.total);
  updateResultInfo(data.total, data.page);
  updateActiveTags();
}

function renderPagination(total) {
  const totalPages = Math.ceil(total / perPage);
  const pag = document.getElementById('pagination');
  if (totalPages <= 1) { pag.innerHTML = ''; return; }

  let pages = [];
  if (totalPages <= 9) {
    pages = Array.from({length: totalPages}, (_,i) => i+1);
  } else {
    let s = Math.max(1, currentPage - 4), e = Math.min(totalPages, s + 8);
    if (e - s < 8) s = Math.max(1, e - 8);
    pages = Array.from({length: e-s+1}, (_,i) => s+i);
    if (pages[0] > 1) pages = [1, '…', ...pages.slice(1)];
    if (pages[pages.length-1] < totalPages) pages = [...pages.slice(0,-1), '…', totalPages];
  }
  pag.innerHTML =
    `<button class="page-btn" onclick="goPage(${currentPage-1})" ${currentPage===1?'disabled':''}>‹</button>` +
    pages.map(p => p==='…'
      ? `<span class="page-btn" style="cursor:default;opacity:.4">…</span>`
      : `<button class="page-btn ${p===currentPage?'active':''}" onclick="goPage(${p})">${p}</button>`
    ).join('') +
    `<button class="page-btn" onclick="goPage(${currentPage+1})" ${currentPage===totalPages?'disabled':''}>›</button>`;
}

function updateResultInfo(total, page) {
  const start = (page-1)*perPage+1, end = Math.min(page*perPage, total);
  document.getElementById('result-info').textContent =
    total===0 ? 'No results' : `${start}–${end} of ${total.toLocaleString()}`;
}

function updateActiveTags() {
  document.getElementById('active-tags').innerHTML = activeTags.map(t =>
    `<span class="tag-pill">${t}<span class="rm" onclick="removeTag('${esc(t)}')">×</span></span>`
  ).join('');
}

// ── Sidebar ──
function fillRatingCounts(r) {
  const total = Object.values(r).reduce((a,b)=>a+b,0);
  document.getElementById('rc-all').textContent = total.toLocaleString();
  for (const [k,v] of Object.entries(r)) { const el=document.getElementById('rc-'+k); if(el) el.textContent=v.toLocaleString(); }
}

function buildSidebar(s) {
  const catEl = document.getElementById('sb-categories');
  const cats = Object.entries(s.categories||{}).sort((a,b)=>b[1]-a[1]);
  catEl.innerHTML = [['','All',s.total],...cats.map(([k,v])=>[k,k,v])].map(([val,label,cnt])=>
    `<button class="cat-btn ${activeCategory===val?'active':''}" onclick="setCategory('${esc(val)}')">
      <span>${label||'(unknown)'}</span><span style="color:var(--text-muted);font-size:10px">${cnt}</span>
     </button>`).join('');

  const render = (id, entries, type) => {
    document.getElementById(id).innerHTML = entries.slice(0,30).map(([tag,cnt])=>
      `<div class="sb-tag t-${type} ${activeTags.includes(tag)?'active':''}" onclick="toggleTag('${esc(tag)}')">
        <span class="tn">${tag}</span><span class="tc">${cnt}</span>
       </div>`).join('');
  };
  render('sb-artists', s.top_artists, 'artist');
  render('sb-chars', s.top_characters, 'character');
  render('sb-copyrights', s.top_copyrights, 'copyright');
  render('sb-tags', s.top_general, 'general');
}

// ── Controls ──
function goPage(n) { currentPage=n; fetchAndRender(); }
function setRating(r) {
  currentRating=r;
  document.querySelectorAll('[data-r]').forEach(b=>b.classList.toggle('active',b.dataset.r===r));
  currentPage=1; fetchAndRender();
}
function setSort(s) { currentSort=s; currentPage=1; fetchAndRender(); }
function setPerPage(n) { perPage=n; currentPage=1; fetchAndRender(); }
function setCategory(c) {
  activeCategory=c; currentPage=1;
  document.querySelectorAll('.cat-btn').forEach(b=>b.classList.toggle('active',
    (b.querySelector('span').textContent===(c||'All'))));
  fetchAndRender();
}
function toggleTag(tag) {
  activeTags=activeTags.includes(tag)?activeTags.filter(t=>t!==tag):[...activeTags,tag];
  currentPage=1; buildSidebar(stats); fetchAndRender();
}
function removeTag(tag) { toggleTag(tag); }
function goHome() {
  activeTags=[]; searchQuery=''; currentRating='all'; currentSort='id-desc';
  activeCategory=''; currentPage=1;
  document.getElementById('search-input').value='';
  document.getElementById('sort-sel').value='id-desc';
  document.querySelectorAll('[data-r]').forEach(b=>b.classList.toggle('active',b.dataset.r==='all'));
  buildSidebar(stats); fetchAndRender();
}

// ── Search ──
const searchInput = document.getElementById('search-input');
const sugg = document.getElementById('suggestions');
let sugTimer;

searchInput.addEventListener('input', () => {
  clearTimeout(sugTimer);
  sugTimer = setTimeout(async () => {
    const lastWord = searchInput.value.split(/\s+/).pop().replace(/^-/,'');
    if (lastWord.length < 2) { sugg.style.display='none'; return; }
    const results = await api('/api/suggest?q='+encodeURIComponent(lastWord));
    if (!results.length) { sugg.style.display='none'; return; }
    sugg.style.display = 'block';
    sugg.innerHTML = results.map(r=>
      `<div class="sug-item" onmousedown="selectSug('${esc(r.tag)}')">
        <span>${r.tag}</span><span class="sug-count">${r.count}</span></div>`).join('');
  }, 150);
});
searchInput.addEventListener('blur', ()=>setTimeout(()=>{sugg.style.display='none'},200));
searchInput.addEventListener('keydown', e=>{ if(e.key==='Enter') doSearch(); });

function selectSug(tag) {
  const parts = searchInput.value.split(/\s+/);
  const prefix = parts[parts.length-1].startsWith('-')?'-':'';
  parts[parts.length-1] = prefix+tag;
  searchInput.value = parts.join(' ')+' ';
  sugg.style.display='none'; searchInput.focus();
}
function doSearch() { searchQuery=searchInput.value; sugg.style.display='none'; currentPage=1; fetchAndRender(); }
function clearSearch() { searchInput.value=''; searchQuery=''; currentPage=1; fetchAndRender(); }

async function refreshIndex() {
  document.getElementById('hdr-stat').textContent='Re-scanning…';
  await api('/api/refresh');
  await new Promise(r=>setTimeout(r,1500));
  const s = await api('/api/stats');
  stats=s;
  document.getElementById('hdr-stat').textContent=`${s.total.toLocaleString()} posts`;
  fillRatingCounts(s.ratings); buildSidebar(s); fetchAndRender();
}

// ── Lightbox ──
function openLb(i) {
  lbIdx=i; showLbPost(posts[i]);
  document.getElementById('lightbox').style.display='flex';
  document.addEventListener('keydown', lbKey);
}
function closeLb() {
  document.getElementById('lightbox').style.display='none';
  const v=document.getElementById('lb-video'); v.pause(); v.src='';
  document.getElementById('lb-img').src='';
  document.removeEventListener('keydown', lbKey);
}
function lbKey(e) {
  if(e.key==='Escape') closeLb();
  else if(e.key==='ArrowRight') lbNav(1);
  else if(e.key==='ArrowLeft') lbNav(-1);
}
function lbNav(d) {
  const n=lbIdx+d;
  if(n<0||n>=posts.length) return;
  lbIdx=n; showLbPost(posts[n]);
}
function showLbPost(p) {
  const url='/media/'+encodeURIFilepath(p.file);
  const isVid=/mp4|webm/.test(p.ext);
  const img=document.getElementById('lb-img'), vid=document.getElementById('lb-video');
  if(isVid){ img.style.display='none'; vid.style.display='block'; vid.src=url; vid.play(); }
  else{ vid.pause(); vid.src=''; vid.style.display='none'; img.style.display='block'; img.src=url; }

  document.getElementById('lb-id-txt').textContent=p.id?`#${p.id}`:'';
  document.getElementById('lb-title-txt').textContent=p.title||'';
  document.getElementById('lb-date-txt').textContent=p.date?new Date(p.date).toLocaleDateString('en-US',{year:'numeric',month:'short',day:'numeric'}):'';
  document.getElementById('lb-path-txt').textContent=p.file;
  document.getElementById('lb-score').textContent=p.has_meta?((p.score>0?'+':'')+p.score):'—';
  document.getElementById('lb-rating').textContent=p.rating||'—';
  document.getElementById('lb-size').textContent=(p.width&&p.height)?`${p.width}×${p.height}`:'—';
  document.getElementById('lb-ext').textContent=p.ext.toUpperCase();
  document.getElementById('lb-src').innerHTML=p.source?`<a href="${p.source}" target="_blank" rel="noopener">${p.source}</a>`:'';
  document.getElementById('lb-dl').href=url;
  document.getElementById('lb-dl').download=p.file.split('/').pop();
  document.getElementById('lb-open').href=url;

  const sec=document.getElementById('lb-tags-sec');
  const secs=[{l:'Artist',f:'tags_artist',c:'ta'},{l:'Copyright',f:'tags_copyright',c:'tk'},
    {l:'Character',f:'tags_character',c:'tc'},{l:'General',f:'tags_general',c:'tg'},{l:'Meta',f:'tags_metadata',c:'tm'}];
  let html='<h4>Tags</h4>';
  if(p.has_meta){
    for(const s of secs){
      const tags=(p[s.f]||'').trim().split(/\s+/).filter(Boolean);
      if(!tags.length) continue;
      html+=`<div class="lb-ts"><div class="lb-ts-lbl">${s.l}</div><div class="lb-tl">
        ${tags.map(t=>`<span class="lb-tag ${s.c}" onclick="lbTagSearch('${esc(t)}')">${t.replace(/_/g,' ')}</span>`).join('')}</div></div>`;
    }
  } else { html+='<div style="color:var(--text-muted);font-size:12px">No metadata</div>'; }
  sec.innerHTML=html;

  document.getElementById('lb-prev').disabled=lbIdx===0;
  document.getElementById('lb-next').disabled=lbIdx===posts.length-1;
}
function lbTagSearch(tag) {
  closeLb();
  if(!activeTags.includes(tag)) activeTags.push(tag);
  currentPage=1; buildSidebar(stats); fetchAndRender();
}

// ── Utils ──
function encodeURIFilepath(fp){ return fp.split('/').map(encodeURIComponent).join('/'); }
function esc(s){ return s.replace(/\\/g,'\\\\').replace(/'/g,"\\'"); }

boot();
</script>
</body>
</html>
"""


def make_handler(index: Index, root: Path):
    class H(Handler):
        pass
    H.index = index
    H.root = root
    return H


def main():
    parser = argparse.ArgumentParser(description="LocalBooru — gallery-dl browser")
    parser.add_argument("root", nargs="?", default=".", help="Root folder to scan (default: current directory)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f"Error: {root} is not a directory.", file=sys.stderr)
        sys.exit(1)

    global _HTML
    _HTML = _HTML_SOURCE.encode("utf-8")

    index = Index(root)
    handler = make_handler(index, root)
    server = HTTPServer((args.host, args.port), handler)
    url = f"http://{args.host}:{args.port}"
    print(f"[localbooru] Serving {root}")
    print(f"[localbooru] Open → {url}")

    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[localbooru] Stopped.")
        server.server_close()


if __name__ == "__main__":
    main()
