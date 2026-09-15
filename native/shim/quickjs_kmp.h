/* Handle-based C API over QuickJS shared by the JNI and cinterop bindings.
   Design notes: docs/architecture.md */
#ifndef QUICKJS_KMP_H
#define QUICKJS_KMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KMPJS_ABI_VERSION 2

typedef struct kmpjs_engine kmpjs_engine;

enum {
    KMPJS_TAG_UNDEFINED = 0,
    KMPJS_TAG_NULL = 1,
    KMPJS_TAG_BOOL = 2,
    KMPJS_TAG_NUMBER = 3,
    KMPJS_TAG_STRING = 4,
    KMPJS_TAG_OBJECT = 5,    /* str = JSON text, or NULL when not serializable */
    KMPJS_TAG_EXCEPTION = 6, /* str = message, stack = JS stack trace or NULL */
    KMPJS_TAG_REF = 7,       /* ref = 64-bit handle (slot index | generation), num = KMPJS_REF_* kind bits */
};

/* KMPJS_TAG_REF kind bits carried in kmpjs_value.num */
enum {
    KMPJS_REF_FUNCTION = 1,
    KMPJS_REF_ARRAY = 2,
    KMPJS_REF_PROMISE = 4,
};

/* kmpjs_define_function rejects fn_id above this (the engine keeps the id in 14 bits) */
#define KMPJS_MAX_FN_ID 0x3FFF

/* flags for kmpjs_eval / kmpjs_define_function / kmpjs_ref_* */
enum {
    KMPJS_FLAG_REF_OBJECTS = 1, /* hand objects out as KMPJS_TAG_REF instead of JSON */
};

typedef struct {
    int32_t tag;
    int32_t reserved;
    int64_t ref;
    double num;          /* KMPJS_TAG_BOOL: 0 or 1, KMPJS_TAG_NUMBER: the value, KMPJS_TAG_REF: kind bits */
    const char *str;     /* UTF-8 (WTF-8), not NUL terminated */
    int32_t str_len;
    const char *stack;
    int32_t stack_len;
} kmpjs_value;

/* Zero means "engine default" for every field. Sizes are bytes. */
typedef struct {
    int64_t memory_limit;   /* JS_SetMemoryLimit; 0 = unlimited */
    int64_t max_stack_size; /* JS_SetMaxStackSize; must stay below the calling thread's stack, 0 disables the check */
    int64_t gc_threshold;   /* JS_SetGCThreshold */
} kmpjs_config;

/* Host function callback. `args` is only valid during the call.
   Return 0 with *result filled, or non-zero with result->str holding an error message
   that is thrown as an Error. String payloads placed in *result must come from
   kmpjs_alloc(); the engine frees them. */
typedef int (*kmpjs_host_fn)(void *user, int32_t fn_id, const kmpjs_value *args,
                             int32_t argc, kmpjs_value *result);
typedef void (*kmpjs_log_fn)(void *user, const char *msg, int32_t len);
/* A promise rejected during the call that just finished and still unhandled when it returns.
   `reason` is a KMPJS_TAG_EXCEPTION value valid only during the callback. */
typedef void (*kmpjs_rejection_fn)(void *user, const kmpjs_value *reason);

int32_t kmpjs_abi_version(void);

/* Returns NULL when the runtime cannot be created. */
kmpjs_engine *kmpjs_create(const kmpjs_config *config, void *user, kmpjs_host_fn host, kmpjs_log_fn log,
                           kmpjs_rejection_fn rejection);
void kmpjs_destroy(kmpjs_engine *e);
void *kmpjs_get_user(kmpjs_engine *e);

/* String payloads in *out stay valid until the next kmpjs_* call on the same engine.
   The outermost call (kmpjs_eval / kmpjs_ref_get* / kmpjs_ref_call, not nested in a host function)
   drains the microtask queue before returning; unhandled rejections are then reported through
   kmpjs_rejection_fn. A Promise result is unwrapped: fulfilled yields its value, rejected raises
   its reason, pending comes back as a KMPJS_REF_PROMISE ref whatever the flags say. */
void kmpjs_eval(kmpjs_engine *e, const char *code, int32_t code_len,
                const char *filename, int32_t flags, kmpjs_value *out);

/* Defines global `name` as a host function dispatching to fn_id. With KMPJS_FLAG_REF_OBJECTS the
   host receives object arguments as refs that are released after the call unless retained.
   Returns 0 on success, otherwise *out holds the exception. */
int32_t kmpjs_define_function(kmpjs_engine *e, const char *name, int32_t fn_id, int32_t flags, kmpjs_value *out);

/* Refs are reference counted handles to JS objects, valid until released or the engine is destroyed.
   All kmpjs_ref_* calls return 0 on success or -1 with *out holding the exception. */
void kmpjs_ref_retain(kmpjs_engine *e, int64_t ref);
void kmpjs_ref_release(kmpjs_engine *e, int64_t ref);
int32_t kmpjs_ref_get(kmpjs_engine *e, int64_t ref, const char *name, int32_t flags, kmpjs_value *out);
int32_t kmpjs_ref_get_index(kmpjs_engine *e, int64_t ref, int32_t index, int32_t flags, kmpjs_value *out);
int32_t kmpjs_ref_set(kmpjs_engine *e, int64_t ref, const char *name, const kmpjs_value *value, kmpjs_value *out);
/* Calls the function behind `ref` with `this_ref` (0 for undefined) and `args`. */
int32_t kmpjs_ref_call(kmpjs_engine *e, int64_t ref, int64_t this_ref, const kmpjs_value *args,
                       int32_t argc, int32_t flags, kmpjs_value *out);
int32_t kmpjs_ref_to_json(kmpjs_engine *e, int64_t ref, kmpjs_value *out);

/* Safe to call from any thread while the engine is alive. Stops the evaluation in
   progress; a call while no evaluation runs is a no-op. */
void kmpjs_interrupt(kmpjs_engine *e);

typedef struct {
    int32_t live_refs;  /* outstanding releases: every retain adds one */
    int32_t ref_slots;  /* slots allocated so far (live + reusable) */
} kmpjs_stats;

void kmpjs_get_stats(kmpjs_engine *e, kmpjs_stats *stats);
/* Text summary of the runtime heap (JS_DumpMemoryUsage) in out->str; diagnostics only. */
void kmpjs_dump_memory(kmpjs_engine *e, kmpjs_value *out);

char *kmpjs_alloc(int32_t len);
void kmpjs_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_KMP_H */
