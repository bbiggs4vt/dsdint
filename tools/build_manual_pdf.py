#!/usr/bin/env python3
"""build_manual_pdf.py -- turn a Markdown manual (default: the network
explorer's, docs/NET_EXPLORER.md) into a printable, hand-out PDF.

    tools/build_manual_pdf.py                         # -> docs/NET_EXPLORER.pdf
    tools/build_manual_pdf.py --a4                    # A4 instead of US Letter
    tools/build_manual_pdf.py --html out.html         # also a single-file HTML (images inlined)
    tools/build_manual_pdf.py docs/OTHER.md -o x.pdf

The Markdown is rendered to HTML with print styling (a cover page, page
numbers, tables and figures kept whole, the contents and cross-references as
links) and printed by headless Chrome / Chromium. Links that point outside the
manual (README, PROTOCOL.md) become plain text, since they don't resolve in a
PDF; links between its own sections stay clickable.

Needs: python3 with the `markdown` package (python3 -m pip install markdown),
and Chrome or Chromium -- found on the PATH, or set CHROME=/path/to/chrome.
"""

import argparse
import base64
import datetime
import mimetypes
import os
import re
import shutil
import subprocess
import sys
import tempfile

try:
    import markdown
except ImportError:
    sys.exit("needs the Python 'markdown' package: python3 -m pip install markdown")

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

CSS = """
@page { size: %(size)s; margin: 18mm 16mm 18mm 16mm;
        @bottom-right { content: counter(page); font: 9pt 'Helvetica Neue', Arial, sans-serif; color: #777; }
        @bottom-left { content: '%(short)s'; font: 9pt 'Helvetica Neue', Arial, sans-serif; color: #777; } }
@page :first { @bottom-right { content: none; } @bottom-left { content: none; } }
html { -webkit-print-color-adjust: exact; print-color-adjust: exact; }
body { font: 10.5pt/1.45 'Helvetica Neue', Helvetica, Arial, sans-serif; color: #1d2125; margin: 0; }
.cover { height: 240mm; display: flex; flex-direction: column; justify-content: center; page-break-after: always; }
.cover .kicker { color: #2a7f9e; font-size: 13pt; letter-spacing: .08em; text-transform: uppercase; margin-bottom: 6mm; }
.cover h1 { font-size: 34pt; line-height: 1.1; margin: 0 0 6mm; border: 0; color: #12171b; }
.cover .sub { font-size: 13pt; color: #444; max-width: 140mm; }
.cover .meta { margin-top: 18mm; color: #666; font-size: 10pt; }
.cover img { margin-top: 14mm; width: 100%%; border: 1px solid #c9cfd4; border-radius: 3px; }
h1 { font-size: 22pt; margin: 0 0 4mm; color: #12171b; }
h2 { font-size: 15pt; margin: 9mm 0 3mm; padding-bottom: 1.5mm; border-bottom: 2px solid #2a7f9e; color: #12171b;
     page-break-after: avoid; break-after: avoid; }
h3 { font-size: 12pt; margin: 6mm 0 2mm; color: #12171b; page-break-after: avoid; break-after: avoid; }
h2.appendix { page-break-before: always; break-before: page; }
p, li { orphans: 3; widows: 3; }
a { color: #1f6f8b; text-decoration: none; }
code { font: 9pt Menlo, Consolas, 'DejaVu Sans Mono', monospace; background: #eef1f3; padding: 0 2px; border-radius: 2px; }
pre { background: #eef1f3; padding: 3mm; border-radius: 3px; overflow-wrap: anywhere; white-space: pre-wrap;
      page-break-inside: avoid; break-inside: avoid; }
pre code { background: none; padding: 0; }
table { border-collapse: collapse; width: 100%%; margin: 3mm 0 4mm; font-size: 9pt; }
th, td { border: 1px solid #c9cfd4; padding: 1.4mm 2mm; text-align: left; vertical-align: top; }
th { background: #e3eaee; font-weight: 600; }
tr { page-break-inside: avoid; break-inside: avoid; }
thead { display: table-header-group; }
img { max-width: 100%%; border: 1px solid #c9cfd4; border-radius: 3px; }
p.figure { margin: 4mm 0 1mm; text-align: center; page-break-inside: avoid; break-inside: avoid;
           page-break-after: avoid; break-after: avoid; }
p.caption { margin: 0 0 5mm; text-align: center; color: #555; font-size: 9pt; }
hr { border: 0; border-top: 1px solid #c9cfd4; margin: 6mm 0; }
blockquote { border-left: 3px solid #2a7f9e; margin: 3mm 0; padding: 1mm 4mm; color: #333; background: #f3f7f9; }
.toc-note { color: #666; }
"""


def find_chrome():
    c = os.environ.get("CHROME")
    if c:
        return c
    for name in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "chrome", "msedge"):
        p = shutil.which(name)
        if p:
            return p
    for p in ("/opt/pw-browsers/chromium",
              "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
              r"C:\Program Files\Google\Chrome\Application\chrome.exe",
              r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"):
        if os.path.exists(p):
            return p
    sys.exit("Chrome / Chromium not found: install one, or set CHROME=/path/to/chrome")


def git_version():
    try:
        rev = subprocess.run(["git", "-C", REPO, "log", "-1", "--format=%h, %cs"], capture_output=True, text=True,
                             timeout=10).stdout.strip()
        return rev
    except Exception:
        return ""


def render(md_path, size):
    text = open(md_path, encoding="utf-8").read()
    title = re.search(r"^#\s+(.+)$", text, re.M)
    title = title.group(1).strip() if title else os.path.basename(md_path)
    text = re.sub(r"^#\s+.+\n", "", text, count=1, flags=re.M)        # the cover carries the title
    body = markdown.markdown(text, extensions=["tables", "fenced_code", "toc", "sane_lists", "attr_list"],
                             extension_configs={"toc": {"permalink": False}})
    # Figures: an image paragraph followed by an italic caption paragraph.
    body = re.sub(r"<p>(<img [^>]+>)(?:</p>\s*<p>|\s*)<em>(Figure[^<]*)</em></p>",
                  r'<p class="figure">\1</p><p class="caption">\2</p>', body)
    body = re.sub(r'<p align="center">((?:<img [^>]+>(?:&nbsp;)*)+)</p>\s*<p><em>(Figure[^<]*)</em></p>',
                  r'<p class="figure">\1</p><p class="caption">\2</p>', body)
    # Appendices start on a new page.
    body = re.sub(r'<h2 id="(appendix-[^"]+)">', r'<h2 class="appendix" id="\1">', body)
    # Links outside the manual don't resolve in a PDF: keep their text.
    body = re.sub(r'<a href="(?!#|https?:)[^"]*">(.*?)</a>', r"\1", body)
    short = title.replace("'", "")
    first_img = re.search(r'<img [^>]*src="([^"]+)"', body)
    cover_img = '<img src="%s" alt="">' % first_img.group(1) if first_img else ""
    when = datetime.date.today().isoformat()
    ver = git_version()
    cover = ('<div class="cover"><div class="kicker">dsd-server</div><h1>%s</h1>'
             '<div class="sub">Using the network explorer: its views and controls, call audio and '
             'speech-to-text, saving and combining data, and how calls, talkgroups, radios and networks '
             'are associated for each protocol.</div>'
             '<div class="meta">%s%s</div>%s</div>') % (
        title, when, (" &middot; build " + ver) if ver else "", cover_img)
    css = CSS % {"size": size, "short": short}
    return ('<!doctype html><html lang="en"><head><meta charset="utf-8"><title>%s</title><style>%s</style>'
            '</head><body>%s%s</body></html>') % (title, css, cover, body)


def inline_images(html, base):
    def sub(m):
        path = os.path.join(base, m.group(1))
        if not os.path.exists(path):
            return m.group(0)
        mime = mimetypes.guess_type(path)[0] or "image/png"
        data = base64.b64encode(open(path, "rb").read()).decode()
        return 'src="data:%s;base64,%s"' % (mime, data)
    return re.sub(r'src="(?!data:|https?:)([^"]+)"', sub, html)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("markdown", nargs="?", default=os.path.join(REPO, "docs", "NET_EXPLORER.md"))
    ap.add_argument("-o", "--output", help="PDF to write (default: next to the Markdown, .pdf)")
    ap.add_argument("--a4", action="store_true", help="A4 paper (default: US Letter)")
    ap.add_argument("--html", help="also write a single-file HTML version (images inlined)")
    a = ap.parse_args()
    md = os.path.abspath(a.markdown)
    out = os.path.abspath(a.output or os.path.splitext(md)[0] + ".pdf")
    base = os.path.dirname(md)
    html = render(md, "A4" if a.a4 else "Letter")
    if a.html:
        with open(a.html, "w", encoding="utf-8") as f:
            f.write(inline_images(html, base))
        print("wrote", a.html)
    # Written next to the Markdown so its relative image paths resolve.
    fd, tmp = tempfile.mkstemp(suffix=".html", prefix=".manual_", dir=base)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(html)
        cmd = [find_chrome(), "--headless", "--disable-gpu", "--no-pdf-header-footer", "--hide-scrollbars",
               "--allow-file-access-from-files", "--print-to-pdf=" + out, "file://" + tmp]
        if hasattr(os, "geteuid") and os.geteuid() == 0:
            cmd.insert(1, "--no-sandbox")                       # Chrome refuses to run as root otherwise
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        if r.returncode != 0 or not os.path.exists(out):
            sys.stderr.write(r.stderr[-2000:])
            sys.exit("printing to PDF failed")
    finally:
        os.unlink(tmp)
    print("wrote", out)


if __name__ == "__main__":
    main()
