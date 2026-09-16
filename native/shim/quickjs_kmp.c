#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#include "quickjs.h"
#include "quickjs_kmp.h"

#ifndef KMPJS_UPSTREAM_COMMIT
#error "KMPJS_UPSTREAM_COMMIT must be defined (CMake reads it from native/UPSTREAM)"
#endif

enum { KMP_IDLE = 0, KMP_RUNNING = 1, KMP_INTERRUPTED = 2 };

typedef struct {
    char *data;
    int32_t len;
    int32_t cap;
} kmp_buf;

/* One slot per live ref, holding a dup'ed JSValue. A ref handle packs the slot index
   (low 32 bits) with a 32-bit generation (high bits) so a stale handle whose slot was
   reused is rejected instead of touching another object. */
typedef struct {
    JSValue val;
    int32_t refcount;
    int32_t next_free;
    uint32_t gen;
} kmp_slot;

typedef struct {
    JSValue promise;
    JSValue reason;
} kmp_rejected;

/* One row per module name the engine knows: registered (source kept until the first import) or
   evaluated directly (source NULL). Both live in the context's module cache once loaded, so the
   two kinds share one namespace and a name can only be claimed once. */
enum { KMP_MODULE_SOURCE = 0, KMP_MODULE_BYTECODE = 1, KMP_MODULE_EVALUATED = 2 };

typedef struct {
    char *name;
    char *source; /* NUL terminated; only KMP_MODULE_SOURCE keeps it, the engine holds the other kinds */
    size_t len;
    int kind;
} kmp_module;

struct kmpjs_engine {
    JSRuntime *rt;
    JSContext *ctx;
    void *user;
    kmpjs_host_fn host;
    kmpjs_log_fn log;
    kmpjs_rejection_fn rejection;
    size_t max_stack_size;
    int64_t memory_limit; /* as configured; the engine stores "unlimited" as (size_t)-1, which differs per word size */
    JSClassID cls_array_buffer;
    JSClassID cls_shared_array_buffer;
    JSClassID cls_typed_array_first; /* Uint8ClampedArray .. Float64Array are contiguous */
    JSClassID cls_typed_array_last;
    JSValue bigint_ctor;
    char *module_scheme;
    kmp_module *modules;
    int32_t module_count;
    int32_t module_cap;
    kmp_rejected *rejected; /* rejected without a handler so far; reported after the outermost drain */
    int32_t rejected_count;
    int32_t rejected_cap;
    kmp_buf rej_str;
    kmp_buf rej_stack;
    atomic_int state; /* KMP_IDLE / KMP_RUNNING / KMP_INTERRUPTED */
    int oom;          /* an allocation was refused by the memory limit since the last reset */
    kmp_buf out_str;
    kmp_buf out_stack;
    kmp_buf log_line;
    kmp_slot *slots;
    int32_t slot_count;
    int32_t slot_cap;
    int32_t free_head; /* index of first free slot or -1 */
};

static void buf_reset(kmp_buf *b)
{
    b->len = 0;
}

static int buf_append(kmp_buf *b, const void *p, size_t n)
{
    if ((size_t)b->cap - b->len < n) {
        size_t cap = b->cap ? b->cap : 64;
        char *data;
        while (cap - b->len < n)
            cap *= 2;
        data = realloc(b->data, cap);
        if (!data)
            return -1;
        b->data = data;
        b->cap = (int32_t)cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += (int32_t)n;
    return 0;
}

static void buf_free(kmp_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void publish(kmp_buf *b, const char **pstr, int32_t *plen)
{
    *pstr = b->data ? b->data : "";
    *plen = b->len;
}

char *kmpjs_alloc(int32_t len)
{
    return malloc(len > 0 ? (size_t)len : 1);
}

void kmpjs_free(void *p)
{
    free(p);
}

int32_t kmpjs_abi_version(void)
{
    return KMPJS_ABI_VERSION;
}

static int32_t fail_message(kmpjs_engine *e, kmpjs_value *out, const char *msg);
static void exception_to_out(kmpjs_engine *e, kmpjs_value *out);
static char *terminated_copy(const char *p, int32_t len);

/* ---- allocator: the engine's default one plus a flag for refused allocations ----
   At the limit QuickJS cannot even build the "out of memory" Error and throws null instead;
   the flag lets exception_to_out tell that null apart from a script's own `throw null`. */

#if defined(__APPLE__)
#define KMP_MALLOC_OVERHEAD 0
#else
#define KMP_MALLOC_OVERHEAD 8
#endif

static size_t kmp_malloc_usable_size(const void *ptr)
{
#if defined(__APPLE__)
    return malloc_size(ptr);
#else
    return malloc_usable_size((void *)ptr);
#endif
}

static void *kmp_malloc(JSMallocState *s, size_t size)
{
    void *ptr;
    if (s->malloc_size + size > s->malloc_limit) {
        ((kmpjs_engine *)s->opaque)->oom = 1;
        return NULL;
    }
    ptr = malloc(size);
    if (!ptr)
        return NULL;
    s->malloc_count++;
    s->malloc_size += kmp_malloc_usable_size(ptr) + KMP_MALLOC_OVERHEAD;
    return ptr;
}

static void kmp_free(JSMallocState *s, void *ptr)
{
    if (!ptr)
        return;
    s->malloc_count--;
    s->malloc_size -= kmp_malloc_usable_size(ptr) + KMP_MALLOC_OVERHEAD;
    free(ptr);
}

static void *kmp_realloc(JSMallocState *s, void *ptr, size_t size)
{
    size_t old_size;
    if (!ptr) {
        if (size == 0)
            return NULL;
        return kmp_malloc(s, size);
    }
    old_size = kmp_malloc_usable_size(ptr);
    if (size == 0) {
        s->malloc_count--;
        s->malloc_size -= old_size + KMP_MALLOC_OVERHEAD;
        free(ptr);
        return NULL;
    }
    if (s->malloc_size + size - old_size > s->malloc_limit) {
        ((kmpjs_engine *)s->opaque)->oom = 1;
        return NULL;
    }
    ptr = realloc(ptr, size);
    if (!ptr)
        return NULL;
    s->malloc_size += kmp_malloc_usable_size(ptr) - old_size;
    return ptr;
}

static const JSMallocFunctions kmp_malloc_funcs = { kmp_malloc, kmp_free, kmp_realloc, kmp_malloc_usable_size };

/* ---- globals the engine core does not provide: console.log / print / performance.now ---- */

static void print_write(void *opaque, const char *buf, size_t len)
{
    buf_append((kmp_buf *)opaque, buf, len);
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    kmpjs_engine *e = JS_GetContextOpaque(ctx);
    int i;

    buf_reset(&e->log_line);
    for (i = 0; i < argc; i++) {
        if (i != 0)
            buf_append(&e->log_line, " ", 1);
        if (JS_IsString(argv[i])) {
            size_t len;
            const char *p = JS_ToCStringLen(ctx, &len, argv[i]);
            if (!p)
                return JS_EXCEPTION;
            buf_append(&e->log_line, p, len);
            JS_FreeCString(ctx, p);
        } else {
            JSPrintValueOptions opts;
            JS_PrintValueSetDefaultOptions(&opts);
            JS_PrintValue(ctx, print_write, &e->log_line, argv[i], &opts);
        }
    }
    if (e->log)
        e->log(e->user, e->log_line.data ? e->log_line.data : "", e->log_line.len);
    return JS_UNDEFINED;
}

static JSValue js_performance_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return JS_NewFloat64(ctx, (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6);
}

static int install_globals(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JSValue performance = JS_NewObject(ctx);
    int rc = -1;

    if (JS_IsException(console) || JS_IsException(performance))
        goto done;
    if (JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_print, "log", 1)) < 0)
        goto done;
    if (JS_SetPropertyStr(ctx, performance, "now", JS_NewCFunction(ctx, js_performance_now, "now", 0)) < 0)
        goto done;
    if (JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1)) < 0)
        goto done;
    if (JS_SetPropertyStr(ctx, global, "console", console) < 0) {
        console = JS_UNDEFINED;
        goto done;
    }
    console = JS_UNDEFINED;
    if (JS_SetPropertyStr(ctx, global, "performance", performance) < 0) {
        performance = JS_UNDEFINED;
        goto done;
    }
    performance = JS_UNDEFINED;
    rc = 0;
done:
    JS_FreeValue(ctx, console);
    JS_FreeValue(ctx, performance);
    JS_FreeValue(ctx, global);
    return rc;
}

/* The class ids of the binary types are engine internals; instances tell them at startup so the
   conversion can recognise them without throwing a TypeError per non-binary object. */
static int probe_classes(kmpjs_engine *e)
{
    static const char src[] = "[new ArrayBuffer(0), new SharedArrayBuffer(0), new Uint8ClampedArray(0), new Float64Array(0), BigInt]";
    JSContext *ctx = e->ctx;
    JSValue arr = JS_Eval(ctx, src, sizeof(src) - 1, "<init>", JS_EVAL_TYPE_GLOBAL);
    JSValue item;
    int rc = -1;

    if (JS_IsException(arr))
        return -1;
    item = JS_GetPropertyUint32(ctx, arr, 0);
    e->cls_array_buffer = JS_GetClassID(item);
    JS_FreeValue(ctx, item);
    item = JS_GetPropertyUint32(ctx, arr, 1);
    e->cls_shared_array_buffer = JS_GetClassID(item);
    JS_FreeValue(ctx, item);
    item = JS_GetPropertyUint32(ctx, arr, 2);
    e->cls_typed_array_first = JS_GetClassID(item);
    JS_FreeValue(ctx, item);
    item = JS_GetPropertyUint32(ctx, arr, 3);
    e->cls_typed_array_last = JS_GetClassID(item);
    JS_FreeValue(ctx, item);
    e->bigint_ctor = JS_GetPropertyUint32(ctx, arr, 4);
    if (e->cls_array_buffer && e->cls_shared_array_buffer && e->cls_typed_array_first &&
        e->cls_typed_array_last > e->cls_typed_array_first && JS_IsFunction(ctx, e->bigint_ctor))
        rc = 0;
    JS_FreeValue(ctx, arr);
    return rc;
}

/* ---- modules ---- */

static kmp_module *find_module(kmpjs_engine *e, const char *name)
{
    int32_t i;
    for (i = 0; i < e->module_count; i++) {
        if (strcmp(e->modules[i].name, name) == 0)
            return &e->modules[i];
    }
    return NULL;
}

static int set_import_meta(kmpjs_engine *e, JSModuleDef *m, const char *name)
{
    JSContext *ctx = e->ctx;
    JSValue meta = JS_GetImportMeta(ctx, m);
    size_t scheme_len = strlen(e->module_scheme), name_len = strlen(name);
    char *url;
    int rc;

    if (JS_IsException(meta))
        return -1;
    url = malloc(scheme_len + 1 + name_len + 1);
    if (!url) {
        JS_FreeValue(ctx, meta);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memcpy(url, e->module_scheme, scheme_len);
    url[scheme_len] = ':';
    memcpy(url + scheme_len + 1, name, name_len + 1);
    rc = JS_DefinePropertyValueStr(ctx, meta, "url", JS_NewString(ctx, url), JS_PROP_C_W_E);
    free(url);
    JS_FreeValue(ctx, meta);
    return rc < 0 ? -1 : 0;
}

static const char *module_name_of(JSContext *ctx, JSModuleDef *m)
{
    JSAtom atom = JS_GetModuleName(ctx, m);
    const char *name = JS_AtomToCString(ctx, atom);
    JS_FreeAtom(ctx, atom);
    return name; /* release with JS_FreeCString */
}

/* Compiles module source (NUL terminated) into a JS_TAG_MODULE value with import.meta set. */
static JSValue compile_module(kmpjs_engine *e, const char *code, size_t len, const char *name)
{
    JSContext *ctx = e->ctx;
    JSValue obj = JS_Eval(ctx, code, len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(obj))
        return obj;
    if (set_import_meta(e, JS_VALUE_GET_PTR(obj), name)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static char *kmp_module_normalize(JSContext *ctx, const char *base, const char *name, void *opaque)
{
    return js_strdup(ctx, name);
}

static JSModuleDef *kmp_module_loader(JSContext *ctx, const char *name, void *opaque)
{
    kmpjs_engine *e = opaque;
    kmp_module *mod = find_module(e, name);
    JSValue obj;
    JSModuleDef *m;

    if (!mod || !mod->source) {
        JS_ThrowReferenceError(ctx, "module '%s' is not registered", name);
        return NULL;
    }
    obj = compile_module(e, mod->source, mod->len, name);
    if (JS_IsException(obj))
        return NULL;
    /* the context keeps the module in its loaded list; the value itself is not needed */
    m = JS_VALUE_GET_PTR(obj);
    JS_FreeValue(ctx, obj);
    return m;
}

/* ---- bytecode ---- */

/* File header in front of JS_WriteObject's output; all fields little endian. */
typedef struct {
    char magic[4];       /* "QJKB" */
    uint32_t header_len; /* KMPJS_BYTECODE_HEADER_SIZE */
    uint32_t kind;       /* 0 script, 1 module */
    char upstream[40];   /* engine commit the bytecode was produced with */
} kmp_bc_header;

static int32_t owned_message(kmpjs_value *out, const char *msg)
{
    size_t len = strlen(msg);
    memset(out, 0, sizeof(*out));
    out->tag = KMPJS_TAG_EXCEPTION;
    out->str = kmpjs_alloc((int32_t)len);
    if (out->str) {
        memcpy((char *)out->str, msg, len);
        out->str_len = (int32_t)len;
    }
    return -1;
}

static int32_t owned_exception(JSContext *ctx, kmpjs_value *out)
{
    JSValue exc = JS_GetException(ctx);
    JSValue s = JS_ToString(ctx, exc);
    size_t len;
    const char *p = JS_IsException(s) ? NULL : JS_ToCStringLen(ctx, &len, s);

    if (!p) {
        owned_message(out, "compilation failed");
    } else {
        memset(out, 0, sizeof(*out));
        out->tag = KMPJS_TAG_EXCEPTION;
        out->str = kmpjs_alloc((int32_t)len);
        if (out->str) {
            memcpy((char *)out->str, p, len);
            out->str_len = (int32_t)len;
        }
        JS_FreeCString(ctx, p);
    }
    JS_FreeValue(ctx, s);
    if (JS_IsError(ctx, exc)) {
        JSValue st = JS_GetPropertyStr(ctx, exc, "stack");
        if (JS_IsString(st) && (p = JS_ToCStringLen(ctx, &len, st)) != NULL) {
            out->stack = kmpjs_alloc((int32_t)len);
            if (out->stack) {
                memcpy((char *)out->stack, p, len);
                out->stack_len = (int32_t)len;
            }
            JS_FreeCString(ctx, p);
        }
        JS_FreeValue(ctx, st);
    }
    JS_FreeValue(ctx, exc);
    return -1;
}

/* The compiler resolves a module's imports even in COMPILE_ONLY mode, but the bytecode only
   records their names, so any name resolves to an empty placeholder here. */
static int stub_module_init(JSContext *ctx, JSModuleDef *m)
{
    return 0;
}

static JSModuleDef *stub_module_loader(JSContext *ctx, const char *name, void *opaque)
{
    return JS_NewCModule(ctx, name, stub_module_init);
}

int32_t kmpjs_compile(const char *code, int32_t code_len, const char *filename, int32_t flags, kmpjs_value *out)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx;
    char *buf;
    JSValue obj;
    uint8_t *data;
    size_t data_len;
    kmp_bc_header hdr;
    char *result;
    int strip = 0, eval_flags = JS_EVAL_FLAG_COMPILE_ONLY;
    int32_t rc;

    if (!rt)
        return owned_message(out, "could not create compile runtime");
    if (flags & KMPJS_COMPILE_STRIP_SOURCE)
        strip |= JS_STRIP_SOURCE;
    if (flags & KMPJS_COMPILE_STRIP_DEBUG)
        strip |= JS_STRIP_DEBUG;
    JS_SetStripInfo(rt, strip);
    JS_SetModuleLoaderFunc(rt, kmp_module_normalize, stub_module_loader, NULL);
    ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return owned_message(out, "could not create compile context");
    }
    eval_flags |= (flags & KMPJS_COMPILE_MODULE) ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    buf = terminated_copy(code, code_len);
    if (!buf) {
        rc = owned_message(out, "out of memory");
        goto done;
    }
    obj = JS_Eval(ctx, buf, (size_t)code_len, filename, eval_flags);
    free(buf);
    if (JS_IsException(obj)) {
        rc = owned_exception(ctx, out);
        goto done;
    }
    data = JS_WriteObject(ctx, &data_len, obj, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, obj);
    if (!data) {
        rc = owned_exception(ctx, out);
        goto done;
    }
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, "QJKB", 4);
    hdr.header_len = KMPJS_BYTECODE_HEADER_SIZE;
    hdr.kind = (flags & KMPJS_COMPILE_MODULE) ? 1 : 0;
    memcpy(hdr.upstream, KMPJS_UPSTREAM_COMMIT, sizeof(hdr.upstream));
    result = kmpjs_alloc((int32_t)(sizeof(hdr) + data_len));
    if (!result) {
        js_free(ctx, data);
        rc = owned_message(out, "out of memory");
        goto done;
    }
    memcpy(result, &hdr, sizeof(hdr));
    memcpy(result + sizeof(hdr), data, data_len);
    js_free(ctx, data);
    memset(out, 0, sizeof(*out));
    out->tag = KMPJS_TAG_STRING;
    out->str = result;
    out->str_len = (int32_t)(sizeof(hdr) + data_len);
    rc = 0;
done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}

/* Checks the header; *kind receives 0 for a script, 1 for a module, *body the serialized object. */
static int32_t check_bytecode_header(kmpjs_engine *e, const uint8_t *buf, int32_t len, int *kind,
                                     const uint8_t **body, size_t *body_len, kmpjs_value *out)
{
    kmp_bc_header hdr;

    if (len < (int32_t)sizeof(hdr) || memcmp(buf, "QJKB", 4) != 0)
        return fail_message(e, out, "not QuickJS bytecode produced by this SDK");
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.header_len != KMPJS_BYTECODE_HEADER_SIZE || hdr.kind > 1)
        return fail_message(e, out, "unsupported bytecode header");
    if (memcmp(hdr.upstream, KMPJS_UPSTREAM_COMMIT, sizeof(hdr.upstream)) != 0) {
        char msg[160];
        snprintf(msg, sizeof msg, "bytecode built for engine %.12s, this SDK embeds engine %.12s", hdr.upstream, KMPJS_UPSTREAM_COMMIT);
        return fail_message(e, out, msg);
    }
    *kind = (int)hdr.kind;
    *body = buf + sizeof(hdr);
    *body_len = (size_t)len - sizeof(hdr);
    return 0;
}

/* Reads the object into the engine's context. */
static JSValue read_bytecode_body(kmpjs_engine *e, const uint8_t *body, size_t body_len, int kind, kmpjs_value *out)
{
    JSValue obj = JS_ReadObject(e->ctx, body, body_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) {
        exception_to_out(e, out);
        return JS_EXCEPTION;
    }
    if ((kind == 1) != (JS_VALUE_GET_TAG(obj) == JS_TAG_MODULE)) {
        JS_FreeValue(e->ctx, obj);
        fail_message(e, out, "bytecode body does not match its header");
        return JS_EXCEPTION;
    }
    return obj;
}

/* Reading a module into the engine's context also caches it there for good, so a name check has
   to happen before that: the name is read out of a throwaway bare context instead. Returns a
   malloc'ed copy, or NULL with *out set. */
static char *peek_module_name(kmpjs_engine *e, const uint8_t *body, size_t body_len, kmpjs_value *out)
{
    JSContext *scratch = JS_NewContextRaw(e->rt);
    JSValue obj;
    const char *name;
    char *copy = NULL;

    if (!scratch) {
        fail_message(e, out, "out of memory");
        return NULL;
    }
    obj = JS_ReadObject(scratch, body, body_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) {
        JS_FreeValue(scratch, JS_GetException(scratch));
        fail_message(e, out, "bytecode body is corrupt");
    } else if (JS_VALUE_GET_TAG(obj) != JS_TAG_MODULE) {
        JS_FreeValue(scratch, obj);
        fail_message(e, out, "bytecode body does not match its header");
    } else {
        name = module_name_of(scratch, JS_VALUE_GET_PTR(obj));
        copy = name ? strdup(name) : NULL;
        JS_FreeCString(scratch, name);
        JS_FreeValue(scratch, obj);
        if (!copy)
            fail_message(e, out, "out of memory");
    }
    JS_FreeContext(scratch);
    return copy;
}

/* ---- interrupt state ---- */

static int kmp_interrupt_handler(JSRuntime *rt, void *opaque)
{
    kmpjs_engine *e = opaque;
    return atomic_load_explicit(&e->state, memory_order_relaxed) == KMP_INTERRUPTED;
}

void kmpjs_interrupt(kmpjs_engine *e)
{
    int expected = KMP_RUNNING;
    atomic_compare_exchange_strong(&e->state, &expected, KMP_INTERRUPTED);
}

/* ---- promise jobs ---- */

static int32_t find_rejected(kmpjs_engine *e, JSValueConst promise)
{
    int32_t i;
    for (i = 0; i < e->rejected_count; i++) {
        if (JS_VALUE_GET_PTR(e->rejected[i].promise) == JS_VALUE_GET_PTR(promise))
            return i;
    }
    return -1;
}

static void forget_rejected(kmpjs_engine *e, int32_t i)
{
    JS_FreeValue(e->ctx, e->rejected[i].promise);
    JS_FreeValue(e->ctx, e->rejected[i].reason);
    e->rejected[i] = e->rejected[--e->rejected_count];
}

/* The engine reports a rejection as soon as it happens and again when a handler is attached later
   in the same tick, so entries are only judged after the drain. */
static void kmp_rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                                  JS_BOOL is_handled, void *opaque)
{
    kmpjs_engine *e = opaque;
    int32_t i = find_rejected(e, promise);
    if (is_handled) {
        if (i >= 0)
            forget_rejected(e, i);
        return;
    }
    if (i >= 0)
        return;
    if (e->rejected_count == e->rejected_cap) {
        int32_t cap = e->rejected_cap ? e->rejected_cap * 2 : 4;
        kmp_rejected *list = realloc(e->rejected, (size_t)cap * sizeof(*list));
        if (!list)
            return;
        e->rejected = list;
        e->rejected_cap = cap;
    }
    e->rejected[e->rejected_count].promise = JS_DupValue(ctx, promise);
    e->rejected[e->rejected_count].reason = JS_DupValue(ctx, reason);
    e->rejected_count++;
}

/* Runs queued microtasks until none is left. Returns -1 with the exception pending when a job
   fails (only uncatchable errors such as an interrupt get here: a throwing reaction rejects its
   derived promise instead) or when an interrupt is already flagged. The jobs left behind are
   discarded: the engine has no API to drop them, so they are run with a 1-byte stack limit and
   fail at function entry before they can enqueue anything (a self-scheduling chain would
   otherwise outlive the interrupt and hang the next call). */
static void discard_jobs(kmpjs_engine *e)
{
    JSContext *ctx1;
    JS_SetMaxStackSize(e->rt, 1);
    while (JS_ExecutePendingJob(e->rt, &ctx1) != 0)
        JS_FreeValue(e->ctx, JS_GetException(e->ctx));
    JS_SetMaxStackSize(e->rt, e->max_stack_size);
    while (e->rejected_count > 0)
        forget_rejected(e, e->rejected_count - 1);
}

static int drain_jobs(kmpjs_engine *e)
{
    JSContext *ctx1;
    int rc;
    for (;;) {
        if (atomic_load_explicit(&e->state, memory_order_relaxed) == KMP_INTERRUPTED) {
            JS_ThrowInternalError(e->ctx, "interrupted");
            rc = -1;
            break;
        }
        rc = JS_ExecutePendingJob(e->rt, &ctx1);
        if (rc <= 0)
            break;
    }
    if (rc < 0) {
        JSValue exc = JS_GetException(e->ctx);
        discard_jobs(e);
        JS_Throw(e->ctx, exc);
    }
    return rc;
}

/* Nested runs (a host function calling back into the engine) keep the outer state so an
   interrupt is never lost; only the outermost run returns the engine to idle. The stack
   limit is measured from the entering thread's stack, so it is refreshed on every outermost
   entry: the runtime only records it once at creation (JS_UpdateStackTop). */
static int run_begin(kmpjs_engine *e)
{
    int expected = KMP_IDLE;
    int outermost = atomic_compare_exchange_strong(&e->state, &expected, KMP_RUNNING);
    if (outermost) {
        JS_UpdateStackTop(e->rt);
        e->oom = 0;
    }
    return outermost;
}

static void run_end(kmpjs_engine *e, int outermost)
{
    if (outermost)
        atomic_store(&e->state, KMP_IDLE);
}

/* ---- ref table ---- */

static int64_t slot_handle(int32_t idx, uint32_t gen)
{
    return (int64_t)(((uint64_t)gen << 32) | (uint32_t)(idx + 1));
}

static int32_t slot_index(int64_t ref)
{
    return (int32_t)((uint64_t)ref & 0xFFFFFFFFu) - 1;
}

static kmp_slot *slot_get(kmpjs_engine *e, int64_t ref)
{
    kmp_slot *s;
    int32_t idx = slot_index(ref);
    uint32_t gen = (uint32_t)((uint64_t)ref >> 32);
    if (ref <= 0 || idx < 0 || idx >= e->slot_count)
        return NULL;
    s = &e->slots[idx];
    return (s->refcount > 0 && s->gen == gen) ? s : NULL;
}

/* Takes ownership of v; frees it and returns 0 when no slot can be allocated. */
static int64_t slot_new(kmpjs_engine *e, JSValue v)
{
    kmp_slot *s;
    int32_t idx;

    if (e->free_head >= 0) {
        idx = e->free_head;
        s = &e->slots[idx];
        e->free_head = s->next_free;
    } else {
        if (e->slot_count == INT32_MAX) {
            JS_FreeValue(e->ctx, v);
            return 0;
        }
        if (e->slot_count == e->slot_cap) {
            int32_t cap = e->slot_cap ? e->slot_cap * 2 : 16;
            kmp_slot *slots = realloc(e->slots, (size_t)cap * sizeof(*slots));
            if (!slots) {
                JS_FreeValue(e->ctx, v);
                return 0;
            }
            e->slots = slots;
            e->slot_cap = cap;
        }
        idx = e->slot_count++;
        s = &e->slots[idx];
        memset(s, 0, sizeof(*s));
    }
    s->val = v;
    s->refcount = 1;
    s->next_free = -1;
    return slot_handle(idx, s->gen);
}

static void slot_free(kmpjs_engine *e, int64_t ref)
{
    int32_t idx = slot_index(ref);
    kmp_slot *s = &e->slots[idx];
    JS_FreeValue(e->ctx, s->val);
    s->val = JS_UNDEFINED;
    s->refcount = 0;
    s->gen++;
    s->next_free = e->free_head;
    e->free_head = idx;
}

void kmpjs_ref_retain(kmpjs_engine *e, int64_t ref)
{
    kmp_slot *s = slot_get(e, ref);
    if (s)
        s->refcount++;
}

void kmpjs_ref_release(kmpjs_engine *e, int64_t ref)
{
    kmp_slot *s = slot_get(e, ref);
    if (s && --s->refcount == 0)
        slot_free(e, ref);
}

void kmpjs_get_stats(kmpjs_engine *e, kmpjs_stats *stats)
{
    JSMemoryUsage usage;
    int32_t i, live = 0;
    /* refcount, not slots: a retained ref counts once per outstanding release */
    for (i = 0; i < e->slot_count; i++)
        live += e->slots[i].refcount;
    stats->live_refs = live;
    stats->ref_slots = e->slot_count;
    JS_ComputeMemoryUsage(e->rt, &usage);
    stats->memory_used = usage.malloc_size;
    stats->memory_limit = e->memory_limit;
    stats->object_count = usage.obj_count;
    stats->string_count = usage.str_count;
    stats->atom_count = usage.atom_count;
    stats->function_count = usage.js_func_count;
}

void kmpjs_dump_memory(kmpjs_engine *e, kmpjs_value *out)
{
    JSMemoryUsage usage;
    char *data = NULL;
    size_t size = 0;
    FILE *fp;

    JS_ComputeMemoryUsage(e->rt, &usage);
    buf_reset(&e->out_str);
    fp = open_memstream(&data, &size);
    if (fp) {
        JS_DumpMemoryUsage(fp, &usage, e->rt);
        fclose(fp);
        buf_append(&e->out_str, data, size);
        free(data);
    }
    memset(out, 0, sizeof(*out));
    out->tag = KMPJS_TAG_STRING;
    publish(&e->out_str, &out->str, &out->str_len);
}

/* ---- engine lifecycle ---- */

kmpjs_engine *kmpjs_create(const kmpjs_config *config, void *user, kmpjs_host_fn host, kmpjs_log_fn log,
                           kmpjs_rejection_fn rejection)
{
    kmpjs_engine *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    atomic_init(&e->state, KMP_IDLE);
    e->free_head = -1;
    e->user = user;
    e->host = host;
    e->log = log;
    e->rejection = rejection;
    e->max_stack_size = (size_t)config->max_stack_size;
    e->memory_limit = config->memory_limit > 0 ? config->memory_limit : 0;
    e->module_scheme = strdup(config->module_scheme && config->module_scheme[0] ? config->module_scheme : "kmp");
    e->rt = e->module_scheme ? JS_NewRuntime2(&kmp_malloc_funcs, e) : NULL;
    if (!e->rt) {
        free(e->module_scheme);
        free(e);
        return NULL;
    }
    JS_SetModuleLoaderFunc(e->rt, kmp_module_normalize, kmp_module_loader, e);
    if (config->memory_limit > 0)
        JS_SetMemoryLimit(e->rt, (size_t)config->memory_limit);
    JS_SetMaxStackSize(e->rt, (size_t)config->max_stack_size);
    if (config->gc_threshold > 0)
        JS_SetGCThreshold(e->rt, (size_t)config->gc_threshold);
    JS_SetRuntimeOpaque(e->rt, e);
    JS_SetInterruptHandler(e->rt, kmp_interrupt_handler, e);
    JS_SetHostPromiseRejectionTracker(e->rt, kmp_rejection_tracker, e);
    e->ctx = JS_NewContext(e->rt);
    if (!e->ctx) {
        JS_FreeRuntime(e->rt);
        free(e->module_scheme);
        free(e);
        return NULL;
    }
    JS_SetContextOpaque(e->ctx, e);
    e->bigint_ctor = JS_UNDEFINED;
    if (install_globals(e->ctx) || probe_classes(e)) {
        JS_FreeValue(e->ctx, e->bigint_ctor);
        JS_FreeContext(e->ctx);
        JS_FreeRuntime(e->rt);
        free(e->module_scheme);
        free(e);
        return NULL;
    }
    return e;
}

/* JS_FreeRuntime asserts that nothing is left alive, so every JSValue the shim holds goes first. */
void kmpjs_destroy(kmpjs_engine *e)
{
    int32_t i;
    if (!e)
        return;
    for (i = 0; i < e->slot_count; i++) {
        if (e->slots[i].refcount > 0)
            JS_FreeValue(e->ctx, e->slots[i].val);
    }
    free(e->slots);
    JS_FreeValue(e->ctx, e->bigint_ctor);
    while (e->rejected_count > 0)
        forget_rejected(e, e->rejected_count - 1);
    free(e->rejected);
    JS_FreeContext(e->ctx);
    JS_FreeRuntime(e->rt);
    for (i = 0; i < e->module_count; i++) {
        free(e->modules[i].name);
        free(e->modules[i].source);
    }
    free(e->modules);
    free(e->module_scheme);
    buf_free(&e->out_str);
    buf_free(&e->out_stack);
    buf_free(&e->rej_str);
    buf_free(&e->rej_stack);
    buf_free(&e->log_line);
    free(e);
}

void *kmpjs_get_user(kmpjs_engine *e)
{
    return e->user;
}

/* ---- value conversion ---- */

static int copy_js_string(JSContext *ctx, JSValueConst str, kmp_buf *b)
{
    size_t len;
    int rc;
    const char *p = JS_ToCStringLen(ctx, &len, str);
    if (!p)
        return -1;
    buf_reset(b);
    rc = buf_append(b, p, len);
    JS_FreeCString(ctx, p);
    return rc;
}

static int object_kind(JSContext *ctx, JSValueConst v)
{
    int kind = 0;
    if (JS_IsFunction(ctx, v))
        kind |= KMPJS_REF_FUNCTION;
    if (JS_IsArray(ctx, v) > 0)
        kind |= KMPJS_REF_ARRAY;
    /* the enum is unsigned on this compiler, so the -1 for "not a promise" needs the cast */
    if ((int)JS_PromiseState(ctx, v) >= 0)
        kind |= KMPJS_REF_PROMISE;
    return kind;
}

/* ArrayBuffer, SharedArrayBuffer and typed arrays leave as a copy of their bytes. Returns 0 for
   any other value; for a binary value returns 1 with *out filled, or with the exception pending
   (detached buffer) and out->tag left at KMPJS_TAG_EXCEPTION. */
static int binary_to_out(kmpjs_engine *e, JSValueConst v, kmp_buf *sbuf, kmpjs_value *out)
{
    JSContext *ctx = e->ctx;
    JSClassID cid = JS_GetClassID(v);
    const uint8_t *data;
    size_t len, offset = 0;
    JSValue buffer = JS_UNDEFINED;
    int rc;

    if (cid == e->cls_array_buffer || cid == e->cls_shared_array_buffer) {
        data = JS_GetArrayBuffer(ctx, &len, v);
    } else if (cid >= e->cls_typed_array_first && cid <= e->cls_typed_array_last) {
        buffer = JS_GetTypedArrayBuffer(ctx, v, &offset, &len, NULL);
        if (JS_IsException(buffer))
            data = NULL;
        else {
            size_t total;
            data = JS_GetArrayBuffer(ctx, &total, buffer);
        }
    } else {
        return 0;
    }
    out->tag = KMPJS_TAG_EXCEPTION;
    if (data) {
        buf_reset(sbuf);
        rc = buf_append(sbuf, data + offset, len);
        if (rc == 0) {
            out->tag = KMPJS_TAG_BINARY;
            publish(sbuf, &out->str, &out->str_len);
        } else {
            JS_ThrowOutOfMemory(ctx);
        }
    }
    JS_FreeValue(ctx, buffer);
    return 1;
}

/* Converts a borrowed JS value. Returns -1 with the exception left pending in ctx. */
static int value_to_out(kmpjs_engine *e, JSValueConst v, kmp_buf *sbuf, int32_t flags, kmpjs_value *out)
{
    JSContext *ctx = e->ctx;

    memset(out, 0, sizeof(*out));
    if (JS_IsUndefined(v)) {
        out->tag = KMPJS_TAG_UNDEFINED;
    } else if (JS_IsNull(v)) {
        out->tag = KMPJS_TAG_NULL;
    } else if (JS_IsBool(v)) {
        out->tag = KMPJS_TAG_BOOL;
        out->num = JS_ToBool(ctx, v) ? 1 : 0;
    } else if (JS_IsNumber(v)) {
        out->tag = KMPJS_TAG_NUMBER;
        if (JS_ToFloat64(ctx, &out->num, v))
            return -1;
    } else if (JS_IsString(v)) {
        out->tag = KMPJS_TAG_STRING;
        if (copy_js_string(ctx, v, sbuf))
            return -1;
        publish(sbuf, &out->str, &out->str_len);
    } else if (JS_IsBigInt(ctx, v)) {
        out->tag = KMPJS_TAG_BIGINT;
        if (copy_js_string(ctx, v, sbuf))
            return -1;
        publish(sbuf, &out->str, &out->str_len);
    } else if (flags & KMPJS_FLAG_REF_OBJECTS) {
        out->tag = KMPJS_TAG_REF;
        out->num = object_kind(ctx, v);
        out->ref = slot_new(e, JS_DupValue(ctx, v));
        if (!out->ref) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    } else if (JS_IsFunction(ctx, v)) {
        out->tag = KMPJS_TAG_OBJECT;
    } else if (binary_to_out(e, v, sbuf, out)) {
        return out->tag == KMPJS_TAG_BINARY ? 0 : -1;
    } else {
        JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(json))
            return -1;
        out->tag = KMPJS_TAG_OBJECT;
        if (!JS_IsUndefined(json)) {
            int rc = copy_js_string(ctx, json, sbuf);
            JS_FreeValue(ctx, json);
            if (rc)
                return -1;
            publish(sbuf, &out->str, &out->str_len);
        }
    }
    return 0;
}

/* Describes an owned error value (message = toString(), stack for Error objects) and frees it. */
static void error_to_out(kmpjs_engine *e, JSValue exc, kmp_buf *sbuf, kmp_buf *stackbuf, kmpjs_value *out)
{
    JSContext *ctx = e->ctx;
    JSValue s;
    static const char unknown[] = "unknown exception";

    memset(out, 0, sizeof(*out));
    out->tag = KMPJS_TAG_EXCEPTION;
    s = JS_ToString(ctx, exc);
    if (JS_IsException(s) || copy_js_string(ctx, s, sbuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        buf_reset(sbuf);
        buf_append(sbuf, unknown, sizeof(unknown) - 1);
    }
    JS_FreeValue(ctx, s);
    publish(sbuf, &out->str, &out->str_len);

    if (JS_IsError(ctx, exc)) {
        s = JS_GetPropertyStr(ctx, exc, "stack");
        if (JS_IsException(s)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            if (JS_IsString(s) && !copy_js_string(ctx, s, stackbuf))
                publish(stackbuf, &out->stack, &out->stack_len);
            JS_FreeValue(ctx, s);
        }
    }
    JS_FreeValue(ctx, exc);
}

static JSValue take_exception(kmpjs_engine *e, int *oom);
static void oom_to_out(kmpjs_engine *e, kmpjs_value *out);

static void exception_to_out(kmpjs_engine *e, kmpjs_value *out)
{
    int oom;
    JSValue exc = take_exception(e, &oom);
    if (oom)
        oom_to_out(e, out);
    else
        error_to_out(e, exc, &e->out_str, &e->out_stack, out);
}

static void report_rejections(kmpjs_engine *e)
{
    kmpjs_value reason;
    while (e->rejected_count > 0) {
        int32_t last = e->rejected_count - 1;
        JSValue r = JS_DupValue(e->ctx, e->rejected[last].reason);
        forget_rejected(e, last);
        error_to_out(e, r, &e->rej_str, &e->rej_stack, &reason);
        if (e->rejection) {
            e->rejection(e->user, &reason);
        } else if (e->log) {
            static const char prefix[] = "Unhandled promise rejection: ";
            buf_reset(&e->log_line);
            buf_append(&e->log_line, prefix, sizeof(prefix) - 1);
            buf_append(&e->log_line, reason.str, (size_t)reason.str_len);
            e->log(e->user, e->log_line.data, e->log_line.len);
        }
    }
}

/* A settled Promise result becomes its value or its (now handled) reason; a pending one is
   always handed out as a ref so the caller can keep it. */
static JSValue unwrap_promise(kmpjs_engine *e, JSValue v, int32_t *flags, JSModuleDef *module)
{
    JSContext *ctx = e->ctx;
    JSValue r;
    int32_t i;
    switch ((int)JS_PromiseState(ctx, v)) {
    case JS_PROMISE_FULFILLED:
        r = module ? JS_GetModuleNamespace(ctx, module) : JS_PromiseResult(ctx, v);
        JS_FreeValue(ctx, v);
        return r;
    case JS_PROMISE_REJECTED:
        r = JS_PromiseResult(ctx, v);
        i = find_rejected(e, v);
        if (i >= 0)
            forget_rejected(e, i);
        JS_FreeValue(ctx, v);
        return JS_Throw(ctx, r);
    case JS_PROMISE_PENDING:
        *flags |= KMPJS_FLAG_REF_OBJECTS;
        return v;
    default:
        return v;
    }
}

/* An error raised by the shim itself, with no JS exception pending. */
static int32_t fail_message(kmpjs_engine *e, kmpjs_value *out, const char *msg)
{
    memset(out, 0, sizeof(*out));
    out->tag = KMPJS_TAG_EXCEPTION;
    buf_reset(&e->out_str);
    buf_append(&e->out_str, msg, strlen(msg));
    publish(&e->out_str, &out->str, &out->str_len);
    return -1;
}

/* JS_Eval and JS_ParseJSON require input[len] == '\0'; callers hand in unterminated byte ranges. */
static char *terminated_copy(const char *p, int32_t len)
{
    char *buf = malloc((size_t)len + 1);
    if (!buf)
        return NULL;
    memcpy(buf, p ? p : "", (size_t)len);
    buf[len] = '\0';
    return buf;
}

static JSValue throw_message(JSContext *ctx, const char *p, int32_t len)
{
    JSValue err = JS_NewError(ctx);
    if (JS_IsException(err))
        return err;
    if (JS_DefinePropertyValueStr(ctx, err, "message", JS_NewStringLen(ctx, p ? p : "", (size_t)len),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, err);
        return JS_EXCEPTION;
    }
    return JS_Throw(ctx, err);
}

/* Returns an owned JSValue (or JS_EXCEPTION with the exception pending). */
static JSValue value_from_host(kmpjs_engine *e, const kmpjs_value *v)
{
    JSContext *ctx = e->ctx;
    switch (v->tag) {
    case KMPJS_TAG_UNDEFINED:
        return JS_UNDEFINED;
    case KMPJS_TAG_NULL:
        return JS_NULL;
    case KMPJS_TAG_BOOL:
        return JS_NewBool(ctx, v->num != 0);
    case KMPJS_TAG_NUMBER:
        return JS_NewFloat64(ctx, v->num);
    case KMPJS_TAG_STRING:
        return JS_NewStringLen(ctx, v->str ? v->str : "", (size_t)v->str_len);
    case KMPJS_TAG_OBJECT: {
        char *buf;
        JSValue r;
        if (!v->str)
            return JS_UNDEFINED;
        buf = terminated_copy(v->str, v->str_len);
        if (!buf)
            return JS_ThrowOutOfMemory(ctx);
        r = JS_ParseJSON(ctx, buf, (size_t)v->str_len, "<host>");
        free(buf);
        return r;
    }
    case KMPJS_TAG_REF: {
        kmp_slot *s = slot_get(e, v->ref);
        if (!s)
            return JS_ThrowTypeError(ctx, "invalid or released ref");
        return JS_DupValue(ctx, s->val);
    }
    case KMPJS_TAG_BIGINT: {
        JSValue text = JS_NewStringLen(ctx, v->str ? v->str : "", (size_t)v->str_len);
        JSValue r;
        if (JS_IsException(text))
            return text;
        r = JS_Call(ctx, e->bigint_ctor, JS_UNDEFINED, 1, &text);
        JS_FreeValue(ctx, text);
        return r;
    }
    case KMPJS_TAG_BINARY:
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)(v->str ? v->str : ""), (size_t)v->str_len);
    default:
        return throw_message(ctx, v->str, v->str_len);
    }
}

/* Marks the pending exception as captured: the value is kept, formatting waits until the end. */
static JSValue take_exception(kmpjs_engine *e, int *oom)
{
    JSValue exc = JS_GetException(e->ctx);
    *oom = JS_IsNull(exc) && e->oom;
    e->oom = 0;
    return exc;
}

static void oom_to_out(kmpjs_engine *e, kmpjs_value *out)
{
    static const char oom[] = "InternalError: out of memory";
    memset(out, 0, sizeof(*out));
    out->tag = KMPJS_TAG_EXCEPTION;
    buf_reset(&e->out_str);
    buf_append(&e->out_str, oom, sizeof(oom) - 1);
    publish(&e->out_str, &out->str, &out->str_len);
}

/* Converts an owned result and frees it; on failure the exception goes to *out.
   Order matters: the outermost call drains microtasks, unwraps a Promise result when asked and
   reports rejections first, and only then writes *out. Anything that runs script after the write
   (a job calling a host function that re-enters the engine) would let a nested finish overwrite
   the shared output buffers, so when the final conversion itself queues work (a toJSON that
   creates promises) the finished payload is set aside while that work runs. The call's own
   exception is taken before the drain so a failing job cannot replace it. */
static int32_t finish(kmpjs_engine *e, int outermost, JSValue v, int32_t flags, int unwrap, JSModuleDef *module, kmpjs_value *out)
{
    JSContext *ctx = e->ctx;
    JSValue exc = JS_UNDEFINED;
    int failed = 0, oom = 0;

    if (JS_IsException(v)) {
        failed = 1;
        exc = take_exception(e, &oom);
    }
    if (outermost && drain_jobs(e) < 0) {
        if (failed) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            failed = 1;
            exc = take_exception(e, &oom);
            JS_FreeValue(ctx, v);
            v = JS_UNDEFINED;
        }
    }
    if (!failed && unwrap) {
        v = unwrap_promise(e, v, &flags, module);
        if (JS_IsException(v)) {
            failed = 1;
            exc = take_exception(e, &oom);
        }
    }
    if (outermost)
        report_rejections(e);
    if (failed) {
        if (oom)
            oom_to_out(e, out);
        else
            error_to_out(e, exc, &e->out_str, &e->out_stack, out);
    } else if (value_to_out(e, v, &e->out_str, flags, out)) {
        failed = 1;
        exception_to_out(e, out);
    } else if (outermost && (JS_IsJobPending(e->rt) || e->rejected_count > 0)) {
        kmp_buf keep = e->out_str;
        int rc;
        memset(&e->out_str, 0, sizeof(e->out_str));
        rc = drain_jobs(e);
        buf_free(&e->out_str);
        e->out_str = keep;
        if (rc < 0) {
            failed = 1;
            exception_to_out(e, out);
        } else {
            report_rejections(e);
        }
    }
    JS_FreeValue(ctx, v);
    run_end(e, outermost);
    return failed ? -1 : 0;
}

/* ---- evaluation ---- */

void kmpjs_eval(kmpjs_engine *e, const char *code, int32_t code_len, const char *filename, int32_t flags, kmpjs_value *out)
{
    int outermost = run_begin(e);
    char *buf = terminated_copy(code, code_len);
    JSValue r;

    if (!buf) {
        fail_message(e, out, "out of memory");
        run_end(e, outermost);
        return;
    }
    r = JS_Eval(e->ctx, buf, (size_t)code_len, filename, JS_EVAL_TYPE_GLOBAL);
    free(buf);
    finish(e, outermost, r, flags, 1, NULL, out);
}

static int32_t module_name_taken(kmpjs_engine *e, const char *name, kmpjs_value *out)
{
    kmp_module *mod = find_module(e, name);
    char msg[192];
    if (!mod)
        return 0;
    snprintf(msg, sizeof msg, mod->kind == KMP_MODULE_EVALUATED ? "module '%.128s' was already evaluated" : "module '%.128s' is already registered", name);
    return fail_message(e, out, msg);
}

/* code is only kept for KMP_MODULE_SOURCE. */
static int32_t claim_module_name(kmpjs_engine *e, const char *name, int kind, const char *code, int32_t code_len, kmpjs_value *out)
{
    kmp_module *mod;
    if (e->module_count == e->module_cap) {
        int32_t cap = e->module_cap ? e->module_cap * 2 : 8;
        kmp_module *list = realloc(e->modules, (size_t)cap * sizeof(*list));
        if (!list)
            return fail_message(e, out, "out of memory");
        e->modules = list;
        e->module_cap = cap;
    }
    mod = &e->modules[e->module_count];
    mod->name = strdup(name);
    mod->source = kind == KMP_MODULE_SOURCE ? terminated_copy(code, code_len) : NULL;
    mod->kind = kind;
    if (!mod->name || (kind == KMP_MODULE_SOURCE && !mod->source)) {
        free(mod->name);
        free(mod->source);
        return fail_message(e, out, "out of memory");
    }
    mod->len = (size_t)code_len;
    e->module_count++;
    return 0;
}

int32_t kmpjs_register_module(kmpjs_engine *e, const char *name, const char *code, int32_t code_len, kmpjs_value *out)
{
    if (!name[0])
        return fail_message(e, out, "module name must not be empty");
    if (module_name_taken(e, name, out) || claim_module_name(e, name, KMP_MODULE_SOURCE, code ? code : "", code_len, out))
        return -1;
    memset(out, 0, sizeof(*out));
    return 0;
}

/* Names in angle brackets ("<module>") are anonymous: not importable, never recorded. */
int32_t kmpjs_eval_module(kmpjs_engine *e, const char *code, int32_t code_len, const char *name, int32_t flags, kmpjs_value *out)
{
    int anonymous = name[0] == '<';
    int outermost;
    char *buf;
    JSValue obj, promise;
    JSModuleDef *m;

    if (!anonymous && module_name_taken(e, name, out))
        return -1;
    outermost = run_begin(e);
    buf = terminated_copy(code, code_len);
    if (!buf) {
        fail_message(e, out, "out of memory");
        run_end(e, outermost);
        return -1;
    }
    obj = compile_module(e, buf, (size_t)code_len, name);
    free(buf);
    if (JS_IsException(obj))
        return finish(e, outermost, obj, flags, 0, NULL, out);
    /* from here on the context caches the module under this name, so the name is spent */
    if (!anonymous && claim_module_name(e, name, KMP_MODULE_EVALUATED, NULL, 0, out)) {
        JS_FreeValue(e->ctx, obj);
        run_end(e, outermost);
        return -1;
    }
    m = JS_VALUE_GET_PTR(obj);
    promise = JS_EvalFunction(e->ctx, obj);
    return finish(e, outermost, promise, flags | KMPJS_FLAG_REF_OBJECTS, 1, m, out);
}

int32_t kmpjs_run_bytecode(kmpjs_engine *e, const uint8_t *buf, int32_t len, int32_t flags, kmpjs_value *out)
{
    int outermost = run_begin(e);
    int kind, anonymous;
    const uint8_t *body;
    size_t body_len;
    char *name = NULL;
    JSValue obj;
    JSModuleDef *m;

    if (check_bytecode_header(e, buf, len, &kind, &body, &body_len, out))
        goto fail;
    if (kind == 0) {
        obj = read_bytecode_body(e, body, body_len, kind, out);
        if (JS_IsException(obj))
            goto fail;
        return finish(e, outermost, JS_EvalFunction(e->ctx, obj), flags, 1, NULL, out);
    }
    name = peek_module_name(e, body, body_len, out);
    if (!name)
        goto fail;
    anonymous = name[0] == '<';
    if (!anonymous && module_name_taken(e, name, out))
        goto fail;
    obj = read_bytecode_body(e, body, body_len, kind, out);
    if (JS_IsException(obj))
        goto fail;
    m = JS_VALUE_GET_PTR(obj);
    if (set_import_meta(e, m, name) || JS_ResolveModule(e->ctx, obj) < 0) {
        JS_FreeValue(e->ctx, obj);
        free(name);
        return finish(e, outermost, JS_EXCEPTION, flags, 0, NULL, out);
    }
    if (!anonymous && claim_module_name(e, name, KMP_MODULE_EVALUATED, NULL, 0, out)) {
        JS_FreeValue(e->ctx, obj);
        goto fail;
    }
    free(name);
    return finish(e, outermost, JS_EvalFunction(e->ctx, obj), flags | KMPJS_FLAG_REF_OBJECTS, 1, m, out);
fail:
    free(name);
    run_end(e, outermost);
    return -1;
}

/* The module is read into the engine right away (the engine caches it under its compiled name from
   then on), but only after its name passed the check: a rejected registration must not leave a
   definition behind that would shadow a source module still waiting for its first import. */
int32_t kmpjs_register_module_bytecode(kmpjs_engine *e, const uint8_t *buf, int32_t len, kmpjs_value *out)
{
    int kind;
    const uint8_t *body;
    size_t body_len;
    char *name;
    JSValue obj;
    int32_t rc = -1;

    if (check_bytecode_header(e, buf, len, &kind, &body, &body_len, out))
        return -1;
    if (kind != 1)
        return fail_message(e, out, "bytecode is a script, not a module");
    name = peek_module_name(e, body, body_len, out);
    if (!name)
        return -1;
    if (name[0] == '<' || !name[0]) {
        fail_message(e, out, "bytecode module has an anonymous name; compile it with a real one");
    } else if (!module_name_taken(e, name, out)) {
        obj = read_bytecode_body(e, body, body_len, kind, out);
        if (!JS_IsException(obj)) {
            if (set_import_meta(e, JS_VALUE_GET_PTR(obj), name)) {
                exception_to_out(e, out);
            } else if (claim_module_name(e, name, KMP_MODULE_BYTECODE, NULL, 0, out) == 0) {
                buf_reset(&e->out_str);
                buf_append(&e->out_str, name, strlen(name));
                memset(out, 0, sizeof(*out));
                out->tag = KMPJS_TAG_STRING;
                publish(&e->out_str, &out->str, &out->str_len);
                rc = 0;
            }
            JS_FreeValue(e->ctx, obj);
        }
    }
    free(name);
    return rc;
}

static JSValue js_kmp_host(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);

int32_t kmpjs_define_function(kmpjs_engine *e, const char *name, int32_t fn_id, int32_t flags, kmpjs_value *out)
{
    JSContext *ctx = e->ctx;
    int magic = (fn_id << 1) | (flags & KMPJS_FLAG_REF_OBJECTS);
    JSValue fn, global;
    int rc;

    /* the engine stores magic as int16_t: fn_id must fit in 14 bits next to the flag bit */
    if (fn_id < 0 || fn_id > KMPJS_MAX_FN_ID)
        return fail_message(e, out, "too many host functions");
    fn = JS_NewCFunctionMagic(ctx, js_kmp_host, name, 0, JS_CFUNC_generic_magic, magic);
    if (JS_IsException(fn)) {
        exception_to_out(e, out);
        return -1;
    }
    global = JS_GetGlobalObject(ctx);
    rc = JS_SetPropertyStr(ctx, global, name, fn);
    JS_FreeValue(ctx, global);
    if (rc < 0) {
        exception_to_out(e, out);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    return 0;
}

static JSValue js_kmp_host(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    kmpjs_engine *e = JS_GetContextOpaque(ctx);
    int32_t fn_id = magic >> 1;
    int32_t flags = magic & KMPJS_FLAG_REF_OBJECTS;
    kmpjs_value *args = NULL;
    kmp_buf *bufs = NULL;
    kmpjs_value result = {0};
    JSValue ret = JS_EXCEPTION;
    int i, rc, converted = 0;

    if (!e->host)
        return JS_ThrowInternalError(ctx, "no host function handler");
    if (argc > 0) {
        args = calloc((size_t)argc, sizeof(*args));
        bufs = calloc((size_t)argc, sizeof(*bufs));
        if (!args || !bufs) {
            ret = JS_ThrowOutOfMemory(ctx);
            goto done;
        }
    }
    for (i = 0; i < argc; i++) {
        if (value_to_out(e, argv[i], &bufs[i], flags, &args[i]))
            goto done;
        converted++;
    }
    rc = e->host(e->user, fn_id, args, argc, &result);
    if (rc != 0)
        ret = throw_message(ctx, result.str, result.str_len);
    else
        ret = value_from_host(e, &result);
    free((void *)result.str);
    free((void *)result.stack);
done:
    /* refs handed to the host for this call are transient unless the host retained them */
    for (i = 0; i < converted; i++) {
        if (args[i].tag == KMPJS_TAG_REF)
            kmpjs_ref_release(e, args[i].ref);
    }
    for (i = 0; i < argc && bufs; i++)
        buf_free(&bufs[i]);
    free(bufs);
    free(args);
    return ret;
}

/* ---- ref operations ---- */

/* Property access and JSON.stringify can run script (accessors, toJSON), so every ref
   operation enters the running state like kmpjs_eval does to stay interruptible. */
int32_t kmpjs_ref_get(kmpjs_engine *e, int64_t ref, const char *name, int32_t flags, kmpjs_value *out)
{
    kmp_slot *s = slot_get(e, ref);
    int outermost;
    if (!s)
        return fail_message(e, out, "invalid or released ref");
    outermost = run_begin(e);
    return finish(e, outermost, JS_GetPropertyStr(e->ctx, s->val, name), flags, 0, NULL, out);
}

int32_t kmpjs_ref_get_index(kmpjs_engine *e, int64_t ref, int32_t index, int32_t flags, kmpjs_value *out)
{
    kmp_slot *s = slot_get(e, ref);
    int outermost;
    if (!s)
        return fail_message(e, out, "invalid or released ref");
    if (index < 0)
        return fail_message(e, out, "negative index");
    outermost = run_begin(e);
    return finish(e, outermost, JS_GetPropertyUint32(e->ctx, s->val, (uint32_t)index), flags, 0, NULL, out);
}

int32_t kmpjs_ref_set(kmpjs_engine *e, int64_t ref, const char *name, const kmpjs_value *value, kmpjs_value *out)
{
    JSContext *ctx = e->ctx;
    kmp_slot *s = slot_get(e, ref);
    JSValue v;
    int outermost;
    if (!s)
        return fail_message(e, out, "invalid or released ref");
    outermost = run_begin(e);
    v = value_from_host(e, value);
    if (JS_IsException(v) || JS_SetPropertyStr(ctx, s->val, name, v) < 0)
        return finish(e, outermost, JS_EXCEPTION, 0, 0, NULL, out);
    return finish(e, outermost, JS_UNDEFINED, 0, 0, NULL, out);
}

int32_t kmpjs_ref_call(kmpjs_engine *e, int64_t ref, int64_t this_ref, const kmpjs_value *args,
                       int32_t argc, int32_t flags, kmpjs_value *out)
{
    JSContext *ctx = e->ctx;
    kmp_slot *fs = slot_get(e, ref);
    kmp_slot *ts = NULL;
    JSValue *argv = NULL;
    JSValue r = JS_EXCEPTION;
    int i, converted = 0, outermost;

    if (!fs)
        return fail_message(e, out, "invalid or released ref");
    if (!JS_IsFunction(ctx, fs->val))
        return fail_message(e, out, "ref is not a function");
    if (this_ref) {
        ts = slot_get(e, this_ref);
        if (!ts)
            return fail_message(e, out, "invalid or released this ref");
    }
    outermost = run_begin(e);
    if (argc > 0) {
        argv = calloc((size_t)argc, sizeof(*argv));
        if (!argv) {
            fail_message(e, out, "out of memory");
            run_end(e, outermost);
            return -1;
        }
    }
    for (i = 0; i < argc; i++) {
        argv[i] = value_from_host(e, &args[i]);
        if (JS_IsException(argv[i]))
            goto done;
        converted++;
    }
    r = JS_Call(ctx, fs->val, ts ? ts->val : JS_UNDEFINED, argc, argv);
done:
    for (i = 0; i < converted; i++)
        JS_FreeValue(ctx, argv[i]);
    free(argv);
    return finish(e, outermost, r, flags, 1, NULL, out);
}

int32_t kmpjs_ref_to_json(kmpjs_engine *e, int64_t ref, kmpjs_value *out)
{
    kmp_slot *s = slot_get(e, ref);
    JSValue json;
    int outermost, rc = 0;
    if (!s)
        return fail_message(e, out, "invalid or released ref");
    memset(out, 0, sizeof(*out));
    out->tag = KMPJS_TAG_OBJECT;
    if (JS_IsFunction(e->ctx, s->val))
        return 0;
    outermost = run_begin(e);
    json = JS_JSONStringify(e->ctx, s->val, JS_UNDEFINED, JS_UNDEFINED);
    rc = finish(e, outermost, json, 0, 0, NULL, out);
    if (rc == 0)
        out->tag = KMPJS_TAG_OBJECT; /* the JSON text, or no payload when nothing is serializable */
    return rc;
}
