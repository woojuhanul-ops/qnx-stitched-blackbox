/* watchdog.c - the supervisor.
 *
 * Spawns the ledger server and the mock sensor, then watches them with a
 * waitpid thread. When a child dies, only that process is revived:
 *   - sensor death  -> FAULT event appended to the ledger, sensor respawned,
 *                      recovery timed kill -> death-detect -> new-sensor READY
 *   - ledger death  -> ledger respawned (clients re-resolve its name)
 * The ledger lives in its own address space and keeps serving while the
 * sensor is down and being revived - that isolation is the demo.
 *
 * Verifier protocol: BB_W_INJECT kills the sensor with SIGKILL and the
 * reply is deferred until the replacement sensor reports READY, so the
 * reply carries the full measured recovery. BB_W_STATUS reports state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/syspage.h>
#include "blackbox.h"

static int      g_chid = -1;
static int      g_ledger_pid = -1;
static int      g_sensor_pid = -1;
static uint32_t g_faults = 0;
static uint64_t g_last_recovery = 0;

/* inject state (main loop only) */
static int      g_inject_rcvid = -1;
static uint64_t g_t_kill = 0, g_t_detect = 0;
static int      g_recovery_open = 0;    /* sensor killed, awaiting READY */
static int      g_dead_sensor_pid = -1; /* pid that died in the open recovery */

static uint64_t cps(void) { return SYSPAGE_ENTRY(qtime)->cycles_per_sec; }

static int spawn_one(const char *path, const char *name)
{
    pid_t pid = spawnl(P_NOWAIT, path, name, NULL);
    return (int)pid;
}

static void *death_thread(void *arg)
{
    int coid;
    (void)arg;
    coid = ConnectAttach(0, getpid(), g_chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) return NULL;
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid > 0) {
            bb_wpid_msg_t death;
            int rep;
            memset(&death, 0, sizeof(death));
            death.type = BB_W_DEATH;
            death.pid = (int)pid;
            MsgSend(coid, &death, sizeof(death), &rep, sizeof(rep));
        }
    }
    return NULL;
}

/* Append an event to the ledger from the watchdog; re-resolve on failure. */
static int g_ledger_coid = -1;
static void wd_append(const char *type, const char *payload)
{
    bb_append_req_t req;
    bb_append_rep_t rep;
    int tries;
    memset(&req, 0, sizeof(req));
    req.type = BB_M_APPEND;
    snprintf(req.ev_type, sizeof(req.ev_type), "%s", type);
    snprintf(req.payload, sizeof(req.payload), "%s", payload);
    for (tries = 0; tries < 2; tries++) {
        if (g_ledger_coid == -1) g_ledger_coid = name_open(BB_NAME_LEDGER, 0);
        if (g_ledger_coid == -1) return;
        if (MsgSend(g_ledger_coid, &req, sizeof(req), &rep, sizeof(rep)) != -1) return;
        ConnectDetach(g_ledger_coid);
        g_ledger_coid = -1;
    }
}

int main(void)
{
    name_attach_t *dpp;
    pthread_t th;
    union {
        uint16_t        type;
        bb_wpid_msg_t   wpid;
        bb_inject_req_t inject;
        bb_wstatus_req_t status;
    } msg;

    setvbuf(stdout, NULL, _IONBF, 0);

    dpp = name_attach(NULL, BB_NAME_WATCHDOG, 0);
    if (!dpp) { perror("WATCHDOG name_attach"); return 1; }
    g_chid = dpp->chid;

    g_ledger_pid = spawn_one("/proc/boot/ledgerd", "ledgerd");
    g_sensor_pid = spawn_one("/proc/boot/sensord", "sensord");
    printf("WATCHDOG READY pid=%d ledger=%d sensor=%d\n",
           getpid(), g_ledger_pid, g_sensor_pid);
    fflush(stdout);
    if (g_ledger_pid == -1 || g_sensor_pid == -1) {
        printf("WATCHDOG FAIL: spawn failed\n");
        return 1;
    }
    pthread_create(&th, NULL, death_thread, NULL);

    for (;;) {
        int rcvid;
        struct _msg_info info;
        /* 5 s receive timeout so a stuck recovery can be failed honestly */
        {
            uint64_t to = 5000000000ull;
            TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
        }
        rcvid = MsgReceive(g_chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) {
            if (errno == ETIMEDOUT && g_inject_rcvid != -1 && g_recovery_open &&
                (ClockCycles() - g_t_kill) > 20 * cps()) {
                bb_inject_rep_t rep;
                memset(&rep, 0, sizeof(rep));
                rep.ok = 0;
                rep.sensor_pid_old = g_dead_sensor_pid;
                rep.ledger_pid = g_ledger_pid;
                rep.cycles_per_sec = cps();
                MsgReply(g_inject_rcvid, 0, &rep, sizeof(rep));
                g_inject_rcvid = -1;
                g_recovery_open = 0;
                printf("WATCHDOG: recovery timed out, inject failed honestly\n");
            }
            continue;
        }
        if (rcvid == 0) continue;

        switch (msg.type) {
        case BB_W_READY: {
            int zero = 0;
            MsgReply(rcvid, 0, &zero, sizeof(zero));
            if (msg.wpid.pid == g_sensor_pid && g_recovery_open) {
                uint64_t t_ready = ClockCycles();
                char payload[160];
                g_last_recovery = t_ready - g_t_kill;
                snprintf(payload, sizeof(payload),
                         "sensor pid=%d recovered in %llu ns",
                         g_sensor_pid,
                         (unsigned long long)(g_last_recovery * 1000000000ull / cps()));
                wd_append("RESPAWN", payload);
                printf("WATCHDOG: sensor %d READY, recovery %llu cycles\n",
                       g_sensor_pid, (unsigned long long)g_last_recovery);
                if (g_inject_rcvid != -1) {
                    bb_inject_rep_t rep;
                    memset(&rep, 0, sizeof(rep));
                    rep.ok = 1;
                    rep.sensor_pid_old = g_dead_sensor_pid;
                    rep.sensor_pid_new = g_sensor_pid;
                    rep.ledger_pid = g_ledger_pid;
                    rep.detect_cycles = g_t_detect - g_t_kill;
                    rep.recovery_cycles = g_last_recovery;
                    rep.cycles_per_sec = cps();
                    MsgReply(g_inject_rcvid, 0, &rep, sizeof(rep));
                    g_inject_rcvid = -1;
                }
                g_recovery_open = 0;
            }
            break;
        }
        case BB_W_DEATH: {
            int zero = 0;
            int dead = msg.wpid.pid;
            MsgReply(rcvid, 0, &zero, sizeof(zero));
            if (dead == g_sensor_pid) {
                char payload[160];
                g_t_detect = ClockCycles();
                g_dead_sensor_pid = dead;
                g_faults++;
                snprintf(payload, sizeof(payload), "sensor died pid=%d", dead);
                wd_append("FAULT", payload);
                printf("WATCHDOG: sensor %d died, respawning\n", dead);
                g_sensor_pid = spawn_one("/proc/boot/sensord", "sensord");
                if (!g_recovery_open) {
                    /* unexpected death: recovery measured from detection */
                    g_t_kill = g_t_detect;
                    g_recovery_open = 1;
                }
            } else if (dead == g_ledger_pid) {
                printf("WATCHDOG: ledger %d died, respawning\n", dead);
                g_ledger_pid = spawn_one("/proc/boot/ledgerd", "ledgerd");
                if (g_ledger_coid != -1) { ConnectDetach(g_ledger_coid); g_ledger_coid = -1; }
            }
            break;
        }
        case BB_W_INJECT: {
            if (g_recovery_open || g_inject_rcvid != -1) {
                bb_inject_rep_t rep;
                memset(&rep, 0, sizeof(rep));
                rep.ok = 0;
                rep.ledger_pid = g_ledger_pid;
                rep.cycles_per_sec = cps();
                MsgReply(rcvid, 0, &rep, sizeof(rep));
                break;
            }
            g_t_kill = ClockCycles();
            g_recovery_open = 1;
            g_inject_rcvid = rcvid;         /* reply deferred until READY */
            printf("WATCHDOG: injecting fault, killing sensor %d\n", g_sensor_pid);
            kill(g_sensor_pid, SIGKILL);
            break;
        }
        case BB_W_STATUS: {
            bb_wstatus_rep_t rep;
            memset(&rep, 0, sizeof(rep));
            rep.ledger_pid = g_ledger_pid;
            rep.sensor_pid = g_sensor_pid;
            rep.faults = g_faults;
            rep.last_recovery_cycles = g_last_recovery;
            rep.cycles_per_sec = cps();
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
