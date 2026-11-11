#!/usr/bin/env python3
"""The detective benchmark: does a memory of justified claims help an LLM name the culprit earlier?

A mystery is fed scene by scene. After every scene the detective:
  1. records the scene as a source in beliefdb,
  2. asks the LLM to extract facts and writes them as claims supported by that scene
     (a fact that already exists gets the scene linked as more support, not a duplicate),
  3. asks the LLM who did it, giving it ONLY what beliefdb returns about each suspect
     (search over the memory), never the transcript,
  4. records the answer as a prediction per suspect's hypothesis (phase 6).
At the end every prediction is settled against the known culprit, so `belief calibration`
reports how the detective's confidence fared scene by scene.

The baseline is the same LLM with the full transcript so far in its context and no memory.
The score is the first scene from which the true culprit is ranked first and stays first,
relative to the reveal scene; lower is better, the reveal scene itself is the worst case.

    examples/detective/detective.py --story examples/detective/stories/lighthouse.json \
        --belief ./build/belief --db /tmp/lighthouse.sqlite \
        --llm openai --llm-url http://127.0.0.1:8080/v1 --model qwen3        # any OpenAI-style chat endpoint
    examples/detective/detective.py --story ... --llm claude-cli             # Claude Code's `claude -p`
    examples/detective/detective.py --story ... --baseline                   # the transcript-only baseline

Requires only the Python standard library. The stories are original and short on purpose:
a model that already knows the ending (from training) would make the benchmark meaningless.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request


# ---------------------------------------------------------------------------- the LLM

def llm_openai(url, model, system, user, api_key=""):
    body = {"model": model, "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
            "temperature": 0, "chat_template_kwargs": {"enable_thinking": False}}
    req = urllib.request.Request(url.rstrip("/") + "/chat/completions", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", **({"Authorization": "Bearer " + api_key} if api_key else {})})
    last = None
    for attempt in range(4):  # a busy local server answers 5xx when its KV cache is full; wait and retry
        try:
            with urllib.request.urlopen(req, timeout=600) as r:
                return json.load(r)["choices"][0]["message"]["content"]
        except urllib.error.HTTPError as e:
            last = e
            detail = e.read().decode(errors="replace")[:200]
            if e.code < 500 or "exceeded" in detail.lower():
                raise RuntimeError(f"LLM {e.code}: {detail}")
            time.sleep(5 * (attempt + 1))
    raise RuntimeError(f"LLM failed after retries: {last}")


def llm_claude_cli(system, user):
    out = subprocess.run(["claude", "-p", "--output-format", "text", "--append-system-prompt", system, user],
                         capture_output=True, text=True, timeout=600)
    if out.returncode != 0:
        raise RuntimeError("claude -p failed: " + out.stderr[:400])
    return out.stdout


def parse_json(text):
    """The first JSON object in a model's answer, fences and prose tolerated."""
    m = re.search(r"\{.*\}", text, re.S)
    if not m:
        raise ValueError("no JSON in the model's answer: " + text[:200])
    return json.loads(m.group(0))


# ---------------------------------------------------------------------------- beliefdb

class Belief:
    def __init__(self, binary, db):
        self.binary, self.db = binary, db

    def run(self, *args, json_out=False, check=True):
        cmd = [self.binary, "--db", self.db, "--no-quota", *args]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if check and p.returncode != 0:
            raise RuntimeError(" ".join(cmd[3:5]) + ": " + p.stderr.strip()[:300])
        return json.loads(p.stdout) if json_out else p.stdout.strip()

    def observe(self, uri, content, date, attribution="narrator", domain="story"):
        return self.run("observe", "--kind", "document", "--uri", uri, "--attribution", attribution, "--observed-at", date,
                        "--domain", domain, "--content", content)

    def remember(self, fact, source_id, domain="story"):
        """A fact as a claim on the scene; an existing structured match gets the scene as more support instead."""
        subject, predicate = fact.get("subject", "").strip(), fact.get("predicate", "").strip()
        if subject and predicate:
            sim = self.run("similar", "--text", fact["text"], "--subject", subject, "--predicate", predicate, "--domain", domain, "--json", json_out=True)
            for e in sim["entries"]:
                if e.get("structural") and e.get("compatible") is True and e["status"] == "active":
                    self.run("link", source_id, e["id"], "supports", check=False)
                    return e["id"], "linked"
        args = ["assert", "--text", fact["text"], "--support", source_id, "--credence", "0.8", "--method", "stated", "--domain", domain]
        if subject and predicate:
            args += ["--subject", subject, "--predicate", predicate, "--value-type", "text", "--value", fact.get("value", fact["text"])[:120]]
        p = subprocess.run([self.binary, "--db", self.db, "--no-quota", *args], capture_output=True, text=True)
        if p.returncode == 0:
            return p.stdout.strip().splitlines()[0], "new"
        m = re.search(r"\b(c_[0-9A-Z]{26})\b", p.stderr)
        if "duplicate_claim" in p.stderr and m:
            self.run("link", source_id, m.group(1), "supports", check=False)
            return m.group(1), "linked"
        raise RuntimeError(p.stderr.strip()[:300])

    def about(self, name, domain="story", limit=12):
        """What the memory holds about a suspect: the claims that mention the name."""
        r = self.run("search", name, "--domain", domain, "--limit", str(limit), "--json", json_out=True)
        return [e for e in r["entries"] if e["type"] == "claim" and e["status"] in ("active", "stale")]

    def ask(self, question, domain="story", limit=10):
        """Claims that answer an investigative question (semantic when embeddings are configured, full-text otherwise)."""
        r = self.run("search", question, "--domain", domain, "--limit", str(limit), "--json", json_out=True)
        return [e for e in r["entries"] if e["type"] == "claim" and e["status"] in ("active", "stale")], r.get("mode", "")

    _scene_cache = {}

    def scene_of(self, claim_id):
        """The scene a claim stands on, read from its provenance (`why`): the first source's uri ends in /scene/N."""
        if claim_id in self._scene_cache:
            return self._scene_cache[claim_id]
        scenes = []
        try:
            w = self.run("why", claim_id, "--json", json_out=True)
            stack = [w]
            while stack:
                n = stack.pop()
                m = re.search(r"/scene/(\d+)", n.get("label", ""))
                if m:
                    scenes.append(int(m.group(1)))
                stack.extend(n.get("children", []))
        except Exception:
            pass
        self._scene_cache[claim_id] = sorted(set(scenes))
        return self._scene_cache[claim_id]

    def conflicts(self, domain="story"):
        try:
            r = self.run("conflicts", "--domain", domain, "--json", json_out=True)
            return r if isinstance(r, list) else r.get("entries", [])
        except Exception:
            return []


# ---------------------------------------------------------------------------- the detective

EXTRACT = ("You extract facts from a scene of a detective story for a memory of justified claims. "
           "Return JSON only: {\"facts\": [{\"text\": \"one short statement\", \"subject\": \"person or thing\", \"predicate\": \"snake_case relation\", \"value\": \"short value\"}]}. "
           "Up to {n} facts. Record what the scene states or what a character claims (say who claims it, e.g. 'X says that ...'), not your own guesses. "
           "Prefer facts an investigator needs: where each person was and when, who had access to the means of death, who gains from it, "
           "what physical evidence was found and where, who contradicts whom or an earlier statement, and relationships to the victim. "
           "Name people exactly as the story names them.")

# generic questions an investigator asks of any whodunit: the memory is searched by these, not by who is talked about most
QUESTIONS = [
    "Who had access to the means of death: the weapon, the poison, the medication, the place?",
    "Who gains from the victim's death: inheritance, money, debt, position, silence?",
    "Where was each person at the time of death, and whose alibi is unconfirmed or contradicted?",
    "Who lied, changed their story, or knew something they could not have known?",
    "What physical evidence was found, where, and whom does it point to?",
    "Who had a quarrel, threat, grievance or secret with the victim?",
]

JUDGE = ("You are a detective. From the material given, estimate the probability that each suspect is the murderer. "
         "Weigh means, motive, opportunity and contradictions between statements; being mentioned often is not evidence. "
         "A confession or an accusation inside the story is a statement to test, not a verdict. "
         "Return JSON only: {\"probabilities\": {\"<suspect>\": p, ...}, \"reason\": \"one sentence\"}. Probabilities must sum to 1 over the given suspects.")


def judge(ask, story, material):
    suspects = ", ".join(story["suspects"])
    user = (f"Victim: {story['victim']}.\nSuspects: {suspects}.\n\n{material}\n\n"
            f"Who killed {story['victim'].split(',')[0]}? JSON with a probability for every suspect.")
    ans = parse_json(ask(JUDGE, user))
    probs = {s: float(ans.get("probabilities", {}).get(s, 0.0)) for s in story["suspects"]}
    total = sum(probs.values())
    if total <= 0:
        raise ValueError("the verdict names no suspect")
    return {s: p / total for s, p in probs.items()}, ans.get("reason", "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--story", required=True)
    ap.add_argument("--belief", default="./build/belief")
    ap.add_argument("--db", default="")
    ap.add_argument("--llm", choices=["openai", "claude-cli"], default="openai")
    ap.add_argument("--llm-url", default=os.environ.get("LLM_URL", "http://127.0.0.1:8080/v1"))
    ap.add_argument("--model", default=os.environ.get("LLM_MODEL", "default"))
    ap.add_argument("--baseline", action="store_true", help="transcript in context, no memory")
    ap.add_argument("--baseline-window-words", type=int, default=6000, help="the baseline keeps only the last N words of the transcript (its context is finite; 0 = all)")
    ap.add_argument("--scenes", type=int, default=0, help="stop after N scenes (0 = all)")
    ap.add_argument("--facts-per-scene", type=int, default=8, help="facts the extractor may return per scene (raise for chapter-length scenes)")
    ap.add_argument("--retrieval", choices=["questions", "name"], default="questions", help="what the judge is shown: answers to investigative questions (default) or claims mentioning each suspect (v1)")
    ap.add_argument("--per-question", type=int, default=10, help="claims per investigative question")
    ap.add_argument("--embeddings", default=os.environ.get("BELIEF_EMBEDDINGS", ""), help="OpenAI-style /v1/embeddings server for semantic search in the memory (strongly recommended)")
    a = ap.parse_args()

    story = json.load(open(a.story, encoding="utf-8"))
    scenes = story["scenes"][: a.scenes or None]
    if a.embeddings:
        os.environ["BELIEF_EMBEDDINGS"] = a.embeddings
    if a.llm == "openai":
        ask = lambda system, user: llm_openai(a.llm_url, a.model, system, user, os.environ.get("LLM_API_KEY", ""))
    else:
        ask = llm_claude_cli

    db = None
    if not a.baseline:
        if not a.db:
            a.db = f"/tmp/detective-{story['slug']}.sqlite"
        for ext in ("", "-wal", "-shm"):
            try: os.remove(a.db + ext)
            except FileNotFoundError: pass
        db = Belief(a.belief, a.db)
        db.run("rule", "--name", "whodunit", "--description", "a suspect's means, motive, opportunity and lies make them the killer", check=False)

    hyp = {}          # suspect -> hypothesis id
    predictions = []  # (scene, suspect, prediction id)
    history = []      # (scene, probabilities)
    transcript = []
    print(f"{story['title']}: {len(scenes)} scenes, reveal at scene {story['reveal_scene']}, "
          f"{'baseline' if a.baseline else 'beliefdb, retrieval by ' + a.retrieval + (', embeddings' if a.embeddings else ', full-text only')}", flush=True)
    for i, scene in enumerate(scenes, 1):
        transcript.append(scene)
        if a.baseline:
            text = "\n\n".join(transcript)
            words = text.split()
            if a.baseline_window_words and len(words) > a.baseline_window_words:
                text = "[...earlier scenes no longer fit the context...]\n\n" + " ".join(words[-a.baseline_window_words:])
            material = "The story so far:\n\n" + text
        else:
            sid = db.observe(f"story:{story['slug']}/scene/{i}", scene, f"2026-01-{i:02d}")
            facts = parse_json(ask(EXTRACT.replace("{n}", str(a.facts_per_scene)), scene)).get("facts", [])
            new = linked = 0
            for f in facts[: a.facts_per_scene]:
                if not f.get("text"):
                    continue
                try:
                    _, how = db.remember(f, sid)
                    new += how == "new"
                    linked += how == "linked"
                except RuntimeError as e:
                    print(f"  skip: {e}", file=sys.stderr)
            parts = []
            if a.retrieval == "name":
                for s in story["suspects"]:
                    claims = db.about(s)
                    parts.append(f"About {s} (from memory, {len(claims)} claims):\n" + ("\n".join("- " + c["text"] for c in claims) or "- nothing yet"))
            else:
                seen = set()
                for q in QUESTIONS:
                    claims, mode = db.ask(q, limit=a.per_question)
                    lines = []
                    for c in claims:
                        if c["id"] in seen:
                            continue
                        seen.add(c["id"])
                        sc = db.scene_of(c["id"])
                        tag = f"[scene {','.join(map(str, sc))}] " if sc else ""
                        lines.append("- " + tag + c["text"])
                    parts.append(f"{q}\n" + ("\n".join(lines) or "- nothing in memory yet"))
                for s in story["suspects"]:
                    claims = [c for c in db.about(s, limit=6) if c["id"] not in seen]
                    if claims:
                        parts.append(f"Also on record about {s}:\n" + "\n".join("- " + (f"[scene {','.join(map(str, db.scene_of(c['id'])))}] " if db.scene_of(c["id"]) else "") + c["text"] for c in claims))
                cs = db.conflicts()
                if cs:
                    lines = []
                    for c in cs[:10]:
                        a_, b_ = c.get("a", {}), c.get("b", {})
                        lines.append(f"- {a_.get('text', '')}  <>  {b_.get('text', '')}")
                    parts.append("Contradictions the memory found between recorded facts:\n" + "\n".join(lines))
            material = "\n\n".join(parts)
            print(f"  scene {i}: {len(facts)} facts, {new} new claims, {linked} linked", flush=True)
        try:
            probs, reason = judge(ask, story, material)
        except (ValueError, json.JSONDecodeError, RuntimeError) as e:  # an unparsable verdict: keep the previous one rather than zeros
            probs = dict(history[-1][1]) if history else {s: 1.0 / len(story["suspects"]) for s in story["suspects"]}
            reason = f"(no verdict this scene: {str(e)[:80]})"
        history.append((i, probs))
        top = max(probs, key=probs.get)
        print(f"  scene {i}: top {top} ({probs[top]:.2f}), culprit p={probs[story['culprit']]:.2f}; {reason[:120]}", flush=True)
        if not a.baseline:
            for s in story["suspects"]:
                if s not in hyp:
                    claims = db.about(s, limit=5)
                    if not claims:
                        continue
                    hyp[s] = db.run("hypothesize", "--text", f"{s} killed {story['victim'].split(',')[0]}", "--from", ",".join(c["id"] for c in claims[:3]),
                                    "--rule", "whodunit", "--domain", "story").splitlines()[0]
                pid = db.run("predict", hyp[s], "--p", f"{max(0.01, min(0.99, probs[s])):.3f}", "--note", f"scene {i}")
                predictions.append((i, s, pid))

    # the score: the first scene from which the culprit is ranked first and stays first
    culprit = story["culprit"]
    first_stable = None
    for i, probs in history:
        if max(probs, key=probs.get) == culprit and all(max(p, key=p.get) == culprit for j, p in history if j >= i):
            first_stable = i
            break
    reveal = story["reveal_scene"]
    print()
    print("scene  " + "  ".join(f"{s[:8]:>8}" for s in story["suspects"]))
    for i, probs in history:
        print(f"{i:>5}  " + "  ".join(f"{probs[s]:>8.2f}" for s in story["suspects"]) + ("  <- reveal" if i == reveal else ""))
    if first_stable is None:
        print(f"result: the culprit ({culprit}) was never ranked first and kept there (reveal at scene {reveal})")
    else:
        print(f"result: {culprit} ranked first from scene {first_stable} of {reveal} ({reveal - first_stable} scenes before the reveal)")
    if not a.baseline:
        for i, s, pid in predictions:
            db.run("settle", pid, "--outcome", "yes" if s == culprit else "no", check=False)
        print()
        print(db.run("calibration", "--domain", "story"))
        print(f"\nmemory kept in {a.db}: `belief --db {a.db} why {hyp.get(culprit, 'ID')}` shows what the verdict stood on")


if __name__ == "__main__":
    main()
