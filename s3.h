#ifndef S3_H
#define S3_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// we need to know about overflows, hence the smaller base
#define BASE 100000000

#define FATAL(...)                    \
    do {                              \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n");        \
        abort();                      \
    } while (0)

#define BATCH_FATHER_SIZE 60

struct obj;
typedef int32_t char_t;

int lstrcmp(const char_t *s, const char_t *t);
long lstrlen(const char_t *s);
char_t utf8_getc(FILE *f);
void utf8_putc(FILE *f, char_t c);

enum stackvar_type_t {
    T_FIXNUM = 1,
    T_FLONUM,
    T_SYMBOL,
    T_CHARACTER,
    T_BOOLEAN,
    T_PRIMITIVE,
    T_EOF,
    T_NIL,
    T_UNBOUND,
    T_PTR,
};

struct obj;  // heap-allocated objects

typedef struct ptr {
    enum stackvar_type_t type;
    union {
        int64_t fixnum;
        long double flonum;
        long symbol;
        char_t character;
        int boolean;
        int primitive;
        // common to all pointer types
        struct obj *pointer;
    };
} ptr;

ptr make_fixnum(int64_t x);
ptr make_flonum(long double x);
ptr make_char(char_t x);
ptr make_bool(int x);
ptr make_primitive(int x);
ptr make_pointer(struct obj *p);
ptr make_eof();
ptr make_nil();
ptr make_unbound();

enum heapvar_type_t {
    H_BIGINT = 1,
    H_RATIONAL,
    H_COMPLEX,
    H_PAIR,
    H_VECTOR,
    H_BYTEVECTOR,
    H_STRING,
    H_ENVIRONMENT,
    H_ACTIVATION_RECORD,
    H_PROCEDURE,
    H_MACRO,
    H_TRANSFORMER,
    H_STRUCT,
    H_CODE,
};

enum opcode_t {
    O_JUMP,
    O_LOAD,
    O_CREATE_ACTIVATION_RECORD,
};

typedef struct instruction {
    enum opcode_t opcode;
    ptr operand[4];
    void (*func)(ptr, struct instruction *);
} instruction;

typedef struct obj {
    enum heapvar_type_t type;
    size_t size;
    char age, mark, moved;
    struct obj *forward;
    union {
        // bigint
        struct {
            long bigint_size, sign;
            uint64_t digits[1];
        };
        // rational
        struct {
            ptr numerator, denominator;
        };
        // complex
        struct {
            ptr real, imaginary;
        };
        // pair
        struct {
            ptr car, cdr;
        };
        // vector
        struct {
            long vector_size;
            ptr vector[1];
        };
        // bytevector
        struct {
            long bytevector_size;
            uint8_t bytes[1];
        };
        // string
        struct {
            long string_size;
            char_t string[1];
        };
        // environment
        struct {
            ptr batch_env_father[BATCH_FATHER_SIZE];
            long env_size;
            ptr entry[1];
        };
        // activation record
        struct {
            ptr batch_ar_father[BATCH_FATHER_SIZE];
            long ar_size;
            ptr value[1];
        };
        // procedure
        struct {
            ptr formals, p_env, body, code;
        };
        // macro
        struct {
            long macro_transformers_count;
            ptr transformers[1];
        };
        // transformer
        struct {
            ptr t_env, pattern, template;
        };
        // struct
        struct {
            long id, struct_size;
            ptr field[1];
        };
        // code
        struct {
            long code_size;
            instruction instructions[1];
        };
    };
} obj;

// we use a hash table from string to index for our obarray
#define OBARRAY_HASH_P 10007
#define OBARRAY_HASH_E 307

typedef struct obarray_node_t {
    char_t *s;
    long index;
    struct obarray_node_t *next;
} obarray_node_t;

typedef struct obarray_t {
    obarray_node_t *heads[OBARRAY_HASH_P];
    long count;
} obarray_t;

void obarray_init(obarray_t *obarray);
ptr obarray_intern(obarray_t *obarray, const char_t *s);

typedef struct ptr_move_transform_t {
    uint8_t *from, *to;
    size_t size;
} ptr_move_transform_t;

ptr_move_transform_t make_transform(uint8_t *from, uint8_t *to, size_t size);
uint8_t *apply_transform(ptr_move_transform_t t, uint8_t *p);
obj *apply_transform_obj(ptr_move_transform_t t, obj *p);

#define GC_THRESHOLD_AGE 8
#define GC_INITIAL_SIZE (1 << 20)
#define GC_OLD_TO_YOUNG_RATIO 2
#define GC_GROW_RATIO 2
#define GC_ALIGNMENT (sizeof(intmax_t))
#define HASH_SIZE 10007

#define GEN_HASHTABLE(valtype, name)                                      \
    typedef struct name##_hashtable_node {                                \
        obj *k;                                                           \
        valtype v;                                                        \
        struct name##_hashtable_node *next;                               \
    } name##_hashtable_node;                                              \
                                                                          \
    typedef struct name##_hashtable_t {                                   \
        name##_hashtable_node *heads[HASH_SIZE];                          \
    } name##_hashtable_t;                                                 \
                                                                          \
    void name##_init(name##_hashtable_t *t) {                             \
        for (int i = 0; i < HASH_SIZE; i++) t->heads[i] = NULL;           \
    }                                                                     \
                                                                          \
    void name##_insert(name##_hashtable_t *t, obj *k, valtype v) {        \
        int h = ((intptr_t)k) % HASH_SIZE;                                \
        for (name##_hashtable_node *u = t->heads[h]; u; u = u->next)      \
            if (u->k == k) return;                                        \
        name##_hashtable_node *u = malloc(sizeof(name##_hashtable_node)); \
        u->next = t->heads[h];                                            \
        u->k = k;                                                         \
        u->v = v;                                                         \
        t->heads[h] = u;                                                  \
    }

GEN_HASHTABLE(char, remset)
GEN_HASHTABLE(obj *, forward)

// when allocating:
// if the young generation size limit is not reached after allocation,
// allocate from the young generation. otherwise, reclaim space from the
// young generation through a copying GC. if needed, grow the young
// generation. then tenure objects old enough, growing the old generation
// when needed. if the old generation is grown, trigger a mark-and-compact
// GC with the stack as the root.

// hand-emit write barriers for the remset

typedef struct gc_t {
    uint8_t *young_from, *young_to, *young_alloc, *young_scan, *old, *old_alloc;
    long young_size, old_size;

    ptr **stack;
    long stack_size, sp;

    remset_hashtable_t remset;
} gc_t;

int young_pointer_p(gc_t *gc, ptr p);
int check_young_refs(gc_t *gc, obj *p);
void copy_refs(gc_t *gc, obj *p);
void resolve_pointers(gc_t *gc, obj *p);
void fill_header(obj *o, enum heapvar_type_t type, long size);
ptr gc_alloc(gc_t *gc, enum heapvar_type_t type, long size);
// returns whether the old generation has ever been filled up during tenuring
// (whether a major collection is needed)
int gc_minor(gc_t *gc);
ptr gc_copy(gc_t *gc, ptr p);
void gc_mark(obj *p);
void gc_major(gc_t *gc);
void gc_grow(gc_t *gc);
void gc_preserve(gc_t *gc, ptr *p);
void gc_release(gc_t *gc, long count);

typedef struct ctx_t {
    gc_t memory;
    ptr env;
} ctx_t;

#endif
