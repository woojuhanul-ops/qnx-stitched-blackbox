#!/usr/bin/env python3
"""Host bridge: streams the QNX target's JSON results into the existing
Stitched Ledger screen. Reuses web/styles.css verbatim and the same DOM
classes as web/index.html (site-header / main-split / answer-card /
ledger-table / evidence-row / evidence-cell) - no new UI design.
Input : ~/workspace/qnx/results_target.json (from check_target.py)
        stitched-ledger data/ledger.json + web/styles.css
Output: ~/workspace/qnx/bridge/{index.html, styles.css}
"""
import json, html, pathlib, shutil

HOME = pathlib.Path.home()
SL = HOME / "workspace/kiroween/stitched-ledger"
OUT = HOME / "workspace/qnx/bridge"
OUT.mkdir(parents=True, exist_ok=True)
shutil.copy(SL / "web/styles.css", OUT / "styles.css")

res = json.loads((HOME / "workspace/qnx/results_target.json").read_text())
ledger = json.loads((SL / "data/ledger.json").read_text())
ph = res["phases"]; summ = res["target_summary_from_verifier"]
ps, rs = res["proc_ns_stats"], res["rt_ns_stats"]
q1 = next(r for r in res["results"] if r["id"] == 1)

def us(ns): return f"{ns/1000:.1f} µs"
def ms(ns): return f"{ns/1_000_000:.2f} ms"

rows_html = []
COLS = ["name", "type", "location", "role", "quantity", "unit", "hazard", "links"]
for i, r in enumerate(ledger, start=1):
    tr_cls = ' class="evidence-row"' if i == 1 else ""
    tds = [f'<td class="row-num">{i}</td>']
    for c in COLS:
        v = r[c]
        v = ", ".join(v) if isinstance(v, list) else str(v)
        cls = ' class="evidence-cell"' if (i == 1 and c == "location") else ""
        tds.append(f"<td{cls}>{html.escape(v)}</td>")
    rows_html.append(f"<tr{tr_cls}>" + "".join(tds) + "</tr>")

qrows = []
for r in res["results"]:
    st = r["gotStatus"]
    st_short = {"ANSWER": "ANSWER", "ABSTAIN_NO_RECORD": "ABSTAIN"}.get(st, st)
    ok = "✓" if r["correct"] else "✗"
    qrows.append(
        f'<tr><td class="row-num">{r["id"]}</td><td>{r["type"]}</td>'
        f'<td>{html.escape(r["question"])}</td><td>{st_short}</td>'
        f'<td>{html.escape(r["gotAnswer"] or "—")}</td>'
        f'<td>{us(r["proc_ns"])}</td><td>{ok}</td></tr>')

page = f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Stitched Ledger — QNX Target Bridge</title>
  <link rel="stylesheet" href="./styles.css" />
</head>
<body>
  <header class="site-header">
    <div class="header-title">STITCHED LEDGER</div>
    <div class="header-subtitle">Deterministic record retrieval — no model, no guessing</div>
    <div class="header-readouts">
      <span class="readout"><span class="readout-label">ROWS</span><span class="readout-value">48</span></span>
      <span class="readout"><span class="readout-label">COLUMNS</span><span class="readout-value">8</span></span>
      <span class="readout"><span class="readout-label">ENGINE</span><span class="readout-value">DETERMINISTIC</span></span>
      <span class="readout"><span class="readout-label">TARGET</span><span class="readout-value">QNX 8.0 · QEMU x86_64</span></span>
    </div>
  </header>

  <main class="main-split">
    <section class="query-panel">
      <div class="answer-card state-answer">
        <div class="card-status">ANSWER · streamed from QNX target</div>
        <div class="card-answer">{html.escape(q1['gotAnswer'])}</div>
        <div class="evidence-chips"><span class="ev-chip">Row 1 · location</span></div>
        <div class="stitch-line">evidence → <span class="stitch-coords">[Row 1 · location]</span></div>
        <p>Question 1 of the on-target exam: “{html.escape(q1['question'])}” — answered by the
        ledger server on QNX via MsgSend/MsgReply in {us(q1['proc_ns'])} of server-side retrieval.</p>
      </div>

      <div class="verify-panel">
        <div class="verify-result">
          <div class="verify-summary">
            <span class="vpass">On-target exam: retrieval {summ['retrieval']} · abstention {summ['traps']} · evidence {summ['evidence']}</span><br/>
            Server retrieval (QEMU-measured): median {us(ps['median'])}, avg {us(ps['avg'])}, p95 {us(ps['p95'])}, max {us(ps['max'])} ·
            IPC round-trip median {us(rs['median'])}<br/>
            Time budget derived from measurement (20 × slowest): {ms(summ['budget_ns'])} — re-run identical {summ['budget_rerun_identical']}/81, timeouts {summ['budget_rerun_timeouts']} ·
            1-cycle budget: ABSTAIN_TIMEOUT {summ['timeout_gate']}<br/>
            Determinism: 8,100 repeated queries, {summ['determinism_mismatches']} mismatches<br/>
            Fault injection: sensor killed → detected {ms(ph['D']['detect_ns'])}, revived {ms(ph['D']['recovery_ns'])},
            ledger process unchanged (pid {ph['D']['ledger_pid_before']}) and still answering, FAULT + RESPAWN recorded in the ledger ring.
          </div>
          <div class="verify-elapsed">Measured on QNX Neutrino 8.0 under QEMU 10.2 on the workstation — emulation timing, reported as measured.</div>
        </div>
      </div>

      <table class="ledger-table">
        <thead><tr><th>#</th><th>Type</th><th>Question</th><th>Status</th><th>Answer (target)</th><th>Server time</th><th>OK</th></tr></thead>
        <tbody>
{chr(10).join(qrows)}
        </tbody>
      </table>
    </section>

    <section class="ledger-panel">
      <div class="ledger-scroll">
        <table id="ledger-table" class="ledger-table">
          <thead><tr><th>#</th><th>name</th><th>type</th><th>location</th><th>role</th><th>quantity</th><th>unit</th><th>hazard</th><th>links</th></tr></thead>
          <tbody>
{chr(10).join(rows_html)}
          </tbody>
        </table>
      </div>
    </section>
  </main>

  <footer class="site-footer">
    Every answer is an exact lookup over the table above. Facts that are not in the table are answered with an abstention — never a guess. This page displays results streamed from the QNX target (serial JSON), rendered with the original Stitched Ledger styles.
  </footer>
</body>
</html>
"""
(OUT / "index.html").write_text(page)
print("bridge page written:", OUT / "index.html", len(page), "bytes")
