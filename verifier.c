/* verifier.c - the on-target examiner.
 *
 * Runs the fixed 81-question exam against the ledger server over QNX
 * message passing and prints one JSON line per question on the console
 * (the host bridge parses these lines from the serial log):
 *
 *   Phase A   the exam itself: status / answer / evidence compared with
 *             the compiled-in expected values; per-question round-trip and
 *             server-side retrieval times measured with ClockCycles.
 *   Phase A2  a time budget is derived from the measured distribution
 *             (20 x the slowest retrieval) and the exam is re-run under
 *             that budget: outcomes must be identical, timeouts zero.
 *   Phase B   three questions with a 1-cycle budget must all come back
 *             ABSTAIN_TIMEOUT - the time gate exercised, not assumed.
 *   Phase C   determinism: every question repeated 100 times; every
 *             result must equal the Phase A result exactly.
 *   Phase D   fault injection: the watchdog kills the sensor; recovery
 *             (kill -> detect -> new sensor READY) is measured, and the
 *             ledger must still answer afterwards with FAULT and RESPAWN
 *             events recorded in its ring.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/syspage.h>
#include "blackbox.h"
#include "exam_data.h"

static uint64_t g_cps;
static int g_lcoid = -1, g_wcoid = -1;

static uint64_t to_ns(uint64_t cycles) { return cycles * 1000000000ull / g_cps; }

static int open_named(const char *name)
{
    int i, coid;
    for (i = 0; i < 150; i++) {          /* up to ~15 s */
        coid = name_open(name, 0);
        if (coid != -1) return coid;
        delay(100);
    }
    return -1;
}

static void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

static void norm(char *dst, const char *src)
{
    int n = 0, sp = 0;
    while (*src == ' ') src++;
    for (; *src; src++) {
        if (*src == ' ') { sp = 1; continue; }
        if (sp && n) dst[n++] = ' ';
        sp = 0; dst[n++] = *src;
    }
    dst[n] = '\0';
}

/* Canonical evidence string "row:col;row:col;" sorted by (row, col). */
static void ev_canon(char *dst, size_t cap, const bb_evcell_t *ev, int n)
{
    bb_evcell_t tmp[BB_EV_MAX];
    int i, j;
    size_t off = 0;
    memcpy(tmp, ev, sizeof(bb_evcell_t) * (size_t)n);
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (tmp[j].row < tmp[i].row ||
                (tmp[j].row == tmp[i].row && strcmp(tmp[j].col, tmp[i].col) < 0)) {
                bb_evcell_t t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
            }
    dst[0] = '\0';
    for (i = 0; i < n; i++)
        off += (size_t)snprintf(dst + off, cap - off, "%d:%s;", tmp[i].row, tmp[i].col);
}

static void ev_canon_expected(char *dst, size_t cap, const bb_exam_t *e)
{
    bb_evcell_t tmp[BB_EV_MAX];
    int i;
    for (i = 0; i < e->ev_count; i++) {
        tmp[i].row = e->ev[i].row;
        snprintf(tmp[i].col, sizeof(tmp[i].col), "%s", e->ev[i].col);
    }
    ev_canon(dst, cap, tmp, e->ev_count);
}

static const char *status_name(int s)
{
    return s == BB_ANSWER ? "ANSWER" :
           s == BB_ABSTAIN_TIMEOUT ? "ABSTAIN_TIMEOUT" : "ABSTAIN_NO_RECORD";
}

static int do_query(const char *question, uint64_t budget, bb_query_rep_t *rep,
                    uint64_t *rt_cycles)
{
    bb_query_req_t req;
    uint64_t t0, t1;
    memset(&req, 0, sizeof(req));
    req.type = BB_M_QUERY;
    req.budget_cycles = budget;
    snprintf(req.question, sizeof(req.question), "%s", question);
    t0 = ClockCycles();
    if (MsgSend(g_lcoid, &req, sizeof(req), rep, sizeof(*rep)) == -1) return -1;
    t1 = ClockCycles();
    if (rt_cycles) *rt_cycles = t1 - t0;
    return 0;
}

/* Phase A stored results for the determinism phase. */
static int  a_status[BB_NUM_EXAM];
static char a_answer[BB_NUM_EXAM][BB_QBUF];
static char a_ev[BB_NUM_EXAM][1024];

int main(void)
{
    int i;
    int retr_ok = 0, trap_ok = 0, ev_ok = 0;
    uint64_t proc_sum = 0, proc_max = 0, rt_sum = 0, rt_max = 0;
    uint64_t budget;
    int a2_same = 0, a2_timeouts = 0;
    int b_timeout = 0;
    long det_total = 0, det_mismatch = 0;
    int fault_ok = 0;
    uint64_t detect_ns = 0, recovery_ns = 0;
    int ledger_pid_before = -1, ledger_pid_after = -1, sensor_old = -1, sensor_new = -1;
    int events_have_fault = 0, events_have_respawn = 0, alive_after = 0;
    bb_stats_rep_t st0, st1;

    setvbuf(stdout, NULL, _IONBF, 0);
    g_cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    printf("VERIFIER START cps=%llu\n", (unsigned long long)g_cps);

    g_lcoid = open_named(BB_NAME_LEDGER);
    g_wcoid = open_named(BB_NAME_WATCHDOG);
    if (g_lcoid == -1 || g_wcoid == -1) {
        printf("{\"fatal\":\"service discovery failed\"}\n");
        return 1;
    }

    /* ---------------- Phase A: the 81-question exam ---------------- */
    for (i = 0; i < BB_NUM_EXAM; i++) {
        const bb_exam_t *e = &BB_EXAM[i];
        bb_query_rep_t rep;
        uint64_t rt = 0;
        char esc[BB_QBUF * 2], expc[1024], gotc[1024];
        char na[BB_QBUF * 2], nb[BB_QBUF * 2];
        int ok = 0, evok = 0;

        memset(&rep, 0, sizeof(rep));
        if (do_query(e->question, 0, &rep, &rt) == -1) {
            printf("{\"id\":%d,\"type\":\"%s\",\"error\":\"msgsend\"}\n", e->id, e->type);
            continue;
        }
        proc_sum += rep.proc_cycles; if (rep.proc_cycles > proc_max) proc_max = rep.proc_cycles;
        rt_sum += rt; if (rt > rt_max) rt_max = rt;
        ledger_pid_before = rep.server_pid;

        a_status[i] = rep.status;
        snprintf(a_answer[i], BB_QBUF, "%s", rep.answer);
        ev_canon(a_ev[i], sizeof(a_ev[i]), rep.ev, rep.ev_count);

        if (e->expected_status == 1) {
            ok = (rep.status == BB_ABSTAIN_NO_RECORD);
            if (ok) trap_ok++;
        } else {
            norm(na, rep.answer);
            norm(nb, e->expected_answer ? e->expected_answer : "");
            ev_canon_expected(expc, sizeof(expc), e);
            ev_canon(gotc, sizeof(gotc), rep.ev, rep.ev_count);
            evok = (strcmp(expc, gotc) == 0);
            ok = (rep.status == BB_ANSWER && strcmp(na, nb) == 0);
            if (ok) { retr_ok++; if (evok) ev_ok++; }
        }
        json_escape(rep.answer, esc, sizeof(esc));
        printf("{\"id\":%d,\"type\":\"%s\",\"status\":\"%s\",\"answer\":\"%s\","
               "\"ev\":\"%s\",\"ok\":%s,\"ev_ok\":%s,\"proc_ns\":%llu,\"rt_ns\":%llu}\n",
               e->id, e->type, status_name(rep.status), esc, a_ev[i],
               ok ? "true" : "false", evok ? "true" : "false",
               (unsigned long long)to_ns(rep.proc_cycles),
               (unsigned long long)to_ns(rt));
    }
    printf("{\"phase\":\"A\",\"retrieval\":%d,\"traps\":%d,\"evidence\":%d,"
           "\"proc_avg_ns\":%llu,\"proc_max_ns\":%llu,\"rt_avg_ns\":%llu,\"rt_max_ns\":%llu}\n",
           retr_ok, trap_ok, ev_ok,
           (unsigned long long)to_ns(proc_sum / BB_NUM_EXAM),
           (unsigned long long)to_ns(proc_max),
           (unsigned long long)to_ns(rt_sum / BB_NUM_EXAM),
           (unsigned long long)to_ns(rt_max));

    /* -------- Phase A2: budget derived from the measurement -------- */
    budget = proc_max * 20;
    for (i = 0; i < BB_NUM_EXAM; i++) {
        bb_query_rep_t rep;
        char gotc[1024];
        memset(&rep, 0, sizeof(rep));
        if (do_query(BB_EXAM[i].question, budget, &rep, NULL) == -1) continue;
        if (rep.status == BB_ABSTAIN_TIMEOUT) { a2_timeouts++; continue; }
        ev_canon(gotc, sizeof(gotc), rep.ev, rep.ev_count);
        if (rep.status == a_status[i] && strcmp(rep.answer, a_answer[i]) == 0 &&
            strcmp(gotc, a_ev[i]) == 0) a2_same++;
    }
    printf("{\"phase\":\"A2\",\"budget_cycles\":%llu,\"budget_ns\":%llu,"
           "\"identical\":%d,\"timeouts\":%d}\n",
           (unsigned long long)budget, (unsigned long long)to_ns(budget),
           a2_same, a2_timeouts);

    /* -------- Phase B: the time gate, exercised with 1 cycle -------- */
    for (i = 0; i < 3 && i < BB_NUM_EXAM; i++) {
        bb_query_rep_t rep;
        memset(&rep, 0, sizeof(rep));
        if (do_query(BB_EXAM[i].question, 1, &rep, NULL) == 0 &&
            rep.status == BB_ABSTAIN_TIMEOUT) b_timeout++;
    }
    printf("{\"phase\":\"B\",\"budget_cycles\":1,\"timeout_abstains\":%d,\"of\":3}\n",
           b_timeout);

    /* -------- Phase C: determinism, 100 repetitions of everything --- */
    {
        int rep_i, q;
        for (rep_i = 0; rep_i < 100; rep_i++) {
            for (q = 0; q < BB_NUM_EXAM; q++) {
                bb_query_rep_t rep;
                char gotc[1024];
                memset(&rep, 0, sizeof(rep));
                det_total++;
                if (do_query(BB_EXAM[q].question, 0, &rep, NULL) == -1) { det_mismatch++; continue; }
                ev_canon(gotc, sizeof(gotc), rep.ev, rep.ev_count);
                if (rep.status != a_status[q] ||
                    strcmp(rep.answer, a_answer[q]) != 0 ||
                    strcmp(gotc, a_ev[q]) != 0) det_mismatch++;
            }
        }
    }
    printf("{\"phase\":\"C\",\"repetitions\":%ld,\"mismatches\":%ld}\n",
           det_total, det_mismatch);

    /* -------- Phase D: fault injection and recovery ---------------- */
    {
        bb_stats_req_t sreq;
        bb_events_req_t ereq;
        bb_events_rep_t erep;
        bb_inject_req_t ireq;
        bb_inject_rep_t irep;

        memset(&sreq, 0, sizeof(sreq)); sreq.type = BB_M_STATS;
        MsgSend(g_lcoid, &sreq, sizeof(sreq), &st0, sizeof(st0));

        memset(&ireq, 0, sizeof(ireq)); ireq.type = BB_W_INJECT;
        memset(&irep, 0, sizeof(irep));
        if (MsgSend(g_wcoid, &ireq, sizeof(ireq), &irep, sizeof(irep)) != -1 && irep.ok) {
            fault_ok = 1;
            detect_ns = to_ns(irep.detect_cycles);
            recovery_ns = to_ns(irep.recovery_cycles);
            sensor_old = irep.sensor_pid_old;
            sensor_new = irep.sensor_pid_new;
            ledger_pid_after = irep.ledger_pid;
        }

        memset(&sreq, 0, sizeof(sreq)); sreq.type = BB_M_STATS;
        if (MsgSend(g_lcoid, &sreq, sizeof(sreq), &st1, sizeof(st1)) != -1)
            ledger_pid_after = st1.pid;

        memset(&ereq, 0, sizeof(ereq)); ereq.type = BB_M_EVENTS;
        memset(&erep, 0, sizeof(erep));
        if (MsgSend(g_lcoid, &ereq, sizeof(ereq), &erep, sizeof(erep)) != -1) {
            events_have_fault = strstr(erep.text, " FAULT ") != NULL;
            events_have_respawn = strstr(erep.text, " RESPAWN ") != NULL;
        }

        {
            bb_query_rep_t rep;
            memset(&rep, 0, sizeof(rep));
            if (do_query("Where is Sulfuric Acid located?", 0, &rep, NULL) != -1 &&
                rep.status == BB_ANSWER && strcmp(rep.answer, "Cabinet A") == 0)
                alive_after = 1;
        }
    }
    printf("{\"phase\":\"D\",\"fault_ok\":%s,\"sensor_old\":%d,\"sensor_new\":%d,"
           "\"detect_ns\":%llu,\"recovery_ns\":%llu,"
           "\"ledger_pid_before\":%d,\"ledger_pid_after\":%d,"
           "\"events_fault\":%s,\"events_respawn\":%s,\"ledger_alive_after\":%s}\n",
           fault_ok ? "true" : "false", sensor_old, sensor_new,
           (unsigned long long)detect_ns, (unsigned long long)recovery_ns,
           ledger_pid_before, ledger_pid_after,
           events_have_fault ? "true" : "false",
           events_have_respawn ? "true" : "false",
           alive_after ? "true" : "false");

    printf("{\"summary\":{\"retrieval\":\"%d/66\",\"traps\":\"%d/15\",\"evidence\":\"%d/66\","
           "\"proc_avg_ns\":%llu,\"proc_max_ns\":%llu,\"budget_ns\":%llu,"
           "\"budget_rerun_identical\":%d,\"budget_rerun_timeouts\":%d,"
           "\"timeout_gate\":\"%d/3\",\"determinism_mismatches\":%ld,"
           "\"fault_recovery_ns\":%llu}}\n",
           retr_ok, trap_ok, ev_ok,
           (unsigned long long)to_ns(proc_sum / BB_NUM_EXAM),
           (unsigned long long)to_ns(proc_max),
           (unsigned long long)to_ns(budget),
           a2_same, a2_timeouts, b_timeout, det_mismatch,
           (unsigned long long)recovery_ns);
    printf("VERIFIER DONE\n");
    return 0;
}
