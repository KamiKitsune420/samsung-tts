/* Host functions for translated code: the C library the original called, on Windows. Each takes its arguments
 * from the arm64 argument registers. The guest is an LP64 Android program: long is 64 bits, wchar_t is 32 bits
 * and UTF-8 is the only multibyte encoding, whatever Windows thinks. The engine runs on one thread. */
#include "a2c_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <windows.h>

#define X(n) (c->x[n])
#define RET(val) (c->x[0] = (uint64_t)(val))
#define RETF(val) do { c->v[0] = mk_f(val); } while (0)
#define RETD(val) do { c->v[0] = mk_d(val); } while (0)
#define D(n) (c->v[n].d[0])

int a2c_trace_files;        /* log every file the guest opens */

/* ---- allocation
 *
 * The guest's memory comes from a heap of its own, not the host program's: the host may be someone else's
 * process (a screen reader), and what the guest does to its heap should stay there. A header in front of every
 * block holds the raw pointer and the size, and a check word: a pointer that was not allocated here, or was
 * already freed, is not passed on to the heap, where it would end the process. */
typedef struct { uintptr_t check; void *raw; size_t size; } blk_t;
#define BLK_CHECK(p) ((uintptr_t)(p) ^ 0x5a17c0de5a17c0deULL)
static HANDLE g_heap;
static size_t g_live_blocks, g_live_bytes, g_mapped_bytes, g_bad_frees;

/* What the guest holds now: blocks and bytes from its allocator, and bytes it has mapped. For finding leaks. */
void a2c_heap_stats(size_t *blocks, size_t *bytes, size_t *mapped) { *blocks = g_live_blocks; *bytes = g_live_bytes; *mapped = g_mapped_bytes; }
/* 1 if the guest's heap is internally consistent: a slow, complete check, for deciding what can be done after a fault. */
int a2c_heap_ok(void) { return !g_heap || HeapValidate(g_heap, 0, NULL); }

static void *g_alloc(size_t size, size_t align) {
    if (align < 16) align = 16;
    if (!g_heap && !(g_heap = HeapCreate(0, 0, 0))) return NULL;
    uint8_t *raw = HeapAlloc(g_heap, 0, size + align + sizeof(blk_t));
    if (!raw) return NULL;
    uintptr_t p = ((uintptr_t)raw + sizeof(blk_t) + align - 1) & ~(uintptr_t)(align - 1);
    blk_t *h = (blk_t *)p - 1;
    h->check = BLK_CHECK(p); h->raw = raw; h->size = size;
    g_live_blocks++; g_live_bytes += size;
    return (void *)p;
}
static void g_free(void *p) {
    if (!p) return;
    blk_t *h = (blk_t *)p - 1;
    if (h->check != BLK_CHECK(p)) { g_bad_frees++; return; }
    h->check = 0;
    g_live_blocks--; g_live_bytes -= h->size;
    HeapFree(g_heap, 0, h->raw);
}
static void *g_realloc(void *p, size_t size) {
    if (!p) return g_alloc(size, 16);
    blk_t *h = (blk_t *)p - 1;
    if (h->check != BLK_CHECK(p)) { a2c_trap(NULL, 0, "realloc of a pointer that is not an allocated block"); return NULL; }
    size_t old = h->size;
    void *q = g_alloc(size, 16);
    if (q) { memcpy(q, p, old < size ? old : size); g_free(p); }
    return q;
}
void *a2c_malloc(size_t n) { return g_alloc(n, 16); }
void a2c_free(void *p) { g_free(p); }

void H_malloc(cpu_t *c) { RET(g_alloc(X(0), 16)); }
void H_calloc(cpu_t *c) { size_t n = X(0) * X(1); void *p = g_alloc(n, 16); if (p) memset(p, 0, n); RET(p); }
void H_realloc(cpu_t *c) { RET(g_realloc((void *)X(0), X(1))); }
void H_free(cpu_t *c) { g_free((void *)X(0)); }
void H_memalign(cpu_t *c) { RET(g_alloc(X(1), X(0))); }
void H_posix_memalign(cpu_t *c) { void *p = g_alloc(X(2), X(1)); *(void **)X(0) = p; RET(p ? 0 : 12); }
void H__Znwm(cpu_t *c) { RET(g_alloc(X(0) ? X(0) : 1, 16)); }
void H__Znam(cpu_t *c) { RET(g_alloc(X(0) ? X(0) : 1, 16)); }
void H__ZdlPv(cpu_t *c) { g_free((void *)X(0)); }
void H__ZdaPv(cpu_t *c) { g_free((void *)X(0)); }

/* ---- memory and strings
 *
 * The *_chk functions are the C library's fortified ones: the compiler passes the size of the destination where
 * it knows it ((size_t)-1 where it does not), and the library ends the program rather than write or read past
 * it. Here that is a trap, which costs the utterance (engine/sstts.c) but keeps the overflow from happening. */
#define FORTIFY(ok, what) do { if (!(ok)) { a2c_trap(c, 0, "fortify: " what " past the end of a buffer"); return; } } while (0)
void H_memset(cpu_t *c) { RET(memset((void *)X(0), (int)X(1), X(2))); }
void H_memcpy(cpu_t *c) { RET(memmove((void *)X(0), (void *)X(1), X(2))); }
void H_memmove(cpu_t *c) { RET(memmove((void *)X(0), (void *)X(1), X(2))); }
void H___memcpy_chk(cpu_t *c) { FORTIFY(X(2) <= X(3), "memcpy"); RET(memmove((void *)X(0), (void *)X(1), X(2))); }
void H___memset_chk(cpu_t *c) { FORTIFY(X(2) <= X(3), "memset"); RET(memset((void *)X(0), (int)X(1), X(2))); }
void H___memmove_chk(cpu_t *c) { FORTIFY(X(2) <= X(3), "memmove"); RET(memmove((void *)X(0), (void *)X(1), X(2))); }
void H_memcmp(cpu_t *c) { RET((int64_t)memcmp((void *)X(0), (void *)X(1), X(2))); }
void H_memchr(cpu_t *c) { RET(memchr((void *)X(0), (int)X(1), X(2))); }
void H_strlen(cpu_t *c) { RET(strlen((char *)X(0))); }
void H___strlen_chk(cpu_t *c) { size_t n = strnlen((char *)X(0), X(1)); FORTIFY(n < X(1), "strlen"); RET(n); }
void H_strcmp(cpu_t *c) { RET((int64_t)strcmp((char *)X(0), (char *)X(1))); }
void H_strncmp(cpu_t *c) { RET((int64_t)strncmp((char *)X(0), (char *)X(1), X(2))); }
void H_strncasecmp(cpu_t *c) { RET((int64_t)_strnicmp((char *)X(0), (char *)X(1), X(2))); }
void H_strcpy(cpu_t *c) { RET(strcpy((char *)X(0), (char *)X(1))); }
void H___strcpy_chk(cpu_t *c) { FORTIFY(strnlen((char *)X(1), X(2)) < X(2), "strcpy"); RET(strcpy((char *)X(0), (char *)X(1))); }
void H_strncpy(cpu_t *c) { RET(strncpy((char *)X(0), (char *)X(1), X(2))); }
void H___strncpy_chk(cpu_t *c) { FORTIFY(X(2) <= X(3), "strncpy"); RET(strncpy((char *)X(0), (char *)X(1), X(2))); }
void H___strncpy_chk2(cpu_t *c) { FORTIFY(X(2) <= X(3), "strncpy"); RET(strncpy((char *)X(0), (char *)X(1), X(2))); }
void H_strcat(cpu_t *c) { RET(strcat((char *)X(0), (char *)X(1))); }
void H___strcat_chk(cpu_t *c) {
    size_t have = strnlen((char *)X(0), X(2));
    FORTIFY(have < X(2) && strnlen((char *)X(1), X(2) - have) < X(2) - have, "strcat");
    RET(strcat((char *)X(0), (char *)X(1)));
}
void H___strncat_chk(cpu_t *c) {
    size_t have = strnlen((char *)X(0), X(3));
    FORTIFY(have < X(3) && strnlen((char *)X(1), X(2)) < X(3) - have, "strncat");
    RET(strncat((char *)X(0), (char *)X(1), X(2)));
}
void H_strchr(cpu_t *c) { RET(strchr((char *)X(0), (int)X(1))); }
void H___strchr_chk(cpu_t *c) {
    const char *p = (const char *)X(0); char ch = (char)X(1);
    for (size_t left = X(2);; p++, left--) {
        FORTIFY(left != 0, "strchr");
        if (*p == ch) { RET(p); return; }
        if (!*p) { RET(0); return; }
    }
}
void H_strrchr(cpu_t *c) { RET(strrchr((char *)X(0), (int)X(1))); }
void H_strstr(cpu_t *c) { RET(strstr((char *)X(0), (char *)X(1))); }
void H_strspn(cpu_t *c) { RET(strspn((char *)X(0), (char *)X(1))); }
void H_strpbrk(cpu_t *c) { RET(strpbrk((char *)X(0), (char *)X(1))); }
void H_strtok(cpu_t *c) { RET(strtok((char *)X(0), (char *)X(1))); }
void H_strnlen(cpu_t *c) { RET(strnlen((char *)X(0), X(1))); }

/* ---- numbers. The C locale throughout; long is 64 bits. */
void H_atoi(cpu_t *c) { RET((int64_t)atoi((char *)X(0))); }
void H_atof(cpu_t *c) { RETD(atof((char *)X(0))); }
void H_strtod(cpu_t *c) { RETD(strtod((char *)X(0), (char **)X(1))); }
void H_strtof(cpu_t *c) { RETF(strtof((char *)X(0), (char **)X(1))); }
void H_strtol(cpu_t *c) { RET(strtoll((char *)X(0), (char **)X(1), (int)X(2))); }
void H_strtoll(cpu_t *c) { RET(strtoll((char *)X(0), (char **)X(1), (int)X(2))); }
void H_strtoul(cpu_t *c) { RET(strtoull((char *)X(0), (char **)X(1), (int)X(2))); }
void H_strtoull(cpu_t *c) { RET(strtoull((char *)X(0), (char **)X(1), (int)X(2))); }
void H_strtoll_l(cpu_t *c) { RET(strtoll((char *)X(0), (char **)X(1), (int)X(2))); }
void H_strtoull_l(cpu_t *c) { RET(strtoull((char *)X(0), (char **)X(1), (int)X(2))); }
/* long double is IEEE binary128 on arm64, returned in v0 */
void H_strtold_l(cpu_t *c) {
    double d = strtod((char *)X(0), (char **)X(1));
    uint64_t u; memcpy(&u, &d, 8);
    uint64_t sign = u >> 63, frac = u & 0xfffffffffffffULL; int e = (int)((u >> 52) & 0x7ff);
    uint64_t hi, lo;
    if (e == 0 && frac == 0) { hi = sign << 63; lo = 0; }
    else {
        if (e == 0x7ff) e = 0x7fff;
        else {
            if (e == 0) { e = 1; while (!(frac >> 52)) { frac <<= 1; e--; } frac &= 0xfffffffffffffULL; }
            e = e - 1023 + 16383;
        }
        hi = (sign << 63) | ((uint64_t)e << 48) | (frac >> 4); lo = frac << 60;
    }
    c->v[0].q[0] = lo; c->v[0].q[1] = hi;
}

/* bsearch calls back into the guest for each comparison */
void H_bsearch(cpu_t *c) {
    uint64_t key = X(0), base = X(1), n = X(2), size = X(3), cmp = X(4);
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2, elem = base + mid * size;
        c->x[0] = key; c->x[1] = elem;
        a2c_call(c, cmp);
        int r = (int)c->x[0];
        if (r == 0) { RET(elem); return; }
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    RET(0);
}

/* ---- variadic arguments: from the registers and stack of a variadic call, or from an arm64 va_list
 * {void *stack; void *gr_top; void *vr_top; int gr_offs; int vr_offs;} */
typedef struct { cpu_t *c; int gr, vr; uint64_t stack; uint8_t *va; } args_t;
static args_t args_regs(cpu_t *c, int named) { args_t a = { c, named, 0, c->sp, NULL }; return a; }
static args_t args_va(cpu_t *c, uint64_t va) { args_t a = { c, 0, 0, 0, (uint8_t *)va }; return a; }
static uint64_t arg_int(args_t *a) {
    if (a->va) {
        int32_t offs; memcpy(&offs, a->va + 24, 4);
        uint64_t p;
        if (offs < 0) { memcpy(&p, a->va + 8, 8); p += (int64_t)offs; offs += 8; memcpy(a->va + 24, &offs, 4); }
        else { memcpy(&p, a->va, 8); uint64_t n = p + 8; memcpy(a->va, &n, 8); }
        return ld64(p);
    }
    if (a->gr < 8) return a->c->x[a->gr++];
    uint64_t v = ld64(a->stack); a->stack += 8; return v;
}
static double arg_dbl(args_t *a) {
    uint64_t p;
    if (a->va) {
        int32_t offs; memcpy(&offs, a->va + 28, 4);
        if (offs < 0) { memcpy(&p, a->va + 16, 8); p += (int64_t)offs; offs += 16; memcpy(a->va + 28, &offs, 4); }
        else { memcpy(&p, a->va, 8); uint64_t n = p + 8; memcpy(a->va, &n, 8); }
    } else {
        if (a->vr < 8) return a->c->v[a->vr++].d[0];
        p = a->stack; a->stack += 8;
    }
    double d; memcpy(&d, (void *)p, 8); return d;
}

/* printf: each conversion is formatted by the host with its own little format string */
static size_t format(char *out, size_t cap, const char *fmt, args_t *a) {
    size_t len = 0;
    if (a2c_trace_files > 1) fprintf(stderr, "a2c: format cap %llu \"%.60s\"\n", (unsigned long long)cap, fmt);
#define PUT(ch) do { char ch_ = (ch); if (len + 1 < cap) out[len] = ch_; len++; } while (0)
    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt++); continue; }
        const char *start = fmt++;
        if (*fmt == '%') { PUT('%'); fmt++; continue; }
        char spec[64]; int n = 0; spec[n++] = '%';
        while (*fmt && strchr("-+ #0'", *fmt)) { if (n < 40) spec[n++] = *fmt; fmt++; }
        if (*fmt == '*') { n += snprintf(spec + n, 16, "%d", (int)arg_int(a)); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') { if (n < 48) spec[n++] = *fmt; fmt++; }
        if (*fmt == '.') {
            spec[n++] = '.'; fmt++;
            if (*fmt == '*') { n += snprintf(spec + n, 16, "%d", (int)arg_int(a)); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') { if (n < 56) spec[n++] = *fmt; fmt++; }
        }
        int size = 4;       /* bytes of the integer argument */
        for (;; fmt++) {
            if (*fmt == 'h') size = size == 2 ? 1 : 2;
            else if (*fmt == 'l' || *fmt == 'j' || *fmt == 'z' || *fmt == 't' || *fmt == 'q') size = 8;
            else if (*fmt == 'L') size = 16;
            else break;
        }
        char conv = *fmt ? *fmt++ : 0;
        char piece[512]; const char *text = piece; int plen;
        char *big = NULL;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v = (int64_t)arg_int(a);
            if (size == 4) v = (int32_t)v; else if (size == 2) v = (int16_t)v; else if (size == 1) v = (int8_t)v;
            spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = 'd'; spec[n] = 0;
            plen = snprintf(piece, sizeof piece, spec, (long long)v); break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = arg_int(a);
            if (size == 4) v = (uint32_t)v; else if (size == 2) v = (uint16_t)v; else if (size == 1) v = (uint8_t)v;
            spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = conv; spec[n] = 0;
            plen = snprintf(piece, sizeof piece, spec, (unsigned long long)v); break;
        }
        case 'c': spec[n++] = 'c'; spec[n] = 0; plen = snprintf(piece, sizeof piece, spec, (int)arg_int(a)); break;
        case 'p': plen = snprintf(piece, sizeof piece, "0x%llx", (unsigned long long)arg_int(a)); break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double v;
            if (size == 16) { arg_int(a); arg_int(a); v = 0; }     /* long double: not supported, consume it */
            else v = arg_dbl(a);
            spec[n++] = conv; spec[n] = 0;
            plen = snprintf(piece, sizeof piece, spec, v); break;
        }
        case 's': {
            const char *s = (const char *)arg_int(a);
            if (!s) s = "(null)";
            spec[n++] = 's'; spec[n] = 0;
            plen = snprintf(NULL, 0, spec, s);
            if (plen >= (int)sizeof piece) { big = malloc((size_t)plen + 1); snprintf(big, (size_t)plen + 1, spec, s); text = big; }
            else snprintf(piece, sizeof piece, spec, s);
            break;
        }
        case 'n': { uint64_t p = arg_int(a); if (p) st32(p, (uint32_t)len); plen = 0; break; }
        default:
            plen = (int)(fmt - start);
            memcpy(piece, start, (size_t)(plen < 500 ? plen : 500));
        }
        for (int i = 0; i < plen; i++) PUT(text[i]);
        free(big);
    }
    if (cap) out[len < cap ? len : cap - 1] = 0;
    return len;
#undef PUT
}

static char *format_alloc(const char *fmt, args_t *a, size_t *n) {
    args_t save = *a; uint8_t va_copy_[32];
    if (a->va) memcpy(va_copy_, a->va, 32);
    size_t len = format(NULL, 0, fmt, a);
    *a = save; if (a->va) memcpy(a->va, va_copy_, 32);
    char *buf = malloc(len + 1);
    format(buf, len + 1, fmt, a);
    if (n) *n = len;
    return buf;
}

/* ---- FILE: host FILE pointers pass through the guest untouched; stdout and stderr are imported data */
uint64_t a2c_sF, a2c_stdout_slot, a2c_stderr_slot;      /* set by the loader when the guest imports them */
static FILE *host_file(uint64_t p) {
    if (a2c_sF && p >= a2c_sF && p < a2c_sF + 3 * 152) {
        int k = (int)((p - a2c_sF) / 152);
        return k == 0 ? stdin : k == 1 ? stdout : stderr;
    }
    if (p == 1) return stdout;
    if (p == 2) return stderr;
    return (FILE *)p;
}

static void fix_mode(const char *mode, char *out) {     /* always binary; glibc/bionic's 'e' is not known here */
    int n = 0, b = 0;
    for (; *mode && n < 6; mode++) { if (*mode == 'e') continue; if (*mode == 'b') b = 1; out[n++] = *mode; }
    if (!b) out[n++] = 'b';
    out[n] = 0;
}

/* A path from the guest is UTF-8, as everything of the guest's is; Windows wants UTF-16. (Text that is not valid
 * UTF-8 is taken to be in the system's code page, for a path typed on a command line.) */
#define PATH_MAX_W 1024
static const wchar_t *wide_path(const char *path, wchar_t *out) {
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, PATH_MAX_W) &&
        !MultiByteToWideChar(CP_ACP, 0, path, -1, out, PATH_MAX_W)) out[0] = 0;
    return out;
}

void H_fopen(cpu_t *c) {
    char mode[8]; fix_mode((const char *)X(1), mode);
    wchar_t wpath[PATH_MAX_W], wmode[8];
    for (int i = 0; i < 8; i++) wmode[i] = (wchar_t)mode[i];
    FILE *f = _wfopen(wide_path((const char *)X(0), wpath), wmode);
    if (a2c_trace_files) fprintf(stderr, "a2c: fopen %s %s -> %s\n", (const char *)X(0), mode, f ? "ok" : "FAIL");
    RET(f);
}
void H_fdopen(cpu_t *c) { char mode[8]; fix_mode((const char *)X(1), mode); RET(_fdopen((int)X(0), mode)); }
void H_fclose(cpu_t *c) { RET((int64_t)fclose(host_file(X(0)))); }
void H_fread(cpu_t *c) { RET(fread((void *)X(0), X(1), X(2), host_file(X(3)))); }
void H___fread_chk(cpu_t *c) { FORTIFY(X(1) * X(2) <= X(4), "fread"); RET(fread((void *)X(0), X(1), X(2), host_file(X(3)))); }
void H_fwrite(cpu_t *c) { RET(fwrite((void *)X(0), X(1), X(2), host_file(X(3)))); }
void H___fwrite_chk(cpu_t *c) { FORTIFY(X(1) * X(2) <= X(4), "fwrite"); RET(fwrite((void *)X(0), X(1), X(2), host_file(X(3)))); }
void H_fseek(cpu_t *c) { RET((int64_t)_fseeki64(host_file(X(0)), (int64_t)X(1), (int)X(2))); }
void H_fseeko(cpu_t *c) { RET((int64_t)_fseeki64(host_file(X(0)), (int64_t)X(1), (int)X(2))); }
void H_ftell(cpu_t *c) { RET(_ftelli64(host_file(X(0)))); }
void H_ftello(cpu_t *c) { RET(_ftelli64(host_file(X(0)))); }
void H_fflush(cpu_t *c) { RET((int64_t)fflush(X(0) ? host_file(X(0)) : NULL)); }
void H_feof(cpu_t *c) { RET((int64_t)feof(host_file(X(0)))); }
void H_ferror(cpu_t *c) { RET((int64_t)ferror(host_file(X(0)))); }
void H_fgetc(cpu_t *c) { RET((int64_t)fgetc(host_file(X(0)))); }
void H_getc(cpu_t *c) { RET((int64_t)fgetc(host_file(X(0)))); }
void H_ungetc(cpu_t *c) { RET((int64_t)ungetc((int)X(0), host_file(X(1)))); }
void H_fputc(cpu_t *c) { RET((int64_t)fputc((int)X(0), host_file(X(1)))); }
void H_fputs(cpu_t *c) { RET((int64_t)fputs((const char *)X(0), host_file(X(1)))); }

/* the engine's own diagnostics on stdout ("spk_rate = 14.0") are dropped unless tracing */
void H_printf(cpu_t *c) { args_t a = args_regs(c, 1); size_t n; char *s = format_alloc((const char *)X(0), &a, &n); if (a2c_trace_files) fwrite(s, 1, n, stdout); free(s); RET(n); }
void H_fprintf(cpu_t *c) { FILE *f = host_file(X(0)); args_t a = args_regs(c, 2); size_t n; char *s = format_alloc((const char *)X(1), &a, &n); fwrite(s, 1, n, f); free(s); RET(n); }
void H_vfprintf(cpu_t *c) { FILE *f = host_file(X(0)); args_t a = args_va(c, X(2)); size_t n; char *s = format_alloc((const char *)X(1), &a, &n); fwrite(s, 1, n, f); free(s); RET(n); }
void H_snprintf(cpu_t *c) { args_t a = args_regs(c, 3); RET(format((char *)X(0), X(1), (const char *)X(2), &a)); }
void H_vsnprintf(cpu_t *c) { args_t a = args_va(c, X(3)); RET(format((char *)X(0), X(1), (const char *)X(2), &a)); }
void H___vsnprintf_chk(cpu_t *c) { FORTIFY(X(1) <= X(3), "vsnprintf"); args_t a = args_va(c, X(5)); RET(format((char *)X(0), X(1), (const char *)X(4), &a)); }
void H_vsprintf(cpu_t *c) { args_t a = args_va(c, X(2)); RET(format((char *)X(0), (size_t)1 << 30, (const char *)X(1), &a)); }
void H___vsprintf_chk(cpu_t *c) {
    size_t room = X(2) < ((size_t)1 << 30) ? X(2) : (size_t)1 << 30;
    args_t a = args_va(c, X(4));
    size_t n = format((char *)X(0), room, (const char *)X(3), &a);
    FORTIFY(n < room, "vsprintf");
    RET(n);
}
void H_vasprintf(cpu_t *c) {
    args_t a = args_va(c, X(2)); size_t n; char *s = format_alloc((const char *)X(1), &a, &n);
    char *g = g_alloc(n + 1, 16); memcpy(g, s, n + 1); free(s);
    *(char **)X(0) = g; RET(n);
}
void H___android_log_print(cpu_t *c) {
    args_t a = args_regs(c, 3); char *s = format_alloc((const char *)X(2), &a, NULL);
    if (a2c_trace_files) fprintf(stderr, "a2c: engine log [%s] %s\n", (const char *)X(1), s);   /* quiet unless tracing */
    free(s); RET(0);
}
void H_syslog(cpu_t *c) { args_t a = args_regs(c, 2); char *s = format_alloc((const char *)X(1), &a, NULL); fprintf(stderr, "a2c: syslog %s\n", s); free(s); }
void H_openlog(cpu_t *c) { (void)c; }
void H_closelog(cpu_t *c) { (void)c; }

/* scanf: the host does the work on a format in which every integer conversion written for a 64-bit long reads a
 * long long; the destinations are fetched as up to sixteen pointers */
static void scan(cpu_t *c, const char *str, const char *fmt, args_t *a) {
    char f2[1024]; int n = 0; int nargs = 0;
    for (const char *p = fmt; *p && n < 1000; p++) {
        f2[n++] = *p;
        if (*p != '%') continue;
        p++;
        if (*p == '%') { f2[n++] = '%'; continue; }
        int star = 0;
        if (*p == '*') { star = 1; f2[n++] = *p++; }
        while (*p >= '0' && *p <= '9') f2[n++] = *p++;
        int l = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') { if (*p == 'l' || *p == 'z' || *p == 'j' || *p == 't') l++; else f2[n++] = *p; p++; }
        if (strchr("diouxX", *p) && l) { f2[n++] = 'l'; f2[n++] = 'l'; }
        else if (strchr("eEfFgGaA", *p) && l) f2[n++] = 'l';
        else if (*p == 'n' && l) { f2[n++] = 'l'; f2[n++] = 'l'; }
        if (*p == '[') { while (*p && *p != ']') f2[n++] = *p++; }
        if (!*p) break;
        f2[n++] = *p;
        if (!star) nargs++;
    }
    f2[n] = 0;
    void *p[16] = {0};
    for (int i = 0; i < nargs && i < 16; i++) p[i] = (void *)arg_int(a);
    RET((int64_t)sscanf(str, f2, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]));
}
void H_sscanf(cpu_t *c) { args_t a = args_regs(c, 2); scan(c, (const char *)X(0), (const char *)X(1), &a); }
void H_vsscanf(cpu_t *c) { args_t a = args_va(c, X(2)); scan(c, (const char *)X(0), (const char *)X(1), &a); }

/* ---- file descriptors, stat, mmap, directories */
static int g_errno;
void H___errno(cpu_t *c) { RET(&g_errno); }

void H_open(cpu_t *c) {
    const char *path = (const char *)X(0); int fl = (int)X(1);
    int w = _O_BINARY | (fl & 3);       /* O_RDONLY, O_WRONLY, O_RDWR agree */
    if (fl & 0100) w |= _O_CREAT;
    if (fl & 01000) w |= _O_TRUNC;
    if (fl & 02000) w |= _O_APPEND;
    wchar_t wpath[PATH_MAX_W];
    int fd = _wopen(wide_path(path, wpath), w, 0644);
    if (fd < 0) g_errno = 2;
    if (a2c_trace_files) fprintf(stderr, "a2c: open %s -> %d\n", path, fd);
    RET((int64_t)fd);
}
void H___open_2(cpu_t *c) { H_open(c); }
void H_close(cpu_t *c) { RET((int64_t)_close((int)X(0))); }
void H_dup(cpu_t *c) { RET((int64_t)_dup((int)X(0))); }
void H_read(cpu_t *c) { RET((int64_t)_read((int)X(0), (void *)X(1), (unsigned)X(2))); }
void H_lseek(cpu_t *c) { RET(_lseeki64((int)X(0), (int64_t)X(1), (int)X(2))); }
void H_access(cpu_t *c) { wchar_t w[PATH_MAX_W]; int r = _waccess(wide_path((const char *)X(0), w), 0); if (r) g_errno = 2; RET((int64_t)r); }
void H_unlink(cpu_t *c) { wchar_t w[PATH_MAX_W]; RET((int64_t)_wunlink(wide_path((const char *)X(0), w))); }

/* struct stat on arm64 Linux: st_mode at 16, st_size at 48, 128 bytes */
static void fill_stat(uint64_t out, const struct _stat64 *s) {
    memset((void *)out, 0, 128);
    st32(out + 16, (uint32_t)((s->st_mode & _S_IFDIR) ? 0040755 : 0100644));
    st32(out + 20, 1);
    st64(out + 48, (uint64_t)s->st_size);
    st32(out + 56, 4096);
    st64(out + 64, (uint64_t)(s->st_size + 511) / 512);
    st64(out + 72, (uint64_t)s->st_atime); st64(out + 88, (uint64_t)s->st_mtime); st64(out + 104, (uint64_t)s->st_ctime);
}
void H_fstat(cpu_t *c) { struct _stat64 s; int r = _fstat64((int)X(0), &s); if (!r) fill_stat(X(1), &s); RET((int64_t)r); }
void H_stat(cpu_t *c) {
    struct _stat64 s; wchar_t w[PATH_MAX_W];
    int r = _wstat64(wide_path((const char *)X(0), w), &s);
    if (!r) fill_stat(X(1), &s); else g_errno = 2;
    RET((int64_t)r);
}
void H_lstat(cpu_t *c) { H_stat(c); }

/* mmap of a file is a private copy of it; nothing the engine maps is written back */
#define MAX_MAPS 256
static struct { uint8_t *p; uint64_t len; } g_maps[MAX_MAPS];
void H_mmap(cpu_t *c) {
    uint64_t len = X(1); int fd = (int)X(4); int64_t off = (int64_t)X(5); int flags = (int)X(3);
    uint8_t *p = VirtualAlloc(NULL, len ? len : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) { RET(-1); return; }
    for (int i = 0; i < MAX_MAPS; i++)
        if (!g_maps[i].p) { g_maps[i].p = p; g_maps[i].len = len; g_mapped_bytes += len; break; }
    if (!(flags & 0x20) && fd >= 0) {
        int64_t at = _lseeki64(fd, 0, SEEK_CUR);
        _lseeki64(fd, off, SEEK_SET);
        uint64_t done = 0;
        while (done < len) {
            unsigned want = (unsigned)(len - done > (1u << 30) ? (1u << 30) : len - done);
            int got = _read(fd, p + done, want);
            if (got <= 0) break;
            done += (uint64_t)got;
        }
        _lseeki64(fd, at, SEEK_SET);
    }
    RET(p);
}
void H_munmap(cpu_t *c) {
    for (int i = 0; i < MAX_MAPS; i++)
        if (g_maps[i].p == (uint8_t *)X(0)) { g_mapped_bytes -= g_maps[i].len; g_maps[i].p = NULL; break; }
    VirtualFree((void *)X(0), 0, MEM_RELEASE);
    RET(0);
}

/* DIR and struct dirent64 {ino 8, off 8, reclen 2, type 1, name[256]} */
typedef struct { HANDLE h; WIN32_FIND_DATAW fd; int first; uint8_t ent[280]; } dir_t;
void H_opendir(cpu_t *c) {
    char pat[1024]; snprintf(pat, sizeof pat, "%s/*", (const char *)X(0));
    wchar_t wpat[PATH_MAX_W];
    dir_t *d = calloc(1, sizeof *d);
    d->h = FindFirstFileW(wide_path(pat, wpat), &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); g_errno = 2; RET(0); return; }
    d->first = 1; RET(d);
}
void H_readdir(cpu_t *c) {
    dir_t *d = (dir_t *)X(0);
    if (!d->first && !FindNextFileW(d->h, &d->fd)) { RET(0); return; }
    d->first = 0;
    memset(d->ent, 0, sizeof d->ent);
    d->ent[0] = 1; d->ent[16] = 24; d->ent[17] = 1;
    d->ent[18] = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
    WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, (char *)d->ent + 19, 255, NULL, NULL);
    RET(d->ent);
}
void H_closedir(cpu_t *c) { dir_t *d = (dir_t *)X(0); FindClose(d->h); free(d); RET(0); }

/* ---- locale, character classes, multibyte: bionic's "C.UTF-8" */
void H_newlocale(cpu_t *c) { static int loc; RET(&loc); }
void H_freelocale(cpu_t *c) { (void)c; }
void H_uselocale(cpu_t *c) { static int loc; RET(&loc); }
void H_setlocale(cpu_t *c) { RET("C"); }
void H___ctype_get_mb_cur_max(cpu_t *c) { RET(4); }
void H_localeconv(cpu_t *c) {
    static uint64_t lc[14]; static char dot[] = ".", empty[] = "";
    lc[0] = (uint64_t)dot;
    for (int i = 1; i < 10; i++) lc[i] = (uint64_t)empty;
    memset(&lc[10], 0x7f, 32);
    RET(lc);
}
void H_isdigit_l(cpu_t *c) { int ch = (int)X(0); RET(ch >= '0' && ch <= '9'); }
void H_isxdigit_l(cpu_t *c) { int ch = (int)X(0); RET((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')); }
void H_islower_l(cpu_t *c) { int ch = (int)X(0); RET(ch >= 'a' && ch <= 'z'); }
void H_isupper_l(cpu_t *c) { int ch = (int)X(0); RET(ch >= 'A' && ch <= 'Z'); }
void H_iswlower_l(cpu_t *c) { uint32_t ch = (uint32_t)X(0); RET(ch >= 'a' && ch <= 'z'); }
void H_iswupper_l(cpu_t *c) { uint32_t ch = (uint32_t)X(0); RET(ch >= 'A' && ch <= 'Z'); }
void H_tolower_l(cpu_t *c) { int ch = (int)X(0); RET((int64_t)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch)); }
void H_toupper_l(cpu_t *c) { int ch = (int)X(0); RET((int64_t)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch)); }

/* one UTF-8 character: bytes used, 0 for NUL, -1 invalid, -2 incomplete */
static int64_t u8_decode(const uint8_t *s, uint64_t n, uint32_t *out) {
    if (n == 0) return -2;
    uint32_t b = s[0];
    if (b < 0x80) { *out = b; return b ? 1 : 0; }
    int need = b >= 0xf0 && b < 0xf8 ? 4 : b >= 0xe0 ? 3 : b >= 0xc2 && b < 0xe0 ? 2 : 0;
    if (!need || b >= 0xf8) return -1;
    uint32_t v = b & (0xff >> (need + 1));
    for (int i = 1; i < need; i++) {
        if ((uint64_t)i >= n) return -2;
        if ((s[i] & 0xc0) != 0x80) return -1;
        v = (v << 6) | (s[i] & 0x3f);
    }
    *out = v; return need;
}
static int u8_encode(uint32_t wc, uint8_t *s) {
    if (wc < 0x80) { s[0] = (uint8_t)wc; return 1; }
    if (wc < 0x800) { s[0] = 0xc0 | (wc >> 6); s[1] = 0x80 | (wc & 0x3f); return 2; }
    if (wc < 0x10000) { s[0] = 0xe0 | (wc >> 12); s[1] = 0x80 | ((wc >> 6) & 0x3f); s[2] = 0x80 | (wc & 0x3f); return 3; }
    if (wc < 0x110000) { s[0] = 0xf0 | (wc >> 18); s[1] = 0x80 | ((wc >> 12) & 0x3f); s[2] = 0x80 | ((wc >> 6) & 0x3f); s[3] = 0x80 | (wc & 0x3f); return 4; }
    return -1;
}
void H_mbrtowc(cpu_t *c) {
    uint32_t wc = 0;
    if (!X(1)) { RET(0); return; }
    int64_t r = u8_decode((const uint8_t *)X(1), X(2), &wc);
    if (r >= 0 && X(0)) st32(X(0), wc);
    if (r == -1) g_errno = 84;
    RET(r);
}
void H_mbtowc(cpu_t *c) {
    uint32_t wc = 0;
    if (!X(1)) { RET(0); return; }
    int64_t r = u8_decode((const uint8_t *)X(1), X(2), &wc);
    if (r >= 0 && X(0)) st32(X(0), wc);
    RET(r < 0 ? -1 : r);
}
void H_mbrlen(cpu_t *c) { uint32_t wc; if (!X(0)) { RET(0); return; } int64_t r = u8_decode((const uint8_t *)X(0), X(1), &wc); RET(r); }
static void mbs_to_wcs(cpu_t *c, uint64_t dst, uint64_t psrc, uint64_t nms, uint64_t len) {
    const uint8_t *s = *(const uint8_t **)psrc; uint64_t done = 0, used = 0;
    while (!dst || done < len) {
        uint32_t wc = 0;
        int64_t r = u8_decode(s + used, nms - used, &wc);
        if (r == -2) break;
        if (r == -1) { g_errno = 84; if (dst) *(const uint8_t **)psrc = s + used; RET(-1); return; }
        if (dst) st32(dst + 4 * done, wc);
        if (r == 0) { if (dst) *(const uint8_t **)psrc = NULL; RET(done); return; }
        used += (uint64_t)r; done++;
    }
    if (dst) *(const uint8_t **)psrc = s + used;
    RET(done);
}
void H_mbsnrtowcs(cpu_t *c) { mbs_to_wcs(c, X(0), X(1), X(2), X(3)); }
void H_mbsrtowcs(cpu_t *c) { mbs_to_wcs(c, X(0), X(1), ~0ULL, X(2)); }
void H_wcrtomb(cpu_t *c) {
    uint8_t tmp[8];
    if (!X(0)) { RET(1); return; }
    int r = u8_encode((uint32_t)X(1), (uint8_t *)X(0));
    (void)tmp;
    if (r < 0) g_errno = 84;
    RET((int64_t)r);
}
void H_wcsnrtombs(cpu_t *c) {
    uint64_t dst = X(0), psrc = X(1), nwc = X(2), len = X(3);
    const uint8_t *s = *(const uint8_t **)psrc; uint64_t out = 0;
    for (uint64_t i = 0; i < nwc; i++) {
        uint32_t wc = ld32((uint64_t)s + 4 * i); uint8_t tmp[4];
        int r = u8_encode(wc, tmp);
        if (r < 0) { g_errno = 84; RET(-1); return; }
        if (dst) { if (out + (uint64_t)r > len) { *(const uint8_t **)psrc = s + 4 * i; RET(out); return; } memcpy((void *)(dst + out), tmp, (size_t)r); }
        if (wc == 0) { if (dst) *(const uint8_t **)psrc = NULL; RET(out); return; }
        out += (uint64_t)r;
    }
    if (dst) *(const uint8_t **)psrc = s + 4 * nwc;
    RET(out);
}
void H_wcslen(cpu_t *c) { uint64_t n = 0; while (ld32(X(0) + 4 * n)) n++; RET(n); }
void H_wmemcpy(cpu_t *c) { RET(memmove((void *)X(0), (void *)X(1), 4 * X(2))); }
void H_wmemmove(cpu_t *c) { RET(memmove((void *)X(0), (void *)X(1), 4 * X(2))); }
void H_wmemset(cpu_t *c) { for (uint64_t i = 0; i < X(2); i++) st32(X(0) + 4 * i, (uint32_t)X(1)); }
void H_wmemcmp(cpu_t *c) {
    for (uint64_t i = 0; i < X(2); i++) {
        int32_t a = (int32_t)ld32(X(0) + 4 * i), b = (int32_t)ld32(X(1) + 4 * i);
        if (a != b) { RET((int64_t)(a < b ? -1 : 1)); return; }
    }
    RET(0);
}
void H_wmemchr(cpu_t *c) { for (uint64_t i = 0; i < X(2); i++) if (ld32(X(0) + 4 * i) == (uint32_t)X(1)) { RET(X(0) + 4 * i); return; } RET(0); }

/* ---- time */
void H_clock_gettime(cpu_t *c) {
    LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    st64(X(1), (uint64_t)(t.QuadPart / f.QuadPart));
    st64(X(1) + 8, (uint64_t)((t.QuadPart % f.QuadPart) * 1000000000LL / f.QuadPart));
    RET(0);
}
void H_gettimeofday(cpu_t *c) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10 - 11644473600000000ULL;
    if (X(0)) { st64(X(0), t / 1000000); st64(X(0) + 8, t % 1000000); }
    RET(0);
}
void H_clock(cpu_t *c) { RET((int64_t)clock() * 1000); }
void H_usleep(cpu_t *c) { Sleep((DWORD)(X(0) / 1000)); RET(0); }
void H_nanosleep(cpu_t *c) { RET(0); }
void H_strftime_l(cpu_t *c) {
    struct tm t; memset(&t, 0, sizeof t); memcpy(&t, (void *)X(3), 36);
    RET(strftime((char *)X(0), X(1), (const char *)X(2), &t));
}
void H_strerror_r(cpu_t *c) { snprintf((char *)X(1), X(2), "error %d", (int)X(0)); RET(0); }

/* ---- threads: there is one */
void H_pthread_mutex_init(cpu_t *c) { RET(0); }
void H_pthread_mutex_destroy(cpu_t *c) { RET(0); }
void H_pthread_mutex_lock(cpu_t *c) { RET(0); }
void H_pthread_mutex_trylock(cpu_t *c) { RET(0); }
void H_pthread_mutex_unlock(cpu_t *c) { RET(0); }
void H_pthread_mutexattr_init(cpu_t *c) { RET(0); }
void H_pthread_mutexattr_destroy(cpu_t *c) { RET(0); }
void H_pthread_mutexattr_settype(cpu_t *c) { RET(0); }
void H_pthread_cond_init(cpu_t *c) { RET(0); }
void H_pthread_cond_destroy(cpu_t *c) { RET(0); }
void H_pthread_cond_signal(cpu_t *c) { RET(0); }
void H_pthread_cond_broadcast(cpu_t *c) { RET(0); }
void H_pthread_cond_wait(cpu_t *c) { a2c_trap(c, 0, "pthread_cond_wait with a single thread"); }
void H_pthread_cond_timedwait(cpu_t *c) { RET(110); }
void H_pthread_rwlock_rdlock(cpu_t *c) { RET(0); }
void H_pthread_rwlock_wrlock(cpu_t *c) { RET(0); }
void H_pthread_rwlock_unlock(cpu_t *c) { RET(0); }
void H_pthread_self(cpu_t *c) { RET(1); }
void H_pthread_equal(cpu_t *c) { RET(X(0) == X(1)); }
void H_pthread_create(cpu_t *c) { a2c_trap(c, 0, "pthread_create"); }
void H_pthread_join(cpu_t *c) { a2c_trap(c, 0, "pthread_join"); }
void H_pthread_exit(cpu_t *c) { a2c_trap(c, 0, "pthread_exit"); }
void H_pthread_detach(cpu_t *c) { RET(0); }
void H_sched_yield(cpu_t *c) { RET(0); }
void H_pthread_once(cpu_t *c) {
    uint64_t once = X(0), fn = X(1);
    if (!ld32(once)) { st32(once, 1); a2c_call(c, fn); }
    RET(0);
}
static uint64_t g_tsd[256]; static int g_ntsd = 1;
void H_pthread_key_create(cpu_t *c) {
    if (g_ntsd >= 256) { RET(11); return; }     /* EAGAIN, as the C library does when its keys run out */
    st32(X(0), (uint32_t)g_ntsd++); RET(0);
}
void H_pthread_key_delete(cpu_t *c) { RET(0); }
void H_pthread_getspecific(cpu_t *c) { RET(g_tsd[X(0) & 255]); }
void H_pthread_setspecific(cpu_t *c) { g_tsd[X(0) & 255] = X(1); RET(0); }

/* ---- process */
void H___stack_chk_fail(cpu_t *c) { a2c_trap(c, 0, "stack protector"); }
void H_abort(cpu_t *c) { a2c_trap(c, 0, "abort()"); }
void H_exit(cpu_t *c) { fflush(NULL); exit((int)X(0)); }
void H__exit(cpu_t *c) { fflush(NULL); _exit((int)X(0)); }
void H_android_set_abort_message(cpu_t *c) { fprintf(stderr, "a2c: abort message: %s\n", (const char *)X(0)); }
void H___cxa_allocate_exception(cpu_t *c) { RET(g_alloc(X(0) + 128, 16)); }
void H___cxa_throw(cpu_t *c) { a2c_trap(c, 0, "C++ exception thrown"); }
void H___cxa_begin_catch(cpu_t *c) { a2c_trap(c, 0, "__cxa_begin_catch"); }
void H___cxa_end_catch(cpu_t *c) { a2c_trap(c, 0, "__cxa_end_catch"); }
void H___cxa_atexit(cpu_t *c) { RET(0); }
void H___cxa_thread_atexit_impl(cpu_t *c) { RET(0); }
void H___cxa_finalize(cpu_t *c) { (void)c; }
void H_getauxval(cpu_t *c) { RET(0); }
void H_syscall(cpu_t *c) { RET(-1); }
void H_dl_iterate_phdr(cpu_t *c) { RET(0); }
void H_getenv(cpu_t *c) { RET(0); }
void H_getpid(cpu_t *c) { RET(1234); }
void H_sysconf(cpu_t *c) { RET(X(0) == 30 || X(0) == 39 || X(0) == 40 ? 4096 : 1); }
void H___system_property_get(cpu_t *c) { *(char *)X(1) = 0; RET(0); }
void H_isnanf(cpu_t *c) { float f = c->v[0].f[0]; RET(f != f); }
void H_ldexp(cpu_t *c) { RETD(ldexp(D(0), (int)X(0))); }

/* The engine asks the cpuinfo library one thing: whether the CPU has half-precision arithmetic, to choose
 * between float16 and float32 models. Leaving cpuinfo's tables empty answers no, which is the float32 path the
 * reference runs and the fast one on x86. */
void H_cpuinfo_initialize(cpu_t *c) { RET(1); }
