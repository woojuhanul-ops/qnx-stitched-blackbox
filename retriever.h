/* retriever.h - deterministic retrieval core + abstention gate.
 *
 * Port of the measured Stitched Ledger core (TypeScript) to C11.
 * Retrieval is index-based, never a per-query scan over the records:
 *   - item names are matched through an Aho-Corasick automaton built once
 *     at init (all names occurring in a text fragment are found in one pass);
 *   - location / type / role lookups go through hash indexes built at init.
 * The gate returns BB_ANSWER only with evidence coordinates, otherwise it
 * abstains: BB_ABSTAIN_NO_RECORD (nothing in the record) or
 * BB_ABSTAIN_TIMEOUT (time budget exceeded). It never guesses.
 */
#ifndef RETRIEVER_H
#define RETRIEVER_H

#include "blackbox.h"

typedef struct {
    int         status;                 /* BB_ANSWER / BB_ABSTAIN_* */
    char        answer[BB_QBUF];
    bb_evcell_t ev[BB_EV_MAX];
    int         ev_count;
    uint64_t    proc_cycles;            /* measured with the now_fn callback */
} bb_result_t;

/* Clock source callback (ClockCycles on target, any counter on host). */
typedef uint64_t (*bb_now_fn)(void);

/* Build all indexes. Call once before querying. No allocation after this. */
void bb_init(void);

/* Answer one question deterministically.
 * budget_cycles == 0 disables the time budget. now_fn may be NULL when the
 * budget is disabled. out->proc_cycles is filled when now_fn is given. */
void bb_query(const char *question, uint64_t budget_cycles,
              bb_now_fn now_fn, bb_result_t *out);

#endif /* RETRIEVER_H */
