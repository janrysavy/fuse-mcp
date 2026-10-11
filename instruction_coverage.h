/* SPDX-License-Identifier: MIT
 * Passive, bounded dispatch evidence. Call only on the CPU owner thread.
 * Fetch hooks receive values from the existing read, never read guest memory.
 * Aggregate exact keys; hash collisions are resolved by comparing every byte.
 */
#ifndef MZ800_INSTRUCTION_COVERAGE_V2_H
#define MZ800_INSTRUCTION_COVERAGE_V2_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZIC_MAX_FETCH 16
#define ZIC_FRAGMENT 1u
#define ZIC_OVERRIDE 2u
#define ZIC_SEMANTIC_OVERRIDE 8u
#define ZIC_SEMANTIC_TAPE_SAVE 1u
#define ZIC_SEMANTIC_TAPE_LOAD 2u
#define ZIC_SEMANTIC_SLT 4u
#define ZIC_SEMANTIC_LOADER_ACCELERATION 8u
#define ZIC_SEMANTIC_CMTHACK_FILE 16u
#define ZIC_SEMANTIC_CMTHACK_BODY 32u
#define ZIC_SEMANTIC_KINDS 63u
/* JSON consumers in both emulators have signed 64-bit integer support. */
#define ZIC_COUNTER_LIMIT ((uint64_t)INT64_MAX)

typedef struct {
    uint16_t pc, next_pc, m1_mask;
    uint8_t length, flags, semantic_kinds;
    int8_t outcome; /* -1 no decision observed, 0 false, 1 true */
    uint8_t bytes[ZIC_MAX_FETCH];
} zic_key;

typedef struct {
    zic_key key;
    uint64_t count, first_dispatch, last_dispatch;
} zic_row;

typedef struct {
    int enabled, pending, malformed, counter_overflow;
    uint32_t capacity, unique;
    uint64_t units, dropped, capacity_failures, fetch_failures, decision_failures;
    uint64_t semantic_override_units, semantic_kind_units[6], opcode_override_failures;
    zic_key key;
    zic_row *rows;
    uint32_t *slots; /* zero empty, otherwise row index + 1 */
} zic_state;

static inline void zic_destroy(zic_state *s) {
    if (!s) return;
    free(s->rows); free(s->slots); free(s);
}

static inline zic_state *zic_create(uint32_t capacity) {
    zic_state *s;
    if (capacity < 2 || capacity > 1048576 || (capacity & (capacity-1))) return NULL;
    s=(zic_state *)calloc(1,sizeof(*s));
    if (!s) return NULL;
    s->capacity=capacity;
    s->rows=(zic_row *)calloc(capacity,sizeof(*s->rows));
    s->slots=(uint32_t *)calloc(2*capacity,sizeof(*s->slots));
    if (!s->rows || !s->slots) { zic_destroy(s); return NULL; }
    return s;
}

static inline void zic_reset(zic_state *s) {
    if (!s) return;
    s->pending=s->malformed=0; s->unique=0;
    s->units=s->dropped=s->capacity_failures=s->fetch_failures=s->decision_failures=0;
    memset(&s->key,0,sizeof(s->key));
    s->counter_overflow=0; s->semantic_override_units=s->opcode_override_failures=0;
    memset(s->semantic_kind_units,0,sizeof(s->semantic_kind_units));
    memset(s->slots,0,2*s->capacity*sizeof(*s->slots));
}

/* Saturate, invalidate exact accounting, and stop adding units until reset. */
static inline int zic_increment(zic_state *s,uint64_t *value) {
    if (*value >= ZIC_COUNTER_LIMIT) {
        *value=ZIC_COUNTER_LIMIT; s->counter_overflow=1; return 0;
    }
    ++*value; return 1;
}

static inline int zic_count_unit(zic_state *s) {
    if (!zic_increment(s,&s->units)) return 0;
    if (s->key.semantic_kinds) {
        zic_increment(s,&s->semantic_override_units);
        for (unsigned i=0;i<6;i++) if (s->key.semantic_kinds & (1u<<i))
            zic_increment(s,&s->semantic_kind_units[i]);
    }
    return !s->counter_overflow;
}

static inline void zic_semantic_override(zic_state *s,uint8_t kinds) {
    if (!s || !s->enabled || !s->pending) return;
    if (!kinds || (kinds & ~ZIC_SEMANTIC_KINDS)) { s->malformed=1; return; }
    s->key.semantic_kinds |= kinds;
    s->key.flags |= ZIC_SEMANTIC_OVERRIDE;
}

static inline void zic_begin(zic_state *s,uint16_t pc) {
    if (!s || !s->enabled || s->counter_overflow) return;
    if (s->pending) {
        if (zic_count_unit(s)) {
            zic_increment(s,&s->dropped); zic_increment(s,&s->fetch_failures);
        }
        s->pending=0;
        if (s->counter_overflow) return;
    }
    memset(&s->key,0,sizeof(s->key));
    s->key.pc=pc; s->key.outcome=-1; s->pending=1; s->malformed=0;
}

static inline uint8_t zic_fetch(zic_state *s,uint16_t address,uint8_t value,int m1) {
    if (s && s->enabled && s->pending) {
        unsigned n=s->key.length;
        if (n >= ZIC_MAX_FETCH || address != (uint16_t)(s->key.pc+n)) s->malformed=1;
        else {
            s->key.bytes[n]=value;
            s->key.m1_mask |= (uint16_t)((m1 != 0) << n);
            s->key.length++;
        }
    }
    return value;
}

static inline int zic_decision(zic_state *s,int taken) {
    if (s && s->enabled && s->pending) s->key.outcome=(int8_t)(taken != 0);
    return taken != 0; /* Use the SAME decision in the CPU handler. */
}

static inline void zic_flag(zic_state *s,unsigned flags) {
    if (s && s->enabled && s->pending) s->key.flags |= (uint8_t)flags;
}

static inline int zic_needs_decision(const zic_key *k) {
    unsigned i=0; uint8_t op;
    if (k->flags & ZIC_FRAGMENT) return 0;
    while (i < k->length && (k->bytes[i]==0xdd || k->bytes[i]==0xfd)) i++;
    if (i==k->length) return 0;
    op=k->bytes[i];
    if (op==0xed) return i+1 < k->length && (k->bytes[i+1]&0xf4)==0xb0;
    return op==0x10 || (op&0xe7)==0x20 ||
        (op&0xc7)==0xc0 || (op&0xc7)==0xc2 || (op&0xc7)==0xc4;
}

static inline uint32_t zic_hash(const zic_key *k) {
    uint32_t h=2166136261u;
#define ZIC_HASH_BYTE(v) do { h=(h^(uint8_t)(v))*16777619u; } while(0)
    ZIC_HASH_BYTE(k->pc); ZIC_HASH_BYTE(k->pc>>8);
    ZIC_HASH_BYTE(k->next_pc); ZIC_HASH_BYTE(k->next_pc>>8);
    ZIC_HASH_BYTE(k->m1_mask); ZIC_HASH_BYTE(k->m1_mask>>8);
    ZIC_HASH_BYTE(k->length); ZIC_HASH_BYTE(k->flags); ZIC_HASH_BYTE(k->semantic_kinds); ZIC_HASH_BYTE(k->outcome);
    for (unsigned i=0;i<k->length;i++) ZIC_HASH_BYTE(k->bytes[i]);
#undef ZIC_HASH_BYTE
    return h;
}

static inline int zic_equal(const zic_key *a,const zic_key *b) {
    return a->pc==b->pc && a->next_pc==b->next_pc && a->m1_mask==b->m1_mask &&
        a->length==b->length && a->flags==b->flags && a->semantic_kinds==b->semantic_kinds && a->outcome==b->outcome &&
        !memcmp(a->bytes,b->bytes,a->length);
}

static inline void zic_finish(zic_state *s,uint16_t next_pc) {
    uint32_t slot,mask; zic_row *row;
    if (!s || !s->enabled || !s->pending) return;
    s->pending=0; s->key.next_pc=next_pc;
    if (s->counter_overflow || !zic_count_unit(s)) return;
    if (s->key.flags & ZIC_OVERRIDE) {
        zic_increment(s,&s->dropped); zic_increment(s,&s->opcode_override_failures); return;
    }
    if (s->malformed || !s->key.length ||
        !!(s->key.flags & ZIC_SEMANTIC_OVERRIDE) != !!s->key.semantic_kinds) {
        zic_increment(s,&s->dropped); zic_increment(s,&s->fetch_failures); return;
    }
    if (!s->key.semantic_kinds && zic_needs_decision(&s->key) && s->key.outcome<0) {
        zic_increment(s,&s->dropped); zic_increment(s,&s->decision_failures); return;
    }
    mask=2*s->capacity-1; slot=zic_hash(&s->key)&mask;
    while (s->slots[slot]) {
        row=&s->rows[s->slots[slot]-1];
        if (zic_equal(&s->key,&row->key)) { if (zic_increment(s,&row->count)) row->last_dispatch=s->units; return; }
        slot=(slot+1)&mask;
    }
    if (s->unique==s->capacity) { zic_increment(s,&s->dropped); zic_increment(s,&s->capacity_failures); return; }
    row=&s->rows[s->unique]; memset(row,0,sizeof(*row)); row->key=s->key;
    row->count=1; row->first_dispatch=row->last_dispatch=s->units;
    s->slots[slot]=++s->unique;
}

static inline int zic_complete(const zic_state *s) {
    return s && !s->pending && !s->dropped && !s->counter_overflow;
}
#endif
