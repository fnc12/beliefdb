#!/usr/bin/env python3
"""Does the judge model already know this story? If it does, the benchmark measures memory of the
training data, not detection. Two probes against an OpenAI-style chat endpoint:

  1. by title: "Who is the murderer in <title>?" — knowledge of the work itself;
  2. blind: the first two scenes, no title: "what work is this, who did it?" — recognition from the text.

Exit code 0 = the model named neither the work nor the culprit, 1 = it did (do not use this text).

    recognition_check.py --story stories/styles.json --llm-url http://127.0.0.1:12366/v1 --model qwen3
"""
import argparse
import json
import os
import re
import sys
import urllib.request


def ask(url, model, user, api_key=""):
    body = {"model": model, "messages": [{"role": "user", "content": user}], "temperature": 0, "chat_template_kwargs": {"enable_thinking": False}}
    req = urllib.request.Request(url.rstrip("/") + "/chat/completions", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", **({"Authorization": "Bearer " + api_key} if api_key else {})})
    with urllib.request.urlopen(req, timeout=300) as r:
        return json.load(r)["choices"][0]["message"]["content"]


def surname(name):
    return name.split()[-1].lower()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--story", required=True)
    ap.add_argument("--llm-url", default=os.environ.get("LLM_URL", "http://127.0.0.1:8080/v1"))
    ap.add_argument("--model", default=os.environ.get("LLM_MODEL", "default"))
    a = ap.parse_args()
    story = json.load(open(a.story, encoding="utf-8"))
    key = os.environ.get("LLM_API_KEY", "")
    culprit = surname(story["culprit"])

    q1 = f"In the detective story \"{story['title']}\", who turns out to be the murderer? Answer with the name only, or 'I don't know'."
    a1 = ask(a.llm_url, a.model, q1, key)
    knows_title = culprit in a1.lower()
    print(f"by title: {a1.strip()[:120]!r} -> {'KNOWS the culprit' if knows_title else 'does not name the culprit'}")

    head = "\n\n".join(story["scenes"][:2])[:6000]
    q2 = ("Here is the beginning of a detective story.\n\n" + head +
          "\n\nDo you recognise this work? Answer as JSON: {\"title\": \"...\" or null, \"author\": \"...\" or null, \"murderer\": \"...\" or null}. Guess only if you actually recognise it.")
    a2 = ask(a.llm_url, a.model, q2, key)
    m = re.search(r"\{.*\}", a2, re.S)
    guess = json.loads(m.group(0)) if m else {}
    title_words = [w for w in re.findall(r"[a-z]{4,}", story["title"].lower()) if w not in ("the", "and", "affair", "mystery", "murder", "case")]
    t = (guess.get("title") or "").lower()
    recognised_title = any(w in t for w in title_words) if title_words else False
    recognised_culprit = culprit in (guess.get("murderer") or "").lower()
    print(f"blind: {guess} -> {'RECOGNISED' if (recognised_title or recognised_culprit) else 'not recognised'}")

    bad = knows_title or recognised_title or recognised_culprit
    print("verdict:", "contaminated, do not use" if bad else "usable")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
