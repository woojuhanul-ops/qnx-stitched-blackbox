/* Host-side logic check for the retrieval core (checker pass on the VM).
 * NOT the target measurement - it only proves the C port reproduces the
 * reference semantics before target cycles are spent. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../retriever.h"
#include "../exam_data.h"

static uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void norm(char *dst, const char *src) {
    int n = 0, sp = 0;
    while (*src == ' ') src++;
    for (; *src; src++) {
        if (*src == ' ') { sp = 1; continue; }
        if (sp && n) dst[n++] = ' ';
        sp = 0; dst[n++] = *src;
    }
    dst[n] = 0;
}
static int ev_match(const bb_result_t *r, const bb_exam_t *e) {
    if (r->ev_count != e->ev_count) return 0;
    int used[16] = {0};
    for (int i = 0; i < r->ev_count; i++) {
        int found = 0;
        for (int j = 0; j < e->ev_count; j++) {
            if (!used[j] && e->ev[j].row == r->ev[i].row && strcmp(e->ev[j].col, r->ev[i].col) == 0) { used[j] = 1; found = 1; break; }
        }
        if (!found) return 0;
    }
    return 1;
}
int main(void) {
    bb_init();
    int retr_ok = 0, retr_tot = 0, trap_ok = 0, trap_tot = 0, ev_ok = 0;
    uint64_t tot = 0, mx = 0;
    for (int i = 0; i < BB_NUM_EXAM; i++) {
        const bb_exam_t *e = &BB_EXAM[i];
        bb_result_t r;
        bb_query(e->question, 0, now_ns, &r);
        tot += r.proc_cycles; if (r.proc_cycles > mx) mx = r.proc_cycles;
        if (e->expected_status == 1) {
            trap_tot++;
            if (r.status == BB_ABSTAIN_NO_RECORD) trap_ok++;
            else printf("TRAP FAIL id=%d status=%d ans=%s q=%s\n", e->id, r.status, r.answer, e->question);
        } else {
            retr_tot++;
            char a[512], b[512]; norm(a, r.answer); norm(b, e->expected_answer ? e->expected_answer : "");
            if (r.status == BB_ANSWER && strcmp(a, b) == 0) {
                retr_ok++;
                if (ev_match(&r, e)) ev_ok++;
                else printf("EV FAIL id=%d q=%s\n", e->id, e->question);
            } else printf("RETR FAIL id=%d status=%d got=%s want=%s q=%s\n", e->id, r.status, r.answer, e->expected_answer, e->question);
        }
    }
    printf("HOST LOGIC CHECK: retrieval %d/%d, traps %d/%d, evidence %d/%d, avg %.3f us, max %.3f us (host ns, logic check only)\n",
           retr_ok, retr_tot, trap_ok, trap_tot, ev_ok, retr_tot,
           (double)tot / BB_NUM_EXAM / 1000.0, (double)mx / 1000.0);
    /* timeout gate logic: budget of 1 ns must always abstain TIMEOUT */
    int to = 0;
    for (int i = 0; i < BB_NUM_EXAM; i++) { bb_result_t r; bb_query(BB_EXAM[i].question, 1, now_ns, &r); if (r.status == BB_ABSTAIN_TIMEOUT) to++; }
    printf("TIMEOUT GATE (budget=1ns): %d/%d abstained TIMEOUT\n", to, BB_NUM_EXAM);
    return 0;
}
