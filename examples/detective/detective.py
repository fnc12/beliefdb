#!/usr/bin/env python3
"""The detective benchmark: does a memory of justified claims help an LLM name the culprit earlier?

A mystery is fed scene by scene. After every scene the detective:
  1. records the scene as a source in beliefdb,
  2. asks the LLM to extract facts and writes them as claims supported by that scene
     (a fact that already exists gets the scene linked as more support, not a duplicate),
  3. thinks (`--retrieval hypotheses`, the default): a walk over the scene's facts with their nearest
     neighbours in memory writes first-order hypotheses (who lied, who had access, whose story does
     not fit), reviews the live ones against the new facts (retract / confirm), and a second-order
     pass writes hypotheses that stand on hypotheses where two thoughts meet on one person,
  4. asks the LLM who did it, giving it ONLY what the memory holds: the tower of live hypotheses
     with what each stands on (older modes: answers to investigative questions, or claims by name),
     never the transcript,
  5. records the answer as a prediction per suspect's hypothesis (phase 6).
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
            with urllib.request.urlopen(req, timeout=1800) as r:
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
    raw = m.group(0)
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        pass
    # a small model sometimes leaves a trailing comma or an unescaped quote inside a string; salvage what can be salvaged
    fixed = re.sub(r",\s*([}\]])", r"\1", raw)
    try:
        return json.loads(fixed)
    except json.JSONDecodeError:
        pass
    out = {}
    for key in ("thoughts", "facts"):
        items = []
        for t, f in re.findall(r"\"text\"\s*:\s*\"((?:[^\"\\]|\\.)*)\"\s*(?:,\s*\"from\"\s*:\s*\[([^\]]*)\])?", raw):
            item = {"text": t.replace('\\"', '"')}
            if f:
                item["from"] = re.findall(r"\"([A-Z]\d+)\"", f)
            items.append(item)
        if items and f'"{key}"' in raw:
            out[key] = items
    if out:
        return out
    raise ValueError("unparsable JSON in the model's answer: " + raw[:200])


# ---------------------------------------------------------------------------- beliefdb

class Belief:
    def __init__(self, binary, db):
        self.binary, self.db = binary, db
        self._scene_cache = {}
        self._why_cache = {}

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

    def neighbours(self, text, exclude, domain="story", limit=6):
        """The nearest living claims and hypotheses to a text (semantic with embeddings, full-text otherwise)."""
        r = self.run("search", text, "--domain", domain, "--limit", str(limit + len(exclude)), "--json", json_out=True)
        return [e for e in r["entries"] if e["id"] not in exclude and e["status"] in ("active", "stale")][:limit]

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

    def premises(self, hid):
        """What a hypothesis stands on, one level down: (id, type, text) of each premise."""
        if hid not in self._why_cache:
            out = []
            try:
                w = self.run("why", hid, "--depth", "1", "--json", json_out=True)
                for ch in w.get("children", []):
                    if ch.get("via") == "derives_from":
                        out.append((ch["id"], ch.get("type", ""), ch.get("label", "")))
            except Exception:
                pass
            self._why_cache[hid] = out
        return self._why_cache[hid]

    def hypothesize(self, text, from_ids, rule, domain="story"):
        """A hypothesis on claims and/or hypotheses; the text-duplicate gate is answered with `despite` (a thought about X and the same thought about Y embed alike)."""
        args = ["hypothesize", "--text", text, "--from", ",".join(from_ids), "--rule", rule, "--domain", domain]
        despite = []
        for _ in range(8):
            p = subprocess.run([self.binary, "--db", self.db, "--no-quota", *args, *sum((["--despite", d] for d in despite), [])], capture_output=True, text=True)
            if p.returncode == 0:
                return p.stdout.strip().splitlines()[0]
            m = re.search(r"\b([ch]_[0-9A-Z]{26})\b", p.stderr)
            if "duplicate_claim" in p.stderr and m and m.group(1) not in despite:
                despite.append(m.group(1))
                continue
            raise RuntimeError(p.stderr.strip()[:300])
        raise RuntimeError("could not record the hypothesis: " + text)

    def live_hypotheses(self, domain="story"):
        r = self.run("hypotheses", "--domain", domain, "--json", json_out=True)
        entries = r if isinstance(r, list) else r.get("entries", [])
        return [h for h in entries if h.get("status") in ("active", "stale")]

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

# the walk: new facts of the scene, each with its nearest neighbours in memory; the detective writes the inferences an
# investigator would draw where two items meet, and reviews the live thoughts against the new facts
WALK = ("You are a detective keeping a memory of justified thoughts about a case. You are given the NEW facts from the latest scene "
        "(F1, F2, ...), for each the nearest facts and thoughts already in memory, and the live thoughts so far (H1, H2, ...). "
        "Write up to {n} new thoughts: inferences an investigator draws where two or more listed items meet. Good thoughts: a statement that "
        "contradicts another or a known fact (a lie), a person with access to the means of death, a motive, an unexplained movement or "
        "absence, an object that is where it should not be, knowledge a person could not have had honestly. Each thought is one sentence "
        "naming the person and the ground, and lists the items it stands on by their labels (at least one, preferably two). Do not restate a "
        "single fact as a thought; do not write 'X is the murderer' (that is judged separately); do not invent what no listed item says. "
        "Then review the live thoughts: name those the new facts refute (with the refuting label) and those a new fact confirms. "
        "Return JSON only: {\"thoughts\": [{\"text\": \"...\", \"from\": [\"F1\", \"H3\"]}], "
        "\"refuted\": [{\"id\": \"H2\", \"by\": \"F1\", \"reason\": \"...\"}], \"confirmed\": [{\"id\": \"H4\", \"by\": [\"F2\"]}]}. "
        "Empty lists are fine.")

# the second order: where two live thoughts point at the same person or the same event, a thought of a higher order
ORDER2 = ("You are a detective reviewing your own live thoughts about a case (H1, H2, ... with what each stands on). "
          "Write up to {n} thoughts of a higher order: an inference that stands on TWO OR MORE listed thoughts (facts may be added) and "
          "says what they mean together, e.g. 'X both lied about the hour and had the key, so X was in the study when the victim died'. "
          "Prefer combinations that converge on one person or one event. Do not repeat a listed thought and do not write 'X is the murderer'. "
          "Return JSON only: {\"thoughts\": [{\"text\": \"...\", \"from\": [\"H1\", \"H5\"]}]}. An empty list is fine when nothing converges.")

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


class Labels:
    """Short labels for the model (F1, H3) in place of 26-character ids, and the way back."""

    def __init__(self):
        self.by_label, self.by_id, self.n = {}, {}, {"F": 0, "H": 0}

    def label(self, node_id, kind):
        if node_id in self.by_id:
            return self.by_id[node_id]
        self.n[kind] += 1
        lab = f"{kind}{self.n[kind]}"
        self.by_label[lab], self.by_id[node_id] = node_id, lab
        return lab

    def ids(self, labels):
        return [self.by_label[l] for l in labels if isinstance(l, str) and l in self.by_label]


def think(db, ask, scene_claims, whodunit_ids, n_walk, n_order2, do_order2):
    """The walk over the scene's facts and the second-order pass. Returns counts for the log."""
    labels = Labels()
    live = [h for h in db.live_hypotheses() if h["id"] not in whodunit_ids]
    live_by_id = {h["id"]: h for h in live}
    lines = ["New facts from the latest scene, each with the nearest items already in memory:"]
    seen_ids = set(scene_claims)
    for cid, text in scene_claims.items():
        lines.append(f"{labels.label(cid, 'F')}: {text}")
        for nb in db.neighbours(text, exclude=seen_ids | whodunit_ids):
            kind = "H" if nb["type"] == "hypothesis" else "F"
            lines.append(f"    near: {labels.label(nb['id'], kind)}: {nb['text']}")
    if live:
        lines.append("")
        lines.append("Live thoughts so far:")
        for h in live:
            lines.append(f"{labels.label(h['id'], 'H')} (order {h.get('order', 1)}): {h['text']}")
    counts = {"walk": 0, "retracted": 0, "confirmed": 0, "order2": 0}
    try:
        ans = parse_json(ask(WALK.replace("{n}", str(n_walk)), "\n".join(lines)))
    except (ValueError, json.JSONDecodeError, RuntimeError) as e:
        print(f"  walk: no usable answer ({str(e)[:80]})", file=sys.stderr)
        ans = {}
    for t in (ans.get("thoughts") or [])[:n_walk]:
        text, frm = (t.get("text") or "").strip(), labels.ids(t.get("from") or [])
        if not text or not frm:
            continue
        try:
            db.hypothesize(text, frm, "walk")
            counts["walk"] += 1
        except RuntimeError as e:
            print(f"  walk: skip thought: {e}", file=sys.stderr)
    for r in ans.get("refuted") or []:
        hid = labels.by_label.get(r.get("id", ""))
        if hid in live_by_id:
            by = labels.by_label.get(r.get("by", ""), "")
            try:
                db.run("retract", hid, "--reason", f"refuted by {by}: {(r.get('reason') or '')[:160]}")
                counts["retracted"] += 1
            except RuntimeError as e:
                print(f"  walk: cannot retract {hid}: {e}", file=sys.stderr)
    for c in ans.get("confirmed") or []:
        hid = labels.by_label.get(c.get("id", ""))
        support = [i for i in labels.ids(c.get("by") or []) if i.startswith("c_")]
        if hid in live_by_id and support:
            try:
                db.run("confirm", hid, "--support", ",".join(support), "--credence", "0.8", "--method", "stated")
                counts["confirmed"] += 1
            except RuntimeError as e:
                print(f"  walk: cannot confirm {hid}: {e}", file=sys.stderr)
    if not do_order2:
        return counts
    # the second order over the live thoughts, each shown with what it stands on
    labels2 = Labels()
    live = [h for h in db.live_hypotheses() if h["id"] not in whodunit_ids]
    if len(live) < 2:
        return counts
    lines = ["Live thoughts, each with what it stands on:"]
    for h in live:
        prem = db.premises(h["id"])
        lines.append(f"{labels2.label(h['id'], 'H')} (order {h.get('order', 1)}): {h['text']}")
        for pid, ptype, ptext in prem:
            lines.append(f"    stands on {labels2.label(pid, 'H' if ptype == 'hypothesis' else 'F')}: {ptext}")
    try:
        ans = parse_json(ask(ORDER2.replace("{n}", str(n_order2)), "\n".join(lines)))
    except (ValueError, json.JSONDecodeError, RuntimeError) as e:
        print(f"  order 2: no usable answer ({str(e)[:80]})", file=sys.stderr)
        return counts
    for t in (ans.get("thoughts") or [])[:n_order2]:
        text, frm = (t.get("text") or "").strip(), labels2.ids(t.get("from") or [])
        if not text or len([i for i in frm if i.startswith("h_")]) < 1 or len(frm) < 2:
            continue
        try:
            db.hypothesize(text, frm, "order2")
            counts["order2"] += 1
        except RuntimeError as e:
            print(f"  order 2: skip thought: {e}", file=sys.stderr)
    return counts


def tower(db, whodunit_ids):
    """What the judge sees in the hypotheses mode: the live thoughts, highest order first, each with what it stands on."""
    live = [h for h in db.live_hypotheses() if h["id"] not in whodunit_ids]
    if not live:
        return "The memory holds no thoughts about the case yet."
    live.sort(key=lambda h: (-h.get("order", 1), h.get("created_at", "")))
    lines = ["The detective's live thoughts (from memory; each with the facts or thoughts it stands on; higher order = built on other thoughts):"]
    for h in live:
        lines.append(f"- [order {h.get('order', 1)}, credence {h['credence']['value']:.2f}] {h['text']}")
        for _, ptype, ptext in db.premises(h["id"]):
            lines.append(f"    <- {'thought' if ptype == 'hypothesis' else 'fact'}: {ptext}")
    cs = db.conflicts()
    if cs:
        lines.append("")
        lines.append("Contradictions the memory found between recorded facts:")
        for c in cs[:10]:
            lines.append(f"- {c.get('a', {}).get('text', '')}  <>  {c.get('b', {}).get('text', '')}")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--story", required=True)
    ap.add_argument("--belief", default="./build/belief")
    ap.add_argument("--db", default="")
    ap.add_argument("--llm", choices=["openai", "claude-cli"], default="openai")
    ap.add_argument("--llm-url", default=os.environ.get("LLM_URL", "http://127.0.0.1:8080/v1"))
    ap.add_argument("--model", default=os.environ.get("LLM_MODEL", "default"))
    ap.add_argument("--baseline", action="store_true", help="transcript in context, no memory")
    ap.add_argument("--baseline-window-words", type=int, default=0, help="the baseline keeps only the last N words of the transcript (0 = all; set it when the model's context is small)")
    ap.add_argument("--scenes", type=int, default=0, help="stop after N scenes (0 = all)")
    ap.add_argument("--facts-per-scene", type=int, default=8, help="facts the extractor may return per scene (raise for chapter-length scenes)")
    ap.add_argument("--retrieval", choices=["hypotheses", "questions", "name"], default="hypotheses",
                    help="what the judge is shown: the tower of live hypotheses (v3, default), answers to investigative questions (v2) or claims mentioning each suspect (v1)")
    ap.add_argument("--per-question", type=int, default=10, help="claims per investigative question (v2)")
    ap.add_argument("--walk-hypotheses", type=int, default=5, help="v3: first-order thoughts the walk may write per scene")
    ap.add_argument("--order2-hypotheses", type=int, default=3, help="v3: second-order thoughts per pass")
    ap.add_argument("--order2-every", type=int, default=1, help="v3: run the second-order pass every N scenes (0 = never)")
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
        db.run("rule", "--name", "walk", "--description", "an investigator's inference where a new fact meets what the memory already holds", check=False)
        db.run("rule", "--name", "order2", "--description", "two or more thoughts converge on one person or one event", check=False)

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
            scene_claims = {}
            for f in facts[: a.facts_per_scene]:
                if not f.get("text"):
                    continue
                try:
                    cid, how = db.remember(f, sid)
                    scene_claims[cid] = f["text"]
                    new += how == "new"
                    linked += how == "linked"
                except RuntimeError as e:
                    print(f"  skip: {e}", file=sys.stderr)
            print(f"  scene {i}: {len(facts)} facts, {new} new claims, {linked} linked", flush=True)
            if a.retrieval == "hypotheses":
                counts = think(db, ask, scene_claims, set(hyp.values()), a.walk_hypotheses, a.order2_hypotheses,
                               a.order2_every > 0 and i % a.order2_every == 0)
                print(f"  scene {i}: thoughts +{counts['walk']}, order 2 +{counts['order2']}, retracted {counts['retracted']}, confirmed {counts['confirmed']}", flush=True)
                material = tower(db, set(hyp.values()))
            elif a.retrieval == "name":
                parts = []
                for s in story["suspects"]:
                    claims = db.about(s)
                    parts.append(f"About {s} (from memory, {len(claims)} claims):\n" + ("\n".join("- " + c["text"] for c in claims) or "- nothing yet"))
                material = "\n\n".join(parts)
            else:
                parts, seen = [], set()
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
                    parts.append("Contradictions the memory found between recorded facts:\n" + "\n".join(f"- {c.get('a', {}).get('text', '')}  <>  {c.get('b', {}).get('text', '')}" for c in cs[:10]))
                material = "\n\n".join(parts)
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
                    hyp[s] = db.hypothesize(f"{s} killed {story['victim'].split(',')[0]}", [c["id"] for c in claims[:3]], "whodunit")
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
        if a.retrieval == "hypotheses":
            live = db.live_hypotheses()
            print(f"\n{len(live)} live thoughts at the end, {sum(1 for h in live if h.get('order', 1) > 1)} of order 2 or higher; `belief --db {a.db} hypotheses --domain story` lists them")
        print(f"\nmemory kept in {a.db}: `belief --db {a.db} why {hyp.get(culprit, 'ID')}` shows what the verdict stood on")


if __name__ == "__main__":
    main()
