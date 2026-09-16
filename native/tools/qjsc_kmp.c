/* Host command line compiler: the same kmpjs_compile that JsBytecode.compile wraps, for build pipelines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs_kmp.h"

static int usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-m] [-n NAME] [--strip-source | --strip-debug] [-o OUT] INPUT\n"
            "  -m             compile as an ES module (registered/imported under NAME)\n"
            "  -n NAME        script or module name recorded in the bytecode (default: INPUT)\n"
            "  --strip-source drop the source text, keep line numbers\n"
            "  --strip-debug  drop all debug information\n"
            "  -o OUT         output file (default: INPUT with .bin appended)\n",
            argv0);
    return 2;
}

static char *read_file(const char *path, long *plen)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long len;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)len + 1);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    *plen = len;
    return buf;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL, *name = NULL;
    int flags = 0, i;
    char *code, *out_default = NULL;
    long len;
    kmpjs_value result;
    FILE *f;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0) {
            flags |= KMPJS_COMPILE_MODULE;
        } else if (strcmp(argv[i], "--strip-source") == 0) {
            flags |= KMPJS_COMPILE_STRIP_SOURCE;
        } else if (strcmp(argv[i], "--strip-debug") == 0) {
            flags |= KMPJS_COMPILE_STRIP_DEBUG;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (argv[i][0] == '-' || input) {
            return usage(argv[0]);
        } else {
            input = argv[i];
        }
    }
    if (!input)
        return usage(argv[0]);
    if (!name)
        name = input;
    if (!output) {
        out_default = malloc(strlen(input) + 5);
        if (!out_default)
            return 1;
        sprintf(out_default, "%s.bin", input);
        output = out_default;
    }
    code = read_file(input, &len);
    if (!code) {
        fprintf(stderr, "%s: cannot read %s\n", argv[0], input);
        return 1;
    }
    if (kmpjs_compile(code, (int32_t)len, name, flags, &result) != 0) {
        fprintf(stderr, "%.*s\n", result.str_len, result.str ? result.str : "compilation failed");
        if (result.stack)
            fprintf(stderr, "%.*s", result.stack_len, result.stack);
        kmpjs_free((void *)result.str);
        kmpjs_free((void *)result.stack);
        free(code);
        free(out_default);
        return 1;
    }
    f = fopen(output, "wb");
    if (!f || fwrite(result.str, 1, (size_t)result.str_len, f) != (size_t)result.str_len || fclose(f)) {
        fprintf(stderr, "%s: cannot write %s\n", argv[0], output);
        kmpjs_free((void *)result.str);
        free(code);
        free(out_default);
        return 1;
    }
    kmpjs_free((void *)result.str);
    free(code);
    free(out_default);
    return 0;
}
