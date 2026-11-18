/* blackbox.h - shared types and IPC protocol for the QNX Stitched Blackbox.
 *
 * A deterministic audit & replay debugger for QNX Neutrino (SDP 8.0):
 * events are recorded in an append-only ledger with evidence coordinates,
 * queries are answered only from the record, and the gate abstains
 * (NO_RECORD / TIMEOUT) instead of guessing.
 *
 * Processes:
 *   ledgerd   - ledger server: static ring buffer + record table, MsgReceive/MsgReply IPC
 *   sensord   - mock sensor: appends heartbeat events to the ledger
 *   watchdog  - supervisor: spawns ledgerd/sensord, revives only the dead process
 *   verifier  - on-target examiner: runs the fixed 81-question exam over IPC
 */
#ifndef BLACKBOX_H
#define BLACKBOX_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Record table row (the lab ledger, 48 records)                       */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *name;
    const char *type;
    const char *location;
    const char *role;
    long        quantity;
    const char *unit;
    const char *hazard;
    const char *links_joined;   /* links joined with ", " (answer form) */
} bb_row_t;

/* ------------------------------------------------------------------ */
/* Exam item (fixed 81-question exam, same as the host reference)      */
/* ------------------------------------------------------------------ */
typedef struct { int row; const char *col; } bb_evdef_t;
typedef struct {
    int         id;
    const char *type;           /* T1..T9 or TRAP */
    const char *question;
    int         expected_status;/* 0 = ANSWER, 1 = ABSTAIN */
    const char *expected_answer;/* NULL for abstain items */
    bb_evdef_t  ev[16];
    int         ev_count;
} bb_exam_t;

/* ------------------------------------------------------------------ */
/* Query result status (gate verdicts)                                 */
/* ------------------------------------------------------------------ */
enum {
    BB_ANSWER            = 0,
    BB_ABSTAIN_NO_RECORD = 1,
    BB_ABSTAIN_TIMEOUT   = 2
};

/* ------------------------------------------------------------------ */
/* IPC protocol. Every message starts with a uint16_t type field.      */
/* ------------------------------------------------------------------ */
#define BB_NAME_LEDGER   "blackbox/ledger"
#define BB_NAME_WATCHDOG "blackbox/watchdog"

enum {
    BB_M_APPEND = 100,  /* client -> ledgerd : append an event          */
    BB_M_QUERY  = 101,  /* client -> ledgerd : deterministic query      */
    BB_M_STATS  = 102,  /* client -> ledgerd : counters                 */
    BB_M_EVENTS = 103   /* client -> ledgerd : recent ring events text  */
};

enum {
    BB_W_INJECT = 200,  /* verifier -> watchdog : kill the sensor now   */
    BB_W_READY  = 201,  /* sensord  -> watchdog : I am up               */
    BB_W_DEATH  = 202,  /* watchdog death-thread -> watchdog main loop  */
    BB_W_STATUS = 203   /* verifier -> watchdog : supervisor status     */
};

#define BB_QBUF 256     /* question / answer text capacity */
#define BB_EV_MAX 48    /* evidence cells per answer (T7 max is 14) */

typedef struct {
    uint16_t type;              /* BB_M_APPEND */
    char     ev_type[16];
    char     payload[160];
} bb_append_req_t;

typedef struct {
    int32_t  status;            /* 0 ok */
    uint32_t seq;               /* ring sequence number assigned */
} bb_append_rep_t;

typedef struct {
    uint16_t type;              /* BB_M_QUERY */
    uint64_t budget_cycles;     /* 0 = no budget; else ABSTAIN_TIMEOUT above it */
    char     question[BB_QBUF];
} bb_query_req_t;

typedef struct { int32_t row; char col[16]; } bb_evcell_t;

typedef struct {
    int32_t     status;         /* BB_ANSWER / BB_ABSTAIN_NO_RECORD / BB_ABSTAIN_TIMEOUT */
    char        answer[BB_QBUF];
    int32_t     ev_count;
    bb_evcell_t ev[BB_EV_MAX];
    uint64_t    proc_cycles;    /* server-side retrieval time (ClockCycles) */
    int32_t     server_pid;
} bb_query_rep_t;

typedef struct { uint16_t type; } bb_stats_req_t;   /* BB_M_STATS */

typedef struct {
    int32_t  pid;
    uint32_t appended;          /* total events appended */
    uint32_t overwritten;       /* ring overwrites */
    uint32_t queries;           /* queries served */
    int32_t  ring_used;         /* entries currently retained */
} bb_stats_rep_t;

typedef struct { uint16_t type; } bb_events_req_t;  /* BB_M_EVENTS */

typedef struct {
    char text[1536];            /* newline-separated "seq TYPE payload" lines */
} bb_events_rep_t;

typedef struct { uint16_t type; int32_t pid; } bb_wpid_msg_t; /* READY / DEATH */

typedef struct { uint16_t type; } bb_inject_req_t;  /* BB_W_INJECT */

typedef struct {
    int32_t  ok;
    int32_t  sensor_pid_old;
    int32_t  sensor_pid_new;
    int32_t  ledger_pid;
    uint64_t detect_cycles;     /* kill -> death detection */
    uint64_t recovery_cycles;   /* kill -> new sensor READY */
    uint64_t cycles_per_sec;
} bb_inject_rep_t;

typedef struct { uint16_t type; } bb_wstatus_req_t; /* BB_W_STATUS */

typedef struct {
    int32_t  ledger_pid;
    int32_t  sensor_pid;
    uint32_t faults;            /* sensor deaths handled */
    uint64_t last_recovery_cycles;
    uint64_t cycles_per_sec;
} bb_wstatus_rep_t;

#endif /* BLACKBOX_H */
