#!/usr/bin/env python3
"""Turns a real text (a novel from Project Gutenberg, a screenplay) into a story file for detective.py.

Novels are split by chapter headings, screenplays by scene headings (INT./EXT.). The metadata the
benchmark needs (victim, suspects, culprit, reveal scene) is given on the command line: the text itself
must not tell the detective who did it before the reveal. The resulting JSON is not meant for the
repository when the text is under copyright; keep it next to the text.

    load_text.py --novel styles.txt --slug styles --title "The Mysterious Affair at Styles" \
        --victim "Emily Inglethorp" --culprit "Alfred Inglethorp" --reveal 12 \
        --suspects "Alfred Inglethorp,John Cavendish,Lawrence Cavendish,Mary Cavendish,Evelyn Howard,Cynthia Murdoch,Dr. Bauerstein" \
        --out stories/styles.json
    load_text.py --screenplay knives_out.txt --scenes-per-chunk 6 ...   # a screenplay has ~150 scenes; group them
"""
import argparse
import json
import re


def gutenberg_body(text):
    start = re.search(r"\*\*\* START OF (THE|THIS) PROJECT GUTENBERG EBOOK.*?\*\*\*", text)
    end = re.search(r"\*\*\* END OF (THE|THIS) PROJECT GUTENBERG EBOOK", text)
    return text[(start.end() if start else 0):(end.start() if end else len(text))]


def split_novel(text):
    body = gutenberg_body(text)
    # chapter headings: "CHAPTER I." / "Chapter 1" / "CHAPTER I. THE TITLE"; the table of contents repeats them, so take the last run
    heads = list(re.finditer(r"^\s*(CHAPTER|Chapter)\s+([IVXLC]+|\d+)\.?[^\n]*$", body, re.M))
    if len(heads) < 3:
        raise SystemExit("no chapter headings found")
    # drop table-of-contents entries: headings followed within 200 chars by another heading
    real = [h for i, h in enumerate(heads) if i + 1 >= len(heads) or heads[i + 1].start() - h.start() > 400]
    chunks = []
    for i, h in enumerate(real):
        stop = real[i + 1].start() if i + 1 < len(real) else len(body)
        chunk = body[h.start():stop].strip()
        chunk = re.sub(r"[ \t]*\n[ \t]*\n+", "\n\n", chunk)
        chunk = re.sub(r"(?<!\n)\n(?!\n)", " ", chunk)  # unwrap hard-wrapped lines
        chunks.append(chunk)
    return chunks


def split_screenplay(text, per_chunk):
    heads = [m.start() for m in re.finditer(r"^\s*\d*\s*(INT\.|EXT\.|INT/EXT|I/E)[^\n]*$", text, re.M)]
    if len(heads) < 5:
        raise SystemExit("no scene headings found")
    scenes = [text[heads[i]:(heads[i + 1] if i + 1 < len(heads) else len(text))].strip() for i in range(len(heads))]
    return ["\n\n".join(scenes[i:i + per_chunk]) for i in range(0, len(scenes), per_chunk)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--novel")
    g.add_argument("--screenplay")
    ap.add_argument("--scenes-per-chunk", type=int, default=6)
    ap.add_argument("--slug", required=True)
    ap.add_argument("--title", required=True)
    ap.add_argument("--victim", required=True)
    ap.add_argument("--suspects", required=True, help="comma-separated, exactly as the text names them")
    ap.add_argument("--culprit", required=True)
    ap.add_argument("--reveal", type=int, required=True, help="1-based scene (chunk) where the culprit is named")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.novel:
        text = open(a.novel, encoding="utf-8-sig").read()
        scenes = split_novel(text)
    else:
        text = open(a.screenplay, encoding="utf-8-sig").read()
        scenes = split_screenplay(text, a.scenes_per_chunk)
    story = {"slug": a.slug, "title": a.title, "victim": a.victim, "suspects": [s.strip() for s in a.suspects.split(",")],
             "culprit": a.culprit, "reveal_scene": a.reveal, "scenes": scenes}
    json.dump(story, open(a.out, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    words = sum(len(s.split()) for s in scenes)
    print(f"{a.out}: {len(scenes)} scenes, {words} words, reveal at {a.reveal}")
    for i, s in enumerate(scenes, 1):
        print(f"  {i:>3} {len(s.split()):>6} words  {s.splitlines()[0][:70]}")


if __name__ == "__main__":
    main()
