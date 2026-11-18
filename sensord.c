/* sensord.c - the mock sensor.
 *
 * Appends a SENSOR_UP event and then a HEARTBEAT event every 500 ms to the
 * ledger, and announces itself to the watchdog once it is up. It exists to
 * be killed: the fault-injection demo terminates this process (and only
 * this process) and watches the watchdog revive it while the ledger keeps
 * serving from its own address space.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include "blackbox.h"

static int ledger_append(int coid, const char *type, const char *payload)
{
    bb_append_req_t req;
    bb_append_rep_t rep;
    memset(&req, 0, sizeof(req));
    req.type = BB_M_APPEND;
    snprintf(req.ev_type, sizeof(req.ev_type), "%s", type);
    snprintf(req.payload, sizeof(req.payload), "%s", payload);
    return MsgSend(coid, &req, sizeof(req), &rep, sizeof(rep));
}

static int open_named(const char *name)
{
    int i, coid;
    for (i = 0; i < 100; i++) {          /* up to ~10 s of retries */
        coid = name_open(name, 0);
        if (coid != -1) return coid;
        delay(100);
    }
    return -1;
}

int main(void)
{
    int lcoid, wcoid, tick = 0;
    char payload[160];

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("SENSORD START pid=%d\n", getpid());

    lcoid = open_named(BB_NAME_LEDGER);
    if (lcoid == -1) { printf("SENSORD FAIL: no ledger\n"); return 1; }

    snprintf(payload, sizeof(payload), "sensor up pid=%d", getpid());
    ledger_append(lcoid, "SENSOR_UP", payload);

    /* Announce readiness to the watchdog (used for recovery timing). */
    wcoid = open_named(BB_NAME_WATCHDOG);
    if (wcoid != -1) {
        bb_wpid_msg_t ready;
        int rep;
        memset(&ready, 0, sizeof(ready));
        ready.type = BB_W_READY;
        ready.pid = getpid();
        MsgSend(wcoid, &ready, sizeof(ready), &rep, sizeof(rep));
        ConnectDetach(wcoid);
    }

    for (;;) {
        delay(500);
        tick++;
        snprintf(payload, sizeof(payload), "tick %d pid=%d", tick, getpid());
        if (ledger_append(lcoid, "HEARTBEAT", payload) == -1) {
            /* Ledger may have been restarted: re-resolve and retry once. */
            ConnectDetach(lcoid);
            lcoid = open_named(BB_NAME_LEDGER);
            if (lcoid == -1) { printf("SENSORD FAIL: ledger lost\n"); return 1; }
            ledger_append(lcoid, "HEARTBEAT", payload);
        }
    }
    return 0;
}
