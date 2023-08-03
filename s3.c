#include "s3.h"

#include <stdlib.h>
#include <string.h>

int lstrcmp(const char_t *s, const char_t *t) {
    long i;
    for (i = 0; s[i] == t[i]; i++)
        ;
    if (s[i] > t[i]) return 1;
    if (s[i] < t[i]) return -1;
    return 0;
}

long lstrlen(const char_t *s) {
    long i;
    for (i = 0; s[i]; i++)
        ;
    return i;
}

char_t utf8_getc(FILE *f) {
    char_t cp;
    int c = fgetc(f);
    if (c == EOF) return EOF;
    // 1 byte sequence
    if ((c >> 7 & 1) == 0) return c;
    // 2 byte sequence
    if ((c >> 5 & 7) == 0b110) {
        cp = c & 31;
        c = fgetc(f);
        if ((c >> 6 & 3) != 0b10) return c;
        cp = (cp << 6) | (c & 63);
        return cp;
    }
    if ((c >> 4 & 15) == 0b1110) {
        cp = c & 15;
        for (int i = 0; i < 2; i++) {
            c = fgetc(f);
            if ((c >> 6 & 3) != 0b10) return c;
            cp = (cp << 6) | (c & 63);
        }
        return cp;
    }
    if ((c >> 3 & 31) == 0b11110) {
        cp = c & 7;
        for (int i = 0; i < 3; i++) {
            c = fgetc(f);
            if ((c >> 6 & 3) != 0b10) return c;
            cp = (cp << 6) | (c & 63);
        }
        return cp;
    }
    return c;
}

void utf8_putc(FILE *f, char_t c) {
    if (c == EOF) return;
    if (c < 0x80) {
        fputc(c, f);
    } else if (c < 0x800) {
        fputc(0b11000000 | (c >> 6), f);
        fputc(0b10000000 | (c & 63), f);
    } else if (c < 0x10000) {
        fputc(0b11100000 | (c >> 12), f);
        fputc(0b10000000 | (c >> 6 & 63), f);
        fputc(0b10000000 | (c & 63), f);
    } else if (c < 0x110000) {
        fputc(0b11110000 | (c >> 18), f);
        fputc(0b10000000 | (c >> 12 & 63), f);
        fputc(0b10000000 | (c >> 6 & 63), f);
        fputc(0b10000000 | (c & 63), f);
    } else {
        fputc(c, f);
    }
}

ptr make_fixnum(int64_t x) {
    ptr p;
    p.type = T_FIXNUM;
    p.fixnum = x;
    return p;
}

ptr make_flonum(long double x) {
    ptr p;
    p.type = T_FLONUM;
    p.flonum = x;
    return p;
}

ptr make_char(char_t x) {
    ptr p;
    p.type = T_CHARACTER;
    p.character = x;
    return p;
}

ptr make_bool(int x) {
    ptr p;
    p.type = T_BOOLEAN;
    p.boolean = x;
    return p;
}

ptr make_primitive(int x) {
    ptr p;
    p.type = T_PRIMITIVE;
    p.primitive = x;
    return p;
}

ptr make_pointer(obj *p) {
    ptr pt;
    pt.type = T_PTR;
    pt.pointer = p;
    return pt;
}

ptr make_eof() {
    ptr p;
    p.type = T_EOF;
    return p;
}

ptr make_nil() {
    ptr p;
    p.type = T_NIL;
    return p;
}

ptr make_unbound() {
    ptr p;
    p.type = T_UNBOUND;
    return p;
}

void obarray_init(obarray_t *obarray) {
    for (int i = 0; i < OBARRAY_HASH_P; i++) obarray->heads[i] = NULL;
    obarray->count = 0;
}

ptr obarray_intern(obarray_t *obarray, const char_t *s) {
    long long hash = 0;
    long n = lstrlen(s);
    for (long i = 0; i < n; i++)
        for (int j = 0; j < 4; j++)
            hash = (hash * OBARRAY_HASH_E % OBARRAY_HASH_P +
                    (s[i] >> (j * 8) & 255) + 1) %
                   OBARRAY_HASH_P;
    for (obarray_node_t *u = obarray->heads[hash]; u; u = u->next) {
        if (lstrcmp(s, u->s) == 0) {
            ptr p;
            p.type = T_SYMBOL;
            p.symbol = u->index;
            return p;
        }
    }
    obarray->count++;
    ptr p;
    p.type = T_SYMBOL;
    p.symbol = obarray->count;
    obarray_node_t *v = malloc(sizeof(obarray_node_t));
    v->index = obarray->count;
    v->s = malloc((n + 1) * sizeof(char_t));
    memcpy(v->s, s, (n + 1) * sizeof(char_t));
    v->next = obarray->heads[hash];
    obarray->heads[hash] = v;
    return p;
}

ptr_move_transform_t make_transform(uint8_t *from, uint8_t *to, size_t size) {
    ptr_move_transform_t t;
    t.from = from;
    t.to = to;
    t.size = size;
    return t;
}

uint8_t *apply_transform(ptr_move_transform_t t, uint8_t *p) {
    ptrdiff_t offset = p - t.from;
    if (offset < 0 || offset >= t.size) return p;
    return p - t.from + t.to;
}

obj *apply_transform_obj(ptr_move_transform_t t, obj *p) {
    return (obj *)apply_transform(t, (uint8_t *)p);
}

void gc_init(gc_t *gc) {
    gc->stack = malloc(GC_INITIAL_SIZE * sizeof(ptr *));
    gc->stack_size = GC_INITIAL_SIZE;
    gc->sp = 0;

    gc->young_from = malloc(GC_INITIAL_SIZE);
    gc->young_size = GC_INITIAL_SIZE;
    gc->young_alloc = 0;

    gc->old = malloc(GC_INITIAL_SIZE * GC_OLD_TO_YOUNG_RATIO);
    gc->old_alloc = 0;
    gc->old_size = GC_INITIAL_SIZE * GC_OLD_TO_YOUNG_RATIO;

    remset_init(&gc->remset);
}

void fill_header(obj *o, enum heapvar_type_t type, long size) {
    o->type = type;
    o->size = size;
    o->age = 0;
    o->moved = 0;
}

int gc_minor(gc_t *gc) {
    gc->young_to = malloc(gc->young_size);
    gc->young_alloc = gc->young_to;
    for (long i = 0; i < gc->sp; i++)
        *gc->stack[i] = gc_copy(gc, *gc->stack[i]);
    for (int i = 0; i < HASH_SIZE; i++) {
        for (remset_hashtable_node **u = gc->remset.heads + i; *u;
             u = &(*u)->next) {
            int del = !check_young_refs(gc, (*u)->k);
            if (del) {
                remset_hashtable_node *v = *u;
                *u = (*u)->next;
                free(v);
            } else {
                copy_refs(gc, (*u)->k);
            }
        }
    }

    int flag = 0;
    gc->young_scan = gc->young_to;
    for (obj *p; gc->young_scan < gc->young_alloc; gc->young_scan += p->size) {
        p = (obj *)gc->young_scan;
        p->age++;
        copy_refs(gc, p);
    }

    free(gc->young_from);
    gc->young_from = gc->young_to;
    gc->young_to = NULL;

    // we scan objects to tenure after copying, to avoid growing while the to-
    // semispace is active
    gc->young_scan = gc->young_from;
    for (obj *p; gc->young_scan < gc->young_alloc; gc->young_scan += p->size) {
        p = (obj *)gc->young_scan;
        if (p->age >= GC_THRESHOLD_AGE) {
            while (gc->old_alloc + p->size > gc->old + gc->old_size) {
                flag = 1;
                gc_grow(gc);
                p = (obj *)gc->young_scan;
            }
            memcpy(gc->old_alloc, p, p->size);
            p->moved = 1;
            p->forward = (obj *)gc->old_alloc;
            gc->old_alloc += p->size;
        }
    }

    // resolve pointers to tenured objects
    for (long i = 0; i < gc->sp; i++) {
        ptr p = *gc->stack[i];
        if (p.type != T_PTR) continue;
        if (p.pointer->moved) p.pointer = p.pointer->forward;
    }
    for (int i = 0; i < HASH_SIZE; i++) {
        for (remset_hashtable_node *u = gc->remset.heads[i]; u; u = u->next) {
            resolve_pointers(gc, u->k);
        }
    }

    return flag;
}

ptr gc_copy(gc_t *gc, ptr p) {
    if (p.type != T_PTR) return p;
    ptrdiff_t offset = (uint8_t *)p.pointer - gc->young_from;
    if (offset < 0 || offset >= gc->young_size) return p;
    if (p.pointer->moved) {
        p.pointer = p.pointer->forward;
        return p;
    }
    obj *to = (obj *)gc->young_alloc;
    gc->young_alloc += p.pointer->size;
    memcpy(to, p.pointer, p.pointer->size);
    p.pointer = to;
    return p;
}

#define MAKE_WALKER(op, p)                                               \
    do {                                                                 \
        switch (p->type) {                                               \
            case H_BIGINT:                                               \
            case H_BYTEVECTOR:                                           \
            case H_STRING:                                               \
            case H_CODE:                                                 \
                break;                                                   \
            case H_RATIONAL:                                             \
                op(numerator);                                           \
                op(denominator);                                         \
                break;                                                   \
            case H_COMPLEX:                                              \
                op(real);                                                \
                op(imaginary);                                           \
                break;                                                   \
            case H_PAIR:                                                 \
                op(car);                                                 \
                op(cdr);                                                 \
                break;                                                   \
            case H_VECTOR:                                               \
                for (long i = 0; i < p->vector_size; i++) op(vector[i]); \
                break;                                                   \
            case H_ENVIRONMENT:                                          \
                for (int i = 0; i < BATCH_FATHER_SIZE; i++)              \
                    op(batch_env_father[i]);                             \
                for (long i = 0; i < p->env_size; i++) op(entry[i]);     \
                break;                                                   \
            case H_ACTIVATION_RECORD:                                    \
                for (int i = 0; i < BATCH_FATHER_SIZE; i++)              \
                    op(batch_ar_father[i]);                              \
                for (long i = 0; i < p->ar_size; i++) op(value[i]);      \
                break;                                                   \
            case H_PROCEDURE:                                            \
                op(formals);                                             \
                op(p_env);                                               \
                op(body);                                                \
                op(code);                                                \
                break;                                                   \
            case H_MACRO:                                                \
                for (long i = 0; i < p->macro_transformers_count; i++)   \
                    op(transformers[i]);                                 \
                break;                                                   \
            case H_TRANSFORMER:                                          \
                op(t_env);                                               \
                op(pattern);                                             \
                op(template);                                            \
                break;                                                   \
            case H_STRUCT:                                               \
                for (long i = 0; i < p->struct_size; i++) op(field[i]);  \
                break;                                                   \
            default:                                                     \
                FATAL("object walker: unknown object type");             \
        }                                                                \
    } while (0)

void gc_mark(obj *p) {
    if (p->mark) return;
    p->mark = 1;
#define MARK_OBJECT(member)                                      \
    do {                                                         \
        if (p->member.type == T_PTR) gc_mark(p->member.pointer); \
    } while (0)
    MAKE_WALKER(MARK_OBJECT, p);
#undef MARK_OBJECT
}

void gc_major(gc_t *gc) {
#define CLEAR_MARK(st, s, stmt)              \
    do {                                     \
        for (uint8_t *p = st; p < st + s;) { \
            obj *o = (obj *)p;               \
            stmt;                            \
            p += o->size;                    \
        }                                    \
    } while (0)
    CLEAR_MARK(gc->young_from, gc->young_size, o->mark = 0);
    CLEAR_MARK(gc->old, gc->old_size, o->mark = 0);
#undef CLEAR_MARK

    for (long i = 0; i < gc->sp; i++) {
        if (gc->stack[i]->type == T_PTR) {
            gc_mark(gc->stack[i]->pointer);
        }
    }

    // TODO: compact
    uint8_t *live, *free;
    // stage 1: compute forwarding pointers
    for (live = gc->young_from, free = gc->young_from;
         live < gc->young_from + gc->young_size;) {
        obj *o = (obj *)live;
        if (o->mark) {
            o->forward = (obj *)free;
            free += o->size;
        }
        live += o->size;
    }
    for (live = gc->old, free = gc->old; live < gc->old + gc->old_size;) {
        obj *o = (obj *)live;
        if (o->mark) {
            o->forward = (obj *)free;
            free += o->size;
        }
        live += o->size;
    }

#define UPDATE_MEMBER(member)                               \
    do {                                                    \
        if (o->member.type == T_PTR)                        \
            o->member.pointer = o->member.pointer->forward; \
    } while (0)

    for (live = gc->young_from; live < gc->young_from + gc->young_size;) {
        obj *o = (obj *)live;
        if (o->mark) {
            MAKE_WALKER(UPDATE_MEMBER, o);
        }
        live += o->size;
    }
    for (live = gc->old; live < gc->old + gc->old_size;) {
        obj *o = (obj *)live;
        if (o->mark) {
            MAKE_WALKER(UPDATE_MEMBER, o);
        }
        live += o->size;
    }
    for (long i = 0; i < gc->sp; i++) {
        if (gc->stack[i]->type == T_PTR)
            gc->stack[i]->pointer = gc->stack[i]->pointer->forward;
    }
#undef UPDATE_MEMBER
    for (live = gc->young_from; live < gc->young_from + gc->young_size;) {
        obj *o = (obj *)live;
        size_t size = o->size;
        if (o->mark) {
            memcpy(o->forward, o, size);  // TODO: bug?
        }
        live += size;
    }
    for (live = gc->old; live < gc->old + gc->old_size;) {
        obj *o = (obj *)live;
        size_t size = o->size;
        if (o->mark) {
            memcpy(o->forward, o, size);  // TODO: bug?
        }
        live += size;
    }
}

ptr gc_alloc(gc_t *gc, enum heapvar_type_t type, long size) {
    size = ((size - 1) / GC_ALIGNMENT + 1) * GC_ALIGNMENT;
    size += offsetof(obj, bigint_size);
    size = ((size - 1) / GC_ALIGNMENT + 1) * GC_ALIGNMENT;

    // try allocating in the young generation
    if (gc->young_alloc + size <= gc->young_from + gc->young_size) {
        gc->young_alloc += size;
        ptr p = make_pointer((obj *)(gc->young_alloc));
        fill_header(p.pointer, type, size);
        return p;
    }

    // trigger a minor collection
    int flag = gc_minor(gc);

    // if needed, trigger a major collection
    if (flag) gc_major(gc);

    while (gc->young_alloc + size <= gc->young_from + gc->young_size) {
        gc_grow(gc);
    }
    ptr p = make_pointer((obj *)(gc->young_alloc));
    gc->young_alloc += size;
    fill_header(p.pointer, type, size);
    return p;
}

// grows the heap by GC_GROW_RATIO, preserving pointers on the stack and GC
// variables
// caution: can be called only when to-semispace is not active, i.e. not during
// copying
void gc_grow(gc_t *gc) {
    if (gc->young_to) FATAL("Gc-grow when the to subspace is active");
    ptr_move_transform_t t_y_f = make_transform(
        gc->young_from, malloc(gc->young_size * GC_GROW_RATIO), gc->young_size);
    ptr_move_transform_t t_o = make_transform(
        gc->old, malloc(gc->old_size * GC_GROW_RATIO), gc->old_size);
    memcpy(t_y_f.to, t_y_f.from, t_y_f.size);
    memcpy(t_o.to, t_o.from, t_o.size);
#define COMPOSED_OBJ(p) apply_transform_obj(t_o, apply_transform_obj(t_y_f, p))
#define COMPOSED(p) apply_transform(t_o, apply_transform(t_y_f, p))
#define TRANSFORM_OBJ(o) o = COMPOSED_OBJ(o)
#define TRANSFORM(o) o = COMPOSED(o)
    TRANSFORM(gc->young_from);
    TRANSFORM(gc->young_to);
    TRANSFORM(gc->young_alloc);
    TRANSFORM(gc->young_scan);
    TRANSFORM(gc->old);
    TRANSFORM(gc->old_alloc);
    for (long i = 0; i < gc->sp; i++) {
        if (gc->stack[i]->type == T_PTR) {
            TRANSFORM_OBJ(gc->stack[i]->pointer);
        }
    }

#define TRANSFORM_MEMBER(member)                                       \
    do {                                                               \
        if (o->member.type == T_PTR) TRANSFORM_OBJ(o->member.pointer); \
    } while (0)
#define TRANSFORM_HEAP(start, s)                   \
    do {                                           \
        for (uint8_t *p = start; p < start + s;) { \
            obj *o = (obj *)p;                     \
            MAKE_WALKER(TRANSFORM_MEMBER, o);      \
            p += o->size;                          \
        }                                          \
    } while (0)
    TRANSFORM_HEAP(gc->young_from, gc->young_size);
    TRANSFORM_HEAP(gc->old, gc->old_size);
#undef COMPOSED_OBJ
#undef TRANSFORM_OBJ
#undef COMPOSED
#undef TRANSFORM
}

void gc_preserve(gc_t *gc, ptr *p) {
    if (gc->sp >= gc->stack_size) {
        gc->stack_size *= 2;
        gc->stack = realloc(gc->stack, gc->stack_size * sizeof(ptr *));
    }
    gc->stack[gc->sp++] = p;
}

void gc_release(gc_t *gc, long count) { gc->sp -= count; }

#define CHECK_MEMBER(member)                          \
    do {                                              \
        if (young_pointer_p(gc, p->member)) return 1; \
    } while (0)

#define COPY_MEMBER(member)                 \
    do {                                    \
        p->member = gc_copy(gc, p->member); \
    } while (0)

#define RESOLVE_MEMBER(member)                   \
    do {                                         \
        ptr o = p->member;                       \
        if (o.type == T_PTR && o.pointer->moved) \
            o.pointer = o.pointer->forward;      \
    } while (0)

int check_young_refs(gc_t *gc, obj *p) { MAKE_WALKER(CHECK_MEMBER, p); }
void copy_refs(gc_t *gc, obj *p) { MAKE_WALKER(COPY_MEMBER, p); }
void resolve_pointers(gc_t *gc, obj *p) { MAKE_WALKER(RESOLVE_MEMBER, p); }

int young_pointer_p(gc_t *gc, ptr p) {
    if (p.type != T_PTR) return 0;
    ptrdiff_t offset = (uint8_t *)p.pointer - gc->young_from;
    if (offset < 0 || offset >= gc->young_size) return 0;
    return 1;
}
