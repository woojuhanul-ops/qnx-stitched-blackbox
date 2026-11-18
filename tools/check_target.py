#!/usr/bin/env python3
"""Independent host checker for the QNX target run (checker seat - it does
not share code with the target verifier). Parses the QEMU serial log,
re-derives every verdict from the original exam.json, recomputes the
timing distribution, and writes results_target.json in the same schema
as the host reference exam/results.json plus target extras."""
import json, re, statistics, sys, pathlib

SRC = pathlib.Path.home() / "workspace/kiroween/stitched-ledger"
exam = json.loads((SRC / "exam/exam.json").read_text())
log_path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "~/workspace/qnx/serial_bb_run1.log").expanduser()
lines = log_path.read_text(errors="replace").splitlines()

qlines, phases, summary = {}, {}, None
bad_json = 0
for ln in lines:
    ln = ln.strip()
    if not ln.startswith("{"):
        continue
    try:
        obj = json.loads(ln)
    except Exception:
        bad_json += 1
        continue
    if "id" in obj and "type" in obj:
        qlines[obj["id"]] = obj
    elif "phase" in obj:
        phases[obj["phase"]] = obj
    elif "summary" in obj:
        summary = obj["summary"]

def norm(s):
    return re.sub(r"\s+", " ", (s or "").strip())

def evset(s):
    out = set()
    for part in (s or "").split(";"):
        if part:
            r, c = part.split(":", 1)
            out.add((int(r), c))
    return out

results, failures = [], []
retr_ok = trap_ok = ev_ok = 0
proc_ns, rt_ns = [], []
for e in exam:
    q = qlines.get(e["id"])
    if q is None:
        failures.append({"id": e["id"], "failureKind": "missing-from-target-log"})
        continue
    proc_ns.append(q["proc_ns"]); rt_ns.append(q["rt_ns"])
    exp_ev = {(x["row"], x["col"]) for x in (e.get("expectedEvidence") or [])}
    got_ev = evset(q.get("ev", ""))
    if e["expectedStatus"] == "ABSTAIN":
        correct = q["status"] == "ABSTAIN_NO_RECORD"
        evc = False
        if correct: trap_ok += 1
        else: failures.append({"id": e["id"], "failureKind": "trap-not-abstained", "got": q["status"]})
    else:
        correct = q["status"] == "ANSWER" and norm(q["answer"]) == norm(e["expectedAnswer"])
        evc = correct and got_ev == exp_ev
        if correct:
            retr_ok += 1
            if evc: ev_ok += 1
            else: failures.append({"id": e["id"], "failureKind": "evidence-mismatch"})
        else:
            failures.append({"id": e["id"], "failureKind": "wrong-or-abstained", "got": q["status"], "answer": q["answer"]})
    results.append({"id": e["id"], "type": e["type"], "question": e["question"],
                    "expectedStatus": e["expectedStatus"], "expectedAnswer": e.get("expectedAnswer"),
                    "gotStatus": q["status"], "gotAnswer": q["answer"] or None,
                    "correct": correct, "evidenceCorrect": evc,
                    "proc_ns": q["proc_ns"], "rt_ns": q["rt_ns"]})

def stats(v):
    if not v: return {}
    s = sorted(v)
    return {"avg": round(statistics.mean(v)), "median": s[len(s)//2],
            "p95": s[min(len(s)-1, int(len(s)*0.95))], "max": s[-1], "min": s[0]}

out = {
    "checker": "independent host checker (check_target.py), verdicts re-derived from exam.json",
    "target_summary_from_verifier": summary,
    "phases": phases,
    "retrieval": f"{retr_ok}/66", "traps": f"{trap_ok}/15", "evidence": f"{ev_ok}/66",
    "proc_ns_stats": stats(proc_ns), "rt_ns_stats": stats(rt_ns),
    "proc_ns_stats_excl_first": stats(proc_ns[1:]),
    "unparsed_json_lines": bad_json,
    "failures": failures,
    "results": results,
}
dest = pathlib.Path("~/workspace/qnx/results_target.json").expanduser()
dest.write_text(json.dumps(out, indent=1, ensure_ascii=False))
print(json.dumps({k: out[k] for k in ("retrieval", "traps", "evidence", "proc_ns_stats",
                                      "rt_ns_stats", "proc_ns_stats_excl_first",
                                      "unparsed_json_lines")}, indent=1))
print("failures:", failures)
print("phases:", json.dumps(phases))
print("verifier summary:", json.dumps(summary))
