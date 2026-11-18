/* ledgerd.c - the ledger server.
 *
 * Owns the record table (48 lab-ledger records, compiled in) and a static
 * ring buffer of runtime events. All storage is static: after bb_init()
 * there is no dynamic allocation anywhere in this process.
 *
 * Clients reach it through the local name service ("blackbox/ledger") and
 * speak MsgReceive/MsgReply with the protocol in blackbox.h:
 *   APPEND - push an event into the ring (seq assigned here)
 *   QUERY  - deterministic retrieval + gate (answer with evidence
 *            coordinates, or ABSTAIN_NO_RECORD / ABSTAIN_TIMEOUT)
 *   STATS  - counters; EVENTS - the retained ring tail as text
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include "blackbox.h"
#include "retriever.h"

#define RING_CAP 256

typedef struct {
    uint32_t seq;
    char     type[16];
    char     payload[160];
    uint64_t cycles;
} ring_entry_t;

static ring_entry_t g_ring[RING_CAP];
static uint32_t g_appended;     /* total appends (== last seq) */
static uint32_t g_overwritten;
static uint32_t g_queries;

static uint64_t now_cycles(void) { return ClockCycles(); }

static void ring_append(const char *type, const char *payload)
{
    ring_entry_t *e = &g_ring[g_appended % RING_CAP];
    if (g_appended >= RING_CAP) g_overwritten++;
    g_appended++;
    e->seq = g_appended;
    snprintf(e->type, sizeof(e->type), "%s", type);
    snprintf(e->payload, sizeof(e->payload), "%s", payload);
    e->cycles = ClockCycles();
}

int main(void)
{
    name_attach_t *dpp;
    union {
        uint16_t        type;
        bb_append_req_t append;
        bb_query_req_t  query;
        bb_stats_req_t  stats;
        bb_events_req_t events;
    } msg;

    setvbuf(stdout, NULL, _IONBF, 0);
    bb_init();

    dpp = name_attach(NULL, BB_NAME_LEDGER, 0);
    if (!dpp) { perror("LEDGERD name_attach"); return 1; }

    ring_append("LEDGER_UP", "ledger server start");
    printf("LEDGERD READY pid=%d chid=%d\n", getpid(), dpp->chid);
    fflush(stdout);

    for (;;) {
        int rcvid = MsgReceive(dpp->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) continue;
        if (rcvid == 0) continue;   /* pulses not used here */

        switch (msg.type) {
        case BB_M_APPEND: {
            bb_append_rep_t rep;
            ring_append(msg.append.ev_type, msg.append.payload);
            rep.status = 0;
            rep.seq = g_appended;
            MsgReply(rcvid, 0, &rep, sizeof(rep));
            break;
        }
        case BB_M_QUERY: {
            bb_result_t res;
            bb_query_rep_t rep;
            int i;
            msg.query.question[BB_QBUF - 1] = '\0';
            bb_query(msg.query.question, msg.query.budget_cycles, now_cycles, &res);
            g_queries++;
            memset(&rep, 0, sizeof(rep));
            rep.status = res.status;
            snprintf(rep.answer, sizeof(rep.answer), "%s", res.answer);
            rep.ev_count = res.ev_count;
            for (i = 0; i < res.ev_count && i < BB_EV_MAX; i++) rep.ev[i] = res.ev[i];
            rep.proc_cycles = res.proc_cycles;
            rep.server_pid = getpid();
            MsgReply(rcvid, 0, &rep, sizeof(rep));
            break;
        }
        case BB_M_STATS: {
            bb_stats_rep_t rep;
            rep.pid = getpid();
            rep.appended = g_appended;
            rep.overwritten = g_overwritten;
            rep.queries = g_queries;
            rep.ring_used = g_appended < RING_CAP ? (int)g_appended : RING_CAP;
            MsgReply(rcvid, 0, &rep, sizeof(rep));
            break;
        }
        case BB_M_EVENTS: {
            bb_events_rep_t rep;
            uint32_t retained = g_appended < RING_CAP ? g_appended : RING_CAP;
            uint32_t start = g_appended - retained;
            uint32_t i;
            size_t off = 0;
            rep.text[0] = '\0';
            for (i = start; i < g_appended; i++) {
                ring_entry_t *e = &g_ring[i % RING_CAP];
                int w = snprintf(rep.text + off, sizeof(rep.text) - off,
                                 "%u %s %s\n", e->seq, e->type, e->payload);
                if (w < 0 || (size_t)w >= sizeof(rep.text) - off) break;
                off += (size_t)w;
            }
            MsgReply(rcvid, 0, &rep, sizeof(rep));
            break;
        }
        default:
            MsgError(rcvid, ENOSYS);
            break;
        }
    }
    return 0;
}
