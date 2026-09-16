/* Host-side checks of the shim, run under ASan.
   Exit status is 1 when any check failed, else 0. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "quickjs_kmp.h"

static int failures;
static kmpjs_engine *g;
static int64_t retained_ref;
static char last_log[512];
static int rejections;
static char last_rejection[512];

#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define S(x) x, (int32_t)strlen(x)

static int str_is(const kmpjs_value *v, const char *expected)
{
    return v->str && v->str_len == (int32_t)strlen(expected) && memcmp(v->str, expected, (size_t)v->str_len) == 0;
}

static int str_has(const kmpjs_value *v, const char *needle)
{
    char buf[1024];
    int32_t n = v->str_len < (int32_t)sizeof(buf) - 1 ? v->str_len : (int32_t)sizeof(buf) - 1;
    if (!v->str)
        return 0;
    memcpy(buf, v->str, (size_t)n);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static void set_str(kmpjs_value *v, int32_t tag, const char *text)
{
    memset(v, 0, sizeof(*v));
    v->tag = tag;
    v->str = kmpjs_alloc((int32_t)strlen(text));
    memcpy((char *)v->str, text, strlen(text));
    v->str_len = (int32_t)strlen(text);
}

static kmpjs_value eval(const char *code, int32_t flags)
{
    kmpjs_value out;
    kmpjs_eval(g, code, (int32_t)strlen(code), "<test>", flags, &out);
    return out;
}

static int host(void *user, int32_t id, const kmpjs_value *args, int32_t argc, kmpjs_value *result)
{
    memset(result, 0, sizeof(*result));
    switch (id) {
    case 0: /* echo argument count as "ok:N" */
        {
            char buf[32];
            snprintf(buf, sizeof buf, "ok:%d", argc);
            set_str(result, KMPJS_TAG_STRING, buf);
            CHECK(argc == 5);
            CHECK(args[0].tag == KMPJS_TAG_NUMBER && args[0].num == 1);
            CHECK(args[1].tag == KMPJS_TAG_STRING && str_is(&args[1], "two"));
            CHECK(args[2].tag == KMPJS_TAG_BOOL && args[2].num == 1);
            CHECK(args[3].tag == KMPJS_TAG_NULL);
            CHECK(args[4].tag == KMPJS_TAG_OBJECT && str_is(&args[4], "{\"k\":[3]}"));
            return 0;
        }
    case 1: /* throw */
        set_str(result, KMPJS_TAG_EXCEPTION, "host failed");
        return 1;
    case 2: /* object result */
        set_str(result, KMPJS_TAG_OBJECT, "{\"n\":7,\"s\":\"x\"}");
        return 0;
    case 3: /* interrupt from inside script */
        kmpjs_interrupt(g);
        result->tag = KMPJS_TAG_UNDEFINED;
        return 0;
    case 4: /* keep a ref argument */
        CHECK(args[0].tag == KMPJS_TAG_REF && (int)args[0].num == 0);
        CHECK(args[1].tag == KMPJS_TAG_REF && ((int)args[1].num & KMPJS_REF_ARRAY));
        CHECK(args[2].tag == KMPJS_TAG_STRING);
        kmpjs_ref_retain(g, args[0].ref);
        retained_ref = args[0].ref;
        result->tag = KMPJS_TAG_UNDEFINED;
        return 0;
    case 5: /* give the retained ref back */
        result->tag = KMPJS_TAG_REF;
        result->ref = retained_ref;
        return 0;
    case 7: /* report(n) -> n */
        result->tag = KMPJS_TAG_NUMBER;
        result->num = argc > 0 ? args[0].num : 0;
        return 0;
    case 6: /* nested evaluation */
        {
            kmpjs_value o = eval("nestedCounter = nestedCounter + 1", 0);
            result->tag = KMPJS_TAG_NUMBER;
            result->num = o.num;
            return 0;
        }
    default:
        result->tag = KMPJS_TAG_UNDEFINED;
        return 0;
    }
}

static void logger(void *user, const char *msg, int32_t len)
{
    int32_t n = len < (int32_t)sizeof(last_log) - 1 ? len : (int32_t)sizeof(last_log) - 1;
    memcpy(last_log, msg, (size_t)n);
    last_log[n] = '\0';
}

static void rejection(void *user, const kmpjs_value *reason)
{
    int32_t n = reason->str_len < (int32_t)sizeof(last_rejection) - 1 ? reason->str_len : (int32_t)sizeof(last_rejection) - 1;
    CHECK(reason->tag == KMPJS_TAG_EXCEPTION);
    memcpy(last_rejection, reason->str, (size_t)n);
    last_rejection[n] = '\0';
    rejections++;
}

static void test_promises(void)
{
    kmpjs_value v, out;
    kmpjs_stats st;

    /* microtasks run after the script body, before the call returns */
    v = eval("var order = []; Promise.resolve().then(() => order.push('micro')); order.push('sync'); order.join()", 0);
    CHECK(str_is(&v, "sync"));
    v = eval("order.join()", 0); CHECK(str_is(&v, "sync,micro"));
    /* a nested call from a host function does not drain early */
    v = eval("order = []; Promise.resolve().then(() => order.push('micro')); nested(); order.push('sync'); order.join()", 0);
    CHECK(str_is(&v, "sync"));
    v = eval("order.join()", 0); CHECK(str_is(&v, "sync,micro"));
    /* a job that re-enters the engine through a host function must not clobber the call's own error */
    v = eval("Promise.resolve().then(() => nested()); null.x", 0);
    CHECK(v.tag == KMPJS_TAG_EXCEPTION && str_is(&v, "TypeError: cannot read property 'x' of null"));
    v = eval("Promise.resolve().then(() => nested()); 'kept'", 0); CHECK(str_is(&v, "kept"));
    /* work queued by toJSON while the result is being serialized still runs inside this call */
    v = eval("order = []; ({toJSON() { Promise.resolve().then(() => order.push('fromToJSON')); return {a: 1}; }})", 0);
    CHECK(v.tag == KMPJS_TAG_OBJECT && str_is(&v, "{\"a\":1}"));
    v = eval("order.join()", 0); CHECK(str_is(&v, "fromToJSON"));
    v = eval("({toJSON() { Promise.reject(new Error('from toJSON')); return 1; }})", 0); CHECK(v.tag == KMPJS_TAG_OBJECT && str_is(&v, "1"));
    CHECK(rejections == 1 && strcmp(last_rejection, "Error: from toJSON") == 0);
    rejections = 0;
    /* settled promises are unwrapped, pending ones come back as refs */
    v = eval("(async () => { await null; return 6 * 7; })()", 0); CHECK(v.tag == KMPJS_TAG_NUMBER && v.num == 42);
    v = eval("(async () => { throw new Error('nope'); })()", 0); CHECK(v.tag == KMPJS_TAG_EXCEPTION && str_is(&v, "Error: nope"));
    CHECK(rejections == 0);
    v = eval("new Promise(() => {})", 0); CHECK(v.tag == KMPJS_TAG_REF && ((int)v.num & KMPJS_REF_PROMISE));
    kmpjs_ref_release(g, v.ref);
    /* unhandled rejections are reported once, handled ones are not */
    v = eval("Promise.reject(new Error('lost')); 1", 0); CHECK(v.num == 1);
    CHECK(rejections == 1 && strcmp(last_rejection, "Error: lost") == 0);
    v = eval("var p = Promise.reject(new Error('caught')); p.catch(() => {}); 2", 0); CHECK(v.num == 2);
    CHECK(rejections == 1);
    /* an interrupt from inside the chain stops the drain */
    v = eval("var n = 0; function loop() { if (++n === 100) stop(); Promise.resolve().then(loop); } loop(); 3", 0);
    CHECK(v.tag == KMPJS_TAG_EXCEPTION && str_has(&v, "interrupted"));
    /* the leftover chain was discarded: the engine is idle again and later promises still work */
    v = eval("n", 0); CHECK(v.tag == KMPJS_TAG_NUMBER && v.num >= 100);
    v = eval("(async () => 5)()", 0); CHECK(v.tag == KMPJS_TAG_NUMBER && v.num == 5);
    v = eval("n", 0); CHECK(v.tag == KMPJS_TAG_NUMBER);
    CHECK(rejections == 1);
    kmpjs_get_stats(g, &st); CHECK(st.live_refs == 0);
    (void)out;
}

static void test_bigint_and_binary(void)
{
    kmpjs_value v, arg, out;
    int64_t f;

    v = eval("2n ** 64n", 0); CHECK(v.tag == KMPJS_TAG_BIGINT && str_is(&v, "18446744073709551616"));
    v = eval("-7n", 0); CHECK(v.tag == KMPJS_TAG_BIGINT && str_is(&v, "-7"));
    v = eval("2n ** 64n", KMPJS_FLAG_REF_OBJECTS); CHECK(v.tag == KMPJS_TAG_BIGINT);
    v = eval("new Uint8Array([1, 2, 3])", 0); CHECK(v.tag == KMPJS_TAG_BINARY && v.str_len == 3 && memcmp(v.str, "\1\2\3", 3) == 0);
    v = eval("new Uint8Array([9, 1, 2, 3, 9]).subarray(1, 4)", 0); CHECK(v.tag == KMPJS_TAG_BINARY && v.str_len == 3 && memcmp(v.str, "\1\2\3", 3) == 0);
    v = eval("new Int32Array([1]).buffer", 0); CHECK(v.tag == KMPJS_TAG_BINARY && v.str_len == 4);
    v = eval("new Float64Array(2)", 0); CHECK(v.tag == KMPJS_TAG_BINARY && v.str_len == 16);
    v = eval("new ArrayBuffer(0)", 0); CHECK(v.tag == KMPJS_TAG_BINARY && v.str_len == 0);
    v = eval("new DataView(new ArrayBuffer(4))", 0); CHECK(v.tag == KMPJS_TAG_OBJECT);
    v = eval("var ab = new ArrayBuffer(4); structuredClone(ab, {transfer: [ab]}); ab", 0); CHECK(v.tag == KMPJS_TAG_EXCEPTION || v.tag == KMPJS_TAG_BINARY);
    v = eval("new Uint8Array([1])", KMPJS_FLAG_REF_OBJECTS); CHECK(v.tag == KMPJS_TAG_REF && ((int)v.num & KMPJS_REF_ARRAY) == 0);
    kmpjs_ref_release(g, v.ref);
    /* into the engine: BigInt from text, bytes as an ArrayBuffer */
    v = eval("(function (n, buf) { return typeof n + ':' + (n + 1n) + ':' + buf.byteLength + ':' + new Uint8Array(buf).join(); })", KMPJS_FLAG_REF_OBJECTS);
    f = v.ref;
    memset(&arg, 0, sizeof arg);
    {
        kmpjs_value args[2];
        set_str(&args[0], KMPJS_TAG_BIGINT, "18446744073709551616");
        set_str(&args[1], KMPJS_TAG_BINARY, "\5\6");
        CHECK(kmpjs_ref_call(g, f, 0, args, 2, 0, &v) == 0 && str_is(&v, "bigint:18446744073709551617:2:5,6"));
        kmpjs_free((void *)args[0].str);
        kmpjs_free((void *)args[1].str);
        set_str(&args[0], KMPJS_TAG_BIGINT, "not a number");
        CHECK(kmpjs_ref_call(g, f, 0, args, 1, 0, &v) != 0 && str_has(&v, "SyntaxError"));
        kmpjs_free((void *)args[0].str);
    }
    kmpjs_ref_release(g, f);
    (void)out;
}

static void test_modules(void)
{
    kmpjs_value v, out;
    kmpjs_stats st;
    int64_t ns;

    CHECK(kmpjs_register_module(g, "counter", S("export let n = 0; export function bump() { return ++n; } export const url = import.meta.url;"), &out) == 0);
    CHECK(kmpjs_register_module(g, "counter", S("export const dup = 1;"), &out) != 0 && str_has(&out, "already registered"));
    CHECK(kmpjs_register_module(g, "", S("export const x = 1;"), &out) != 0);
    CHECK(kmpjs_register_module(g, "broken", S("export const = ;"), &out) == 0);
    CHECK(kmpjs_eval_module(g, S("import { bump, url } from 'counter'; bump(); export const seen = url;"), "a", 0, &v) == 0 && v.tag == KMPJS_TAG_REF);
    ns = v.ref;
    CHECK(kmpjs_ref_get(g, ns, "seen", 0, &v) == 0 && str_is(&v, "test:counter"));
    kmpjs_ref_release(g, ns);
    /* the module ran once: a second importer sees the shared state */
    CHECK(kmpjs_eval_module(g, S("import { n } from 'counter'; export default n; export const missing = typeof nope;"), "b", 0, &v) == 0);
    ns = v.ref;
    CHECK(kmpjs_ref_get(g, ns, "default", 0, &v) == 0 && v.tag == KMPJS_TAG_NUMBER && v.num == 1);
    CHECK(kmpjs_ref_get(g, ns, "missing", 0, &v) == 0 && str_is(&v, "undefined"));
    kmpjs_ref_release(g, ns);
    /* unknown names and broken sources surface when the importer runs */
    CHECK(kmpjs_eval_module(g, S("import x from 'missing';"), "c", 0, &v) != 0 && str_has(&v, "ReferenceError") && str_has(&v, "module 'missing' is not registered"));
    CHECK(kmpjs_eval_module(g, S("import './util.js';"), "d", 0, &v) != 0 && str_has(&v, "'./util.js' is not registered"));
    CHECK(kmpjs_eval_module(g, S("import 'broken';"), "e", 0, &v) != 0 && str_has(&v, "SyntaxError"));
    CHECK(kmpjs_eval_module(g, S("export const = ;"), "f", 0, &v) != 0 && str_has(&v, "SyntaxError"));
    CHECK(kmpjs_eval_module(g, S("throw new Error('in body');"), "g", 0, &v) != 0 && str_is(&v, "Error: in body"));
    /* top-level await: settled within the call, or pending as a promise ref */
    CHECK(kmpjs_eval_module(g, S("const x = await Promise.resolve(41); export const y = x + 1;"), "h", 0, &v) == 0 && v.tag == KMPJS_TAG_REF);
    ns = v.ref;
    CHECK(kmpjs_ref_get(g, ns, "y", 0, &v) == 0 && v.num == 42);
    kmpjs_ref_release(g, ns);
    CHECK(kmpjs_eval_module(g, S("await new Promise(r => { globalThis.__go = r; }); export const done = true;"), "i", 0, &v) == 0 && v.tag == KMPJS_TAG_REF && ((int)v.num & KMPJS_REF_PROMISE));
    kmpjs_ref_release(g, v.ref);
    v = eval("__go(); 1", 0); CHECK(v.num == 1);
    /* host functions and cyclic imports work inside modules */
    CHECK(kmpjs_register_module(g, "ring-a", S("import { b } from 'ring-b'; export const a = 'a'; export const viaB = () => b;"), &out) == 0);
    CHECK(kmpjs_register_module(g, "ring-b", S("import { a } from 'ring-a'; export const b = 'b'; export const viaA = () => a;"), &out) == 0);
    CHECK(kmpjs_eval_module(g, S("import { viaB } from 'ring-a'; import { viaA } from 'ring-b'; export const r = viaB() + viaA() + report(3);"), "j", 0, &v) == 0);
    ns = v.ref;
    CHECK(kmpjs_ref_get(g, ns, "r", 0, &v) == 0 && str_is(&v, "ba3"));
    kmpjs_ref_release(g, ns);
    /* evaluated modules are importable under their name; registered and evaluated names never collide */
    CHECK(kmpjs_eval_module(g, S("export const page = 'p';"), "page", 0, &v) == 0);
    kmpjs_ref_release(g, v.ref);
    CHECK(kmpjs_eval_module(g, S("import { page } from 'page'; export default page + '!';"), "<module>", 0, &v) == 0);
    ns = v.ref;
    CHECK(kmpjs_ref_get(g, ns, "default", 0, &v) == 0 && str_is(&v, "p!"));
    kmpjs_ref_release(g, ns);
    CHECK(kmpjs_eval_module(g, S("export const again = 1;"), "<module>", 0, &v) == 0);
    kmpjs_ref_release(g, v.ref);
    CHECK(kmpjs_register_module(g, "page", S("export const page = 'shadow';"), &out) != 0 && str_has(&out, "already evaluated"));
    CHECK(kmpjs_eval_module(g, S("export const page = 'twice';"), "page", 0, &v) != 0 && str_has(&v, "already evaluated"));
    CHECK(kmpjs_eval_module(g, S("export const c = 1;"), "counter", 0, &v) != 0 && str_has(&v, "already registered"));
    CHECK(kmpjs_eval_module(g, S("export const = ;"), "never", 0, &v) != 0);
    CHECK(kmpjs_register_module(g, "never", S("export const ok = 1;"), &out) == 0); /* a failed compile spends no name */
    kmpjs_get_stats(g, &st); CHECK(st.live_refs == 0);
}

static void test_bytecode(void)
{
    static const char script[] = "var runs = (typeof runs === 'number' ? runs : 0) + 1; BigInt(report(runs) * 10) + 2n ** 64n % 7n";
    static const char module[] = "import { bump } from 'counter'; export const url = import.meta.url; export default bump();";
    kmpjs_value bc, mbc, v, out;
    int64_t ns;
    uint8_t *tampered;

    CHECK(kmpjs_compile(S(script), "prog.js", 0, &bc) == 0 && bc.str_len > KMPJS_BYTECODE_HEADER_SIZE);
    CHECK(kmpjs_compile(S("var x = ;"), "bad.js", 0, &v) != 0 && str_has(&v, "SyntaxError") && v.stack && str_has(&(kmpjs_value){.str = v.stack, .str_len = v.stack_len}, "bad.js"));
    kmpjs_free((void *)v.str);
    kmpjs_free((void *)v.stack);
    /* a script runs any number of times and sees the global state */
    CHECK(kmpjs_run_bytecode(g, (const uint8_t *)bc.str, bc.str_len, 0, &v) == 0 && v.tag == KMPJS_TAG_BIGINT && str_is(&v, "12"));
    CHECK(kmpjs_run_bytecode(g, (const uint8_t *)bc.str, bc.str_len, 0, &v) == 0 && str_is(&v, "22"));
    /* stripping shrinks the output; errors keep or lose their location accordingly */
    CHECK(kmpjs_compile(S("function boom() { throw new Error('x'); } boom()"), "loc.js", 0, &v) == 0);
    CHECK(kmpjs_compile(S("function boom() { throw new Error('x'); } boom()"), "loc.js", KMPJS_COMPILE_STRIP_DEBUG, &out) == 0 && out.str_len < v.str_len);
    CHECK(kmpjs_run_bytecode(g, (const uint8_t *)v.str, v.str_len, 0, &mbc) != 0 && str_is(&mbc, "Error: x") && mbc.stack && str_has(&(kmpjs_value){.str = mbc.stack, .str_len = mbc.stack_len}, "loc.js:1"));
    CHECK(kmpjs_run_bytecode(g, (const uint8_t *)out.str, out.str_len, 0, &mbc) != 0 && str_is(&mbc, "Error: x") && !(mbc.stack && str_has(&(kmpjs_value){.str = mbc.stack, .str_len = mbc.stack_len}, "loc.js:1")));
    kmpjs_free((void *)v.str);
    kmpjs_free((void *)out.str);
    /* modules: registered from bytecode under their compiled name, or run directly */
    CHECK(kmpjs_compile(S(module), "bc-page", KMPJS_COMPILE_MODULE, &mbc) == 0);
    CHECK(kmpjs_register_module_bytecode(g, (const uint8_t *)bc.str, bc.str_len, &out) != 0 && str_has(&out, "not a module"));
    CHECK(kmpjs_register_module_bytecode(g, (const uint8_t *)mbc.str, mbc.str_len, &out) == 0 && out.tag == KMPJS_TAG_STRING && str_is(&out, "bc-page"));
    CHECK(kmpjs_register_module_bytecode(g, (const uint8_t *)mbc.str, mbc.str_len, &out) != 0 && str_has(&out, "already registered"));
    /* a rejected registration must not shadow a source module that has not been imported yet */
    CHECK(kmpjs_register_module(g, "lazy", S("export const from = 'source';"), &out) == 0);
    CHECK(kmpjs_compile(S("export const from = 'bytecode';"), "lazy", KMPJS_COMPILE_MODULE, &out) == 0);
    CHECK(kmpjs_register_module_bytecode(g, (const uint8_t *)out.str, out.str_len, &v) != 0 && str_has(&v, "already registered"));
    CHECK(kmpjs_run_bytecode(g, (const uint8_t *)out.str, out.str_len, 0, &v) != 0 && str_has(&v, "already registered"));
    kmpjs_free((void *)out.str);
    CHECK(kmpjs_eval_module(g, S("import { from } from 'lazy'; export default from;"), "<module>", 0, &v) == 0);
    ns = v.ref;
    CHECK(kmpjs_ref_get(g, ns, "default", 0, &v) == 0 && str_is(&v, "source"));
    kmpjs_ref_release(g, ns);
    CHECK(kmpjs_compile(S("export const anon = 1;"), "<module>", KMPJS_COMPILE_MODULE, &out) == 0);
    CHECK(kmpjs_register_module_bytecode(g, (const uint8_t *)out.str, out.str_len, &v) != 0 && str_has(&v, "anonymous"));
    kmpjs_free((void *)out.str);
    CHECK(kmpjs_eval_module(g, S("import page, { url } from 'bc-page'; export const r = page + ':' + url;"), "<module>", 0, &v) == 0);
    ns = v.ref;
    CHECK(kmpjs_ref_get(g, ns, "r", 0, &v) == 0 && str_has(&v, ":test:bc-page"));
    kmpjs_ref_release(g, ns);
    kmpjs_free((void *)mbc.str);
    CHECK(kmpjs_compile(S("import { bump } from 'counter'; export const n = bump();"), "bc-direct", KMPJS_COMPILE_MODULE, &mbc) == 0);
    CHECK(kmpjs_run_bytecode(g, (const uint8_t *)mbc.str, mbc.str_len, 0, &v) == 0 && v.tag == KMPJS_TAG_REF);
    ns = v.ref;
    CHECK(kmpjs_ref_get(g, ns, "n", 0, &v) == 0 && v.tag == KMPJS_TAG_NUMBER);
    kmpjs_ref_release(g, ns);
    CHECK(kmpjs_run_bytecode(g, (const uint8_t *)mbc.str, mbc.str_len, 0, &v) != 0 && str_has(&v, "already evaluated"));
    CHECK(kmpjs_compile(S("import 'nowhere';"), "bc-missing", KMPJS_COMPILE_MODULE, &out) == 0);
    CHECK(kmpjs_run_bytecode(g, (const uint8_t *)out.str, out.str_len, 0, &v) != 0 && str_has(&v, "'nowhere' is not registered"));
    kmpjs_free((void *)out.str);
    kmpjs_free((void *)mbc.str);
    /* the header binds the bytes to this engine build */
    CHECK(kmpjs_run_bytecode(g, (const uint8_t *)"garbage", 7, 0, &v) != 0 && str_has(&v, "not QuickJS bytecode"));
    tampered = malloc((size_t)bc.str_len);
    memcpy(tampered, bc.str, (size_t)bc.str_len);
    memcpy(tampered + 12, "0000000000", 10);
    CHECK(kmpjs_run_bytecode(g, tampered, bc.str_len, 0, &v) != 0 && str_has(&v, "built for engine 0000"));
    memcpy(tampered, bc.str, (size_t)bc.str_len);
    tampered[8] = 1; /* claims to be a module */
    CHECK(kmpjs_run_bytecode(g, tampered, bc.str_len, 0, &v) != 0 && str_has(&v, "does not match its header"));
    free(tampered);
    kmpjs_free((void *)bc.str);
}

static void test_values(void)
{
    kmpjs_value v;
    v = eval("1 + 2", 0); CHECK(v.tag == KMPJS_TAG_NUMBER && v.num == 3);
    v = eval("'h\xc3\xa9llo'", 0); CHECK(v.tag == KMPJS_TAG_STRING && str_is(&v, "h\xc3\xa9llo"));
    v = eval("1 < 2", 0); CHECK(v.tag == KMPJS_TAG_BOOL && v.num == 1);
    v = eval("null", 0); CHECK(v.tag == KMPJS_TAG_NULL);
    v = eval("undefined", 0); CHECK(v.tag == KMPJS_TAG_UNDEFINED);
    v = eval("({a: 1, b: [1, 2, 'x']})", 0); CHECK(v.tag == KMPJS_TAG_OBJECT && str_is(&v, "{\"a\":1,\"b\":[1,2,\"x\"]}"));
    v = eval("(function () {})", 0); CHECK(v.tag == KMPJS_TAG_OBJECT && v.str == NULL);
    v = eval("var counter = 41;", 0); CHECK(v.tag == KMPJS_TAG_UNDEFINED);
    v = eval("counter + 1", 0); CHECK(v.tag == KMPJS_TAG_NUMBER && v.num == 42);
    v = eval("'\\uD800'", 0); CHECK(v.tag == KMPJS_TAG_STRING && v.str_len == 3);
}

static void test_exceptions(void)
{
    kmpjs_value v;
    v = eval("null.x", 0); CHECK(v.tag == KMPJS_TAG_EXCEPTION && str_is(&v, "TypeError: cannot read property 'x' of null") && v.stack && str_has(&(kmpjs_value){.str = v.stack, .str_len = v.stack_len}, "<test>"));
    v = eval("var x = ;", 0); CHECK(v.tag == KMPJS_TAG_EXCEPTION && str_has(&v, "SyntaxError"));
    v = eval("throw 42", 0); CHECK(v.tag == KMPJS_TAG_EXCEPTION && str_is(&v, "42"));
    v = eval("(function () { var a = []; for (var i = 0; i < 1000000; i++) a.push({i: i}); })()", 0); CHECK(v.tag == KMPJS_TAG_EXCEPTION && str_has(&v, "out of memory"));
    v = eval("1", 0); CHECK(v.tag == KMPJS_TAG_NUMBER && v.num == 1);
}

static void test_host_functions(void)
{
    kmpjs_value v, out;
    CHECK(kmpjs_define_function(g, "record", 0, 0, &out) == 0);
    CHECK(kmpjs_define_function(g, "boom", 1, 0, &out) == 0);
    CHECK(kmpjs_define_function(g, "obj", 2, 0, &out) == 0);
    CHECK(kmpjs_define_function(g, "stop", 3, 0, &out) == 0);
    CHECK(kmpjs_define_function(g, "keep", 4, KMPJS_FLAG_REF_OBJECTS, &out) == 0);
    CHECK(kmpjs_define_function(g, "give", 5, 0, &out) == 0);
    CHECK(kmpjs_define_function(g, "nested", 6, 0, &out) == 0);
    CHECK(kmpjs_define_function(g, "report", 7, 0, &out) == 0);
    CHECK(kmpjs_define_function(g, "tooMany", KMPJS_MAX_FN_ID + 1, 0, &out) != 0 && str_has(&out, "too many"));
    CHECK(kmpjs_define_function(g, "last", KMPJS_MAX_FN_ID, 0, &out) == 0);
    v = eval("typeof tooMany + ':' + typeof last", 0); CHECK(str_is(&v, "undefined:function"));
    v = eval("record(1, 'two', true, null, {k: [3]})", 0); CHECK(v.tag == KMPJS_TAG_STRING && str_is(&v, "ok:5"));
    v = eval("try { boom() } catch (e) { 'caught: ' + e.message }", 0); CHECK(str_is(&v, "caught: host failed"));
    v = eval("boom()", 0); CHECK(v.tag == KMPJS_TAG_EXCEPTION && str_is(&v, "Error: host failed"));
    v = eval("obj().n + 1", 0); CHECK(v.tag == KMPJS_TAG_NUMBER && v.num == 8);
    v = eval("console.log('a', 1, {x: true}); print('b'); 5", 0); CHECK(v.num == 5 && strcmp(last_log, "b") == 0);
    v = eval("stop(); for (;;) {}", 0); CHECK(v.tag == KMPJS_TAG_EXCEPTION && str_has(&v, "interrupted"));
    v = eval("var nestedCounter = 0; nested() + nested()", 0); CHECK(v.tag == KMPJS_TAG_NUMBER && v.num == 3);
    kmpjs_interrupt(g); /* idle: must be a no-op */
    v = eval("2", 0); CHECK(v.tag == KMPJS_TAG_NUMBER && v.num == 2);
}

static void test_refs(void)
{
    kmpjs_value v, out, args[2], sv;
    kmpjs_stats st;
    int64_t obj, arr, f, g2, held[50];
    int i;

    v = eval("var target = {n: 7, s: 'x', arr: [1, 2, 3], f: function (a, b) { 'use strict'; return a + b + this.n; }}; target", KMPJS_FLAG_REF_OBJECTS);
    CHECK(v.tag == KMPJS_TAG_REF && (int)v.num == 0); obj = v.ref;
    CHECK(kmpjs_ref_get(g, obj, "n", 0, &v) == 0 && v.num == 7);
    CHECK(kmpjs_ref_get(g, obj, "arr", 0, &v) == 0 && str_is(&v, "[1,2,3]"));
    CHECK(kmpjs_ref_get(g, obj, "arr", KMPJS_FLAG_REF_OBJECTS, &v) == 0 && v.tag == KMPJS_TAG_REF && ((int)v.num & KMPJS_REF_ARRAY)); arr = v.ref;
    CHECK(kmpjs_ref_get_index(g, arr, 1, 0, &v) == 0 && v.num == 2);
    CHECK(kmpjs_ref_get_index(g, arr, 10, 0, &v) == 0 && v.tag == KMPJS_TAG_UNDEFINED);
    CHECK(kmpjs_ref_get_index(g, arr, -1, 0, &v) != 0);
    CHECK(kmpjs_ref_get(g, obj, "f", KMPJS_FLAG_REF_OBJECTS, &v) == 0 && ((int)v.num & KMPJS_REF_FUNCTION)); f = v.ref;
    memset(args, 0, sizeof args);
    args[0].tag = KMPJS_TAG_NUMBER; args[0].num = 10; args[1].tag = KMPJS_TAG_NUMBER; args[1].num = 5;
    CHECK(kmpjs_ref_call(g, f, obj, args, 2, 0, &v) == 0 && v.num == 22);
    CHECK(kmpjs_ref_call(g, f, 0, args, 2, 0, &v) != 0 && str_has(&v, "TypeError"));
    CHECK(kmpjs_ref_call(g, obj, 0, NULL, 0, 0, &v) != 0 && str_has(&v, "not a function"));
    set_str(&sv, KMPJS_TAG_STRING, "hello");
    CHECK(kmpjs_ref_set(g, obj, "s", &sv, &out) == 0);
    kmpjs_free((void *)sv.str);
    set_str(&sv, KMPJS_TAG_OBJECT, "{\"z\":[true]}");
    CHECK(kmpjs_ref_set(g, obj, "j", &sv, &out) == 0);
    kmpjs_free((void *)sv.str);
    memset(&sv, 0, sizeof sv); sv.tag = KMPJS_TAG_REF; sv.ref = arr;
    CHECK(kmpjs_ref_set(g, obj, "arr2", &sv, &out) == 0);
    CHECK(kmpjs_ref_to_json(g, obj, &v) == 0 && str_is(&v, "{\"n\":7,\"s\":\"hello\",\"arr\":[1,2,3],\"j\":{\"z\":[true]},\"arr2\":[1,2,3]}"));
    CHECK(kmpjs_ref_to_json(g, f, &v) == 0 && v.str == NULL);
    v = eval("target.arr2 === target.arr", 0); CHECK(v.tag == KMPJS_TAG_BOOL && v.num == 1);
    v = eval("(function (x) { return x.length * 10; })", KMPJS_FLAG_REF_OBJECTS); g2 = v.ref;
    CHECK(kmpjs_ref_call(g, g2, 0, &sv, 1, 0, &v) == 0 && v.num == 30);

    /* transient refs into a host function, retained and given back */
    v = eval("keep({k: 'kept'}, [1], 'str')", 0); CHECK(v.tag == KMPJS_TAG_UNDEFINED);
    kmpjs_get_stats(g, &st); CHECK(st.live_refs == 5); /* obj, arr, f, g2 + retained */
    kmpjs_ref_retain(g, retained_ref);
    kmpjs_get_stats(g, &st); CHECK(st.live_refs == 6);
    kmpjs_ref_release(g, retained_ref);
    CHECK(kmpjs_ref_get(g, retained_ref, "k", 0, &v) == 0 && str_is(&v, "kept"));
    v = eval("give().k + '!'", 0); CHECK(str_is(&v, "kept!"));
    kmpjs_ref_release(g, retained_ref);
    CHECK(kmpjs_ref_get(g, retained_ref, "k", 0, &v) != 0 && str_has(&v, "released"));

    /* generation: a stale handle never aliases a reused slot */
    v = eval("({a: 1})", KMPJS_FLAG_REF_OBJECTS); { int64_t r1 = v.ref; kmpjs_ref_release(g, r1);
        v = eval("({b: 2})", KMPJS_FLAG_REF_OBJECTS); { int64_t r2 = v.ref;
            CHECK((r1 & 0xFFFFFFFF) == (r2 & 0xFFFFFFFF) && (r1 >> 32) != (r2 >> 32));
            CHECK(kmpjs_ref_get(g, r1, "b", 0, &v) != 0);
            kmpjs_ref_release(g, r1);
            CHECK(kmpjs_ref_get(g, r2, "b", 0, &v) == 0 && v.num == 2);
            kmpjs_ref_release(g, r2); } }

    /* refs survive GC, churn returns slots */
    for (i = 0; i < 50; i++) { char code[32]; snprintf(code, sizeof code, "({v: %d})", i); v = eval(code, KMPJS_FLAG_REF_OBJECTS); held[i] = v.ref; }
    v = eval("var junk = []; for (var i = 0; i < 2000; i++) junk.push({i: i}); junk = null; 1", 0);
    for (i = 0; i < 50; i++) { CHECK(kmpjs_ref_get(g, held[i], "v", 0, &v) == 0 && v.num == i); kmpjs_ref_release(g, held[i]); }
    kmpjs_get_stats(g, &st);
    CHECK(st.live_refs == 4); /* obj, arr, f, g2 */
    for (i = 0; i < 20000; i++) { v = eval("({i: 1})", KMPJS_FLAG_REF_OBJECTS); CHECK(v.tag == KMPJS_TAG_REF); kmpjs_ref_release(g, v.ref); }
    kmpjs_get_stats(g, &st);
    CHECK(st.live_refs == 4 && st.ref_slots < 100);

    /* interrupt reaches accessors */
    v = eval("({get y() { stop(); for (;;) {} }})", KMPJS_FLAG_REF_OBJECTS);
    CHECK(kmpjs_ref_get(g, v.ref, "y", 0, &out) != 0 && str_has(&out, "interrupted"));
    kmpjs_ref_release(g, v.ref);

    kmpjs_ref_release(g, obj); kmpjs_ref_release(g, arr); kmpjs_ref_release(g, f); kmpjs_ref_release(g, g2);
    kmpjs_ref_release(g, 9999); kmpjs_ref_release(g, obj);
    kmpjs_get_stats(g, &st);
    CHECK(st.live_refs == 0);
    kmpjs_dump_memory(g, &v); CHECK(v.tag == KMPJS_TAG_STRING && v.str_len > 0);
    CHECK(st.memory_used > 0 && st.memory_limit == 4 * 1024 * 1024 && st.object_count > 0 && st.string_count > 0 && st.atom_count > 0 && st.function_count > 0);
    v = eval("var big = new Array(10000).fill(0).map((_, i) => ({i})); 1", 0);
    { kmpjs_stats after; kmpjs_get_stats(g, &after); CHECK(after.memory_used > st.memory_used && after.object_count > st.object_count + 9000); }
    v = eval("big = null; 1", 0);
}

int main(void)
{
    kmpjs_value v;
    kmpjs_config cfg = { "test", 4 * 1024 * 1024, 256 * 1024, 0 };
    kmpjs_config tiny = { NULL, 100, 0, 0 };
    g = kmpjs_create(&cfg, NULL, host, logger, rejection);
    CHECK(g != NULL);
    CHECK(kmpjs_create(&tiny, NULL, host, logger, rejection) == NULL);
    test_values();
    test_exceptions();
    test_host_functions();
    test_refs();
    test_promises();
    test_bigint_and_binary();
    test_modules();
    test_bytecode();
    /* destroy with refs still open must be clean */
    v = eval("({leak: 1})", KMPJS_FLAG_REF_OBJECTS); CHECK(v.tag == KMPJS_TAG_REF);
    kmpjs_destroy(g);
    printf("shim_test: %d failure(s)\n", failures);
    return failures != 0;
}
