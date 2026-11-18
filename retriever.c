/* retriever.c - deterministic retrieval core + abstention gate (C11).
 *
 * Semantics ported 1:1 from the measured TypeScript core:
 *   findItemByName: rows whose name occurs in the fragment; ties broken by
 *   longest name; equal-length tie between distinct rows = AMBIGUOUS = miss.
 *   Templates T1..T9 tried in order; first hit wins; a miss everywhere is
 *   ABSTAIN (no record), never a guess.
 *
 * Difference from the reference implementation is only in *how* matches are
 * found: an Aho-Corasick automaton over the name set (built once in bb_init)
 * replaces the per-query substring scan, and hash indexes replace the field
 * scans for T6/T7/T9. The match *results* are identical by construction.
 */
#include <string.h>
#include <stdio.h>
#include "retriever.h"
#include "ledger_data.h"

/* ================================================================== */
/* Aho-Corasick automaton over the record names                        */
/* ================================================================== */
#define AC_MAX_NODES (BB_NAME_TOTAL_CHARS + 1)
#define AC_MAX_EDGES (BB_NAME_TOTAL_CHARS + 1)

static int  ac_next_head[AC_MAX_NODES];     /* first edge index or -1 */
static int  ac_fail[AC_MAX_NODES];
static int  ac_out_row[AC_MAX_NODES];       /* row whose name ends here, -1 */
static int  ac_dict_link[AC_MAX_NODES];     /* nearest fail-ancestor with out, -1 */
static char ac_edge_ch[AC_MAX_EDGES];
static int  ac_edge_to[AC_MAX_EDGES];
static int  ac_edge_next[AC_MAX_EDGES];
static int  ac_num_nodes;
static int  ac_num_edges;

static int ac_find_edge(int node, char c)
{
    int e;
    for (e = ac_next_head[node]; e != -1; e = ac_edge_next[e])
        if (ac_edge_ch[e] == c) return ac_edge_to[e];
    return -1;
}

static int ac_add_edge(int node, char c)
{
    int e = ac_num_edges++;
    ac_edge_ch[e] = c;
    ac_edge_to[e] = ac_num_nodes++;
    ac_edge_next[e] = ac_next_head[node];
    ac_next_head[node] = e;
    ac_next_head[ac_edge_to[e]] = -1;
    ac_fail[ac_edge_to[e]] = 0;
    ac_out_row[ac_edge_to[e]] = -1;
    ac_dict_link[ac_edge_to[e]] = -1;
    return ac_edge_to[e];
}

static void ac_build(void)
{
    int i, qh = 0, qt = 0;
    int queue[AC_MAX_NODES];

    ac_num_nodes = 1;               /* node 0 = root */
    ac_num_edges = 0;
    ac_next_head[0] = -1;
    ac_fail[0] = 0;
    ac_out_row[0] = -1;
    ac_dict_link[0] = -1;

    /* Insert every record name; terminal node remembers its row. */
    for (i = 0; i < BB_NUM_ROWS; i++) {
        const char *s = BB_ROWS[i].name;
        int node = 0;
        for (; *s; s++) {
            int nxt = ac_find_edge(node, *s);
            if (nxt == -1) nxt = ac_add_edge(node, *s);
            node = nxt;
        }
        ac_out_row[node] = i;
    }

    /* Failure links, breadth-first. */
    {
        int e;
        for (e = ac_next_head[0]; e != -1; e = ac_edge_next[e]) {
            ac_fail[ac_edge_to[e]] = 0;
            queue[qt++] = ac_edge_to[e];
        }
    }
    while (qh < qt) {
        int node = queue[qh++];
        int e;
        /* dict link: fail node itself if it ends a name, else its dict link */
        int f = ac_fail[node];
        ac_dict_link[node] = (ac_out_row[f] != -1) ? f : ac_dict_link[f];
        for (e = ac_next_head[node]; e != -1; e = ac_edge_next[e]) {
            int child = ac_edge_to[e];
            char c = ac_edge_ch[e];
            int ff = ac_fail[node];
            int tgt;
            while (ff != 0 && ac_find_edge(ff, c) == -1) ff = ac_fail[ff];
            tgt = ac_find_edge(ff, c);
            ac_fail[child] = (tgt != -1) ? tgt : 0;
            queue[qt++] = child;
        }
    }
}

/* Mark every row whose name occurs in `text`. Returns match count. */
static int ac_match(const char *text, unsigned char matched[BB_NUM_ROWS])
{
    int node = 0, count = 0;
    const char *p;
    memset(matched, 0, BB_NUM_ROWS);
    for (p = text; *p; p++) {
        int tgt;
        while (node != 0 && ac_find_edge(node, *p) == -1) node = ac_fail[node];
        tgt = ac_find_edge(node, *p);
        node = (tgt != -1) ? tgt : 0;
        /* collect outputs: this node, then dict-link ancestors */
        {
            int n;
            for (n = node; n != -1; n = ac_dict_link[n]) {
                if (ac_out_row[n] != -1 && !matched[ac_out_row[n]]) {
                    matched[ac_out_row[n]] = 1;
                    count++;
                }
                if (n == 0) break;
            }
        }
    }
    return count;
}

/* ================================================================== */
/* Hash indexes for location / type / role (T6 / T7 / T9)              */
/* ================================================================== */
#define FH_SLOTS 128
typedef struct { const char *key; int head; } fh_slot_t;
static fh_slot_t fh_loc[FH_SLOTS], fh_type[FH_SLOTS], fh_role[FH_SLOTS];
static int fh_loc_next[BB_NUM_ROWS], fh_type_next[BB_NUM_ROWS], fh_role_next[BB_NUM_ROWS];

static unsigned fh_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h & (FH_SLOTS - 1);
}

static void fh_insert(fh_slot_t *tab, int *nextarr, const char *key, int row)
{
    unsigned h = fh_hash(key);
    while (tab[h].key && strcmp(tab[h].key, key) != 0) h = (h + 1) & (FH_SLOTS - 1);
    if (!tab[h].key) { tab[h].key = key; tab[h].head = -1; }
    nextarr[row] = tab[h].head;
    tab[h].head = row;
}

/* Collect rows whose field equals key. Returns count (rows in chain order). */
static int fh_lookup(const fh_slot_t *tab, const int *nextarr, const char *key,
                     int rows_out[BB_NUM_ROWS])
{
    unsigned h = fh_hash(key);
    int r, n = 0;
    while (tab[h].key && strcmp(tab[h].key, key) != 0) h = (h + 1) & (FH_SLOTS - 1);
    if (!tab[h].key) return 0;
    for (r = tab[h].head; r != -1; r = nextarr[r]) rows_out[n++] = r;
    return n;
}

/* ================================================================== */
/* Init                                                                */
/* ================================================================== */
void bb_init(void)
{
    int i;
    ac_build();
    for (i = 0; i < BB_NUM_ROWS; i++) {
        fh_insert(fh_loc,  fh_loc_next,  BB_ROWS[i].location, i);
        fh_insert(fh_type, fh_type_next, BB_ROWS[i].type, i);
        fh_insert(fh_role, fh_role_next, BB_ROWS[i].role, i);
    }
}

/* ================================================================== */
/* Item finding (findItemByName semantics)                             */
/* ================================================================== */
#define ROW_NONE (-1)
#define ROW_AMBIGUOUS (-2)

static int find_row(const char *fragment)
{
    unsigned char matched[BB_NUM_ROWS];
    int count = ac_match(fragment, matched);
    int i, best = ROW_NONE, bestlen = -1, ties = 0;
    if (count == 0) return ROW_NONE;
    for (i = 0; i < BB_NUM_ROWS; i++) {
        if (!matched[i]) continue;
        int len = (int)strlen(BB_ROWS[i].name);
        if (len > bestlen) { bestlen = len; best = i; ties = 1; }
        else if (len == bestlen) { ties++; }
    }
    if (ties != 1) return ROW_AMBIGUOUS;
    return best;
}

/* ================================================================== */
/* Template helpers                                                    */
/* ================================================================== */
/* Anchored prefix/suffix match; on success mid/midlen locate the capture. */
static int affix(const char *q, const char *prefix, const char *suffix,
                 const char **mid, size_t *midlen)
{
    size_t ql = strlen(q), pl = strlen(prefix), sl = strlen(suffix);
    if (ql < pl + sl + 1) return 0;             /* capture needs >= 1 char */
    if (strncmp(q, prefix, pl) != 0) return 0;
    if (strcmp(q + ql - sl, suffix) != 0) return 0;
    *mid = q + pl;
    *midlen = ql - pl - sl;
    return 1;
}

static void copy_mid(char *dst, size_t dstcap, const char *mid, size_t midlen)
{
    if (midlen >= dstcap) midlen = dstcap - 1;
    memcpy(dst, mid, midlen);
    dst[midlen] = '\0';
}

static void set_answer(bb_result_t *out, const char *answer)
{
    out->status = BB_ANSWER;
    snprintf(out->answer, sizeof(out->answer), "%s", answer);
}

static void add_ev(bb_result_t *out, int rowidx, const char *col)
{
    if (out->ev_count >= BB_EV_MAX) return;
    out->ev[out->ev_count].row = rowidx + 1;    /* evidence rows are 1-based */
    snprintf(out->ev[out->ev_count].col, sizeof(out->ev[0].col), "%s", col);
    out->ev_count++;
}

/* Each matcher returns 1 when it produced an ANSWER, else 0 (miss). */
static int m_t1(const char *q, bb_result_t *out)
{
    const char *mid; size_t ml; char frag[BB_QBUF]; int r;
    if (!affix(q, "Where is ", " located?", &mid, &ml)) return 0;
    copy_mid(frag, sizeof(frag), mid, ml);
    r = find_row(frag);
    if (r < 0) return 0;
    set_answer(out, BB_ROWS[r].location);
    add_ev(out, r, "location");
    return 1;
}

static int m_t2(const char *q, bb_result_t *out)
{
    const char *mid; size_t ml; char frag[BB_QBUF]; int r;
    if (!affix(q, "What is the role of ", "?", &mid, &ml)) return 0;
    copy_mid(frag, sizeof(frag), mid, ml);
    r = find_row(frag);
    if (r < 0) return 0;
    set_answer(out, BB_ROWS[r].role);
    add_ev(out, r, "role");
    return 1;
}

static int m_t3(const char *q, bb_result_t *out)
{
    const char *mid; size_t ml; char frag[BB_QBUF]; int r;
    char buf[BB_QBUF];
    if (!affix(q, "What is the quantity of ", "?", &mid, &ml)) return 0;
    copy_mid(frag, sizeof(frag), mid, ml);
    r = find_row(frag);
    if (r < 0) return 0;
    snprintf(buf, sizeof(buf), "%ld %s", BB_ROWS[r].quantity, BB_ROWS[r].unit);
    set_answer(out, buf);
    add_ev(out, r, "quantity");
    return 1;
}

static int m_t4(const char *q, bb_result_t *out)
{
    const char *mid; size_t ml; char frag[BB_QBUF]; int r;
    if (!affix(q, "What is the hazard class of ", "?", &mid, &ml)) return 0;
    copy_mid(frag, sizeof(frag), mid, ml);
    r = find_row(frag);
    if (r < 0) return 0;
    set_answer(out, BB_ROWS[r].hazard);
    add_ev(out, r, "hazard");
    return 1;
}

static int m_t5(const char *q, bb_result_t *out)
{
    const char *mid; size_t ml; char frag[BB_QBUF]; int r;
    if (!affix(q, "What is ", " connected to?", &mid, &ml)) return 0;
    copy_mid(frag, sizeof(frag), mid, ml);
    r = find_row(frag);
    if (r < 0) return 0;
    set_answer(out, BB_ROWS[r].links_joined);
    add_ev(out, r, "links");
    return 1;
}

static int m_t6(const char *q, bb_result_t *out)
{
    const char *mid; size_t ml; char key[BB_QBUF];
    int rows[BB_NUM_ROWS], n, i;
    char buf[32];
    if (!affix(q, "How many items are located in ", "?", &mid, &ml)) return 0;
    copy_mid(key, sizeof(key), mid, ml);
    n = fh_lookup(fh_loc, fh_loc_next, key, rows);
    if (n == 0) return 0;
    snprintf(buf, sizeof(buf), "%d", n);
    set_answer(out, buf);
    for (i = 0; i < n; i++) add_ev(out, rows[i], "location");
    return 1;
}

static int m_t7(const char *q, bb_result_t *out)
{
    const char *mid; size_t ml; char key[BB_QBUF];
    int rows[BB_NUM_ROWS], n, i;
    char buf[32];
    if (!affix(q, "How many items are of type ", "?", &mid, &ml)) return 0;
    copy_mid(key, sizeof(key), mid, ml);
    n = fh_lookup(fh_type, fh_type_next, key, rows);
    if (n == 0) return 0;
    snprintf(buf, sizeof(buf), "%d", n);
    set_answer(out, buf);
    for (i = 0; i < n; i++) add_ev(out, rows[i], "type");
    return 1;
}

static int m_t8(const char *q, bb_result_t *out)
{
    const char *mid; size_t ml;
    char a[BB_QBUF], b[BB_QBUF];
    const char *sep = NULL, *p;
    int ra, rb;
    if (!affix(q, "Which has the greater quantity, ", "?", &mid, &ml)) return 0;
    /* JS regex group 1 is greedy: split at the LAST " or ". */
    for (p = mid; p + 4 <= mid + ml; p++)
        if (strncmp(p, " or ", 4) == 0) sep = p;
    if (!sep) return 0;
    copy_mid(a, sizeof(a), mid, (size_t)(sep - mid));
    copy_mid(b, sizeof(b), sep + 4, ml - (size_t)(sep - mid) - 4);
    ra = find_row(a);
    rb = find_row(b);
    if (ra < 0 || rb < 0) return 0;
    if (BB_ROWS[ra].quantity == BB_ROWS[rb].quantity) return 0;  /* tied */
    set_answer(out, BB_ROWS[BB_ROWS[ra].quantity > BB_ROWS[rb].quantity ? ra : rb].name);
    add_ev(out, ra, "quantity");
    add_ev(out, rb, "quantity");
    return 1;
}

static int m_t9(const char *q, bb_result_t *out)
{
    const char *mid; size_t ml; char key[BB_QBUF];
    int rows[BB_NUM_ROWS], n;
    if (!affix(q, "Which item has the role \"", "\"?", &mid, &ml)) return 0;
    copy_mid(key, sizeof(key), mid, ml);
    n = fh_lookup(fh_role, fh_role_next, key, rows);
    if (n != 1) return 0;               /* zero or several: no record */
    set_answer(out, BB_ROWS[rows[0]].name);
    add_ev(out, rows[0], "role");
    return 1;
}

/* ================================================================== */
/* The gate                                                            */
/* ================================================================== */
void bb_query(const char *question, uint64_t budget_cycles,
              bb_now_fn now_fn, bb_result_t *out)
{
    uint64_t t0 = 0;
    int hit = 0;
    memset(out, 0, sizeof(*out));
    out->status = BB_ABSTAIN_NO_RECORD;
    if (now_fn) t0 = now_fn();

#define BB_TRY(fn) do { \
        if (!hit) { \
            hit = fn(question, out); \
            if (!hit && budget_cycles && now_fn && \
                (now_fn() - t0) > budget_cycles) { \
                out->status = BB_ABSTAIN_TIMEOUT; \
                out->answer[0] = '\0'; out->ev_count = 0; \
                if (now_fn) out->proc_cycles = now_fn() - t0; \
                return; \
            } \
        } \
    } while (0)

    BB_TRY(m_t1); BB_TRY(m_t2); BB_TRY(m_t3); BB_TRY(m_t4); BB_TRY(m_t5);
    BB_TRY(m_t6); BB_TRY(m_t7); BB_TRY(m_t8); BB_TRY(m_t9);
#undef BB_TRY

    if (now_fn) out->proc_cycles = now_fn() - t0;
    if (hit && budget_cycles && now_fn && out->proc_cycles > budget_cycles) {
        /* Answered, but too late: the gate abstains on time instead. */
        out->status = BB_ABSTAIN_TIMEOUT;
        out->answer[0] = '\0';
        out->ev_count = 0;
        return;
    }
    if (!hit) out->status = BB_ABSTAIN_NO_RECORD;
}
