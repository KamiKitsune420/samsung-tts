/* Runtime for code translated by a2c.py: maps the original libraries' data at their fixed addresses, binds
 * indirect calls, and provides the arm64 operations declared in a2c_rt.h. Windows x64. */
#include "a2c_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

extern const a2c_func a2c_funcs[];
extern const int a2c_nfuncs;
extern const a2c_export a2c_exports[];
extern const char *const a2c_host_names[];
extern void (*const a2c_host_funcs[])(cpu_t *);

#define MAX_IMAGES 4
typedef struct {
    uint64_t base, span;
    const uint8_t *file;       /* the ELF file, kept in memory */
    const uint8_t *dynsym, *dynstr;
    uint64_t nsyms;
} image_t;
static image_t g_img[MAX_IMAGES + 1];
static char g_unresolved[4096][96];
static int g_nunresolved;
#define A2C_UNRES (A2C_HOST + 0x10000000ULL)
#define A2C_CALLBACK (A2C_HOST + 0x20000000ULL)
extern uint64_t a2c_sF;

/* host functions the guest is handed as function pointers (callbacks) */
static void (*g_callbacks[64])(cpu_t *);
static int g_ncallbacks;
uint64_t a2c_callback(void (*fn)(cpu_t *)) {
    g_callbacks[g_ncallbacks] = fn;
    return A2C_CALLBACK + ((uint64_t)g_ncallbacks++ << 4);
}

/* What a trap does: by default the program stops with a message. A host that would rather lose one call than
 * the process sets a2c_trap_raises and catches the structured exception A2C_TRAP_EXCEPTION around its calls
 * into translated code; a2c_last_trap then says what happened. */
int a2c_trap_raises;
char a2c_last_trap[200];
void a2c_trap(cpu_t *c, uint64_t addr, const char *why) {
    (void)c;
    snprintf(a2c_last_trap, sizeof a2c_last_trap, "trap at %llx: %s", (unsigned long long)addr, why);
    if (a2c_trap_raises) RaiseException(0xE0A2C001u, EXCEPTION_NONCONTINUABLE, 0, NULL);
    fprintf(stderr, "a2c: %s\n", a2c_last_trap);
    fflush(NULL);
    abort();
}

uint64_t a2c_mrs(cpu_t *c, const char *reg) {
    if (!strcmp(reg, "tpidr_el0")) return c->tls;
    if (!strcmp(reg, "fpcr")) return c->fpcr;
    if (!strcmp(reg, "fpsr")) return c->fpsr;
    a2c_trap(c, 0, reg);
    return 0;
}

void a2c_msr(cpu_t *c, const char *reg, uint64_t v) {
    if (!strcmp(reg, "fpcr")) c->fpcr = v;
    else if (!strcmp(reg, "fpsr")) c->fpsr = v;
    else a2c_trap(c, 0, reg);
}

void a2c_call(cpu_t *c, uint64_t addr) {
    if (addr >= A2C_HOST && addr < A2C_UNRES) {
        a2c_host_funcs[(addr - A2C_HOST) >> 4](c);
        return;
    }
    if (addr >= A2C_CALLBACK && addr < A2C_CALLBACK + 0x400) {
        g_callbacks[(addr - A2C_CALLBACK) >> 4](c);
        return;
    }
    if (addr >= A2C_UNRES && addr < A2C_UNRES + 0x10000000ULL) {
        fprintf(stderr, "a2c: call to unresolved import %s\n", g_unresolved[(addr - A2C_UNRES) >> 4]);
        abort();
    }
    int lo = 0, hi = a2c_nfuncs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a2c_funcs[mid].addr == addr) { a2c_funcs[mid].fn(c); return; }
        if (a2c_funcs[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    {
        char msg[96];
        int img = addr >= IMG2 ? 2 : 1;
        snprintf(msg, sizeof msg, "indirect call to an untranslated function (image %d, %llx)", img,
                 (unsigned long long)(addr - (img == 2 ? IMG2 : IMG1)));
        a2c_trap(c, addr, msg);
    }
}

void (*a2c_find(const char *name))(cpu_t *) {
    for (int i = 0; a2c_exports[i].name; i++)
        if (!strcmp(a2c_exports[i].name, name)) return a2c_exports[i].fn;
    return NULL;
}

/* The translated function that contains a host code address: its index in a2c_funcs, or -1 for host code.
 * The system's unwind table says where the function containing the address begins; it is a translated one if
 * that is where one of them begins. (A function too simple to have an unwind entry is found by the nearest
 * start below, within a few kilobytes.) */
static int translated_at(uint64_t host_address, uint64_t *start) {
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(host_address, &base, NULL);
    uint64_t begin = rf ? base + rf->BeginAddress : 0;
    int best = -1;
    for (int i = 0; i < a2c_nfuncs; i++) {
        uint64_t f = (uint64_t)a2c_funcs[i].fn;
        if (rf ? f == begin : (f <= host_address && host_address - f < 0x2000 && (best < 0 || f > (uint64_t)a2c_funcs[best].fn))) best = i;
        if (rf && best >= 0) break;
    }
    if (best >= 0 && start) *start = (uint64_t)a2c_funcs[best].fn;
    return best;
}

/* Where a host code address is, for reporting a fault: the translated function that contains it, by its address
 * in the original image, or else the module and the offset in it (for the linker map). */
void a2c_where(uint64_t host_address, char *out, size_t cap) {
    uint64_t start = 0;
    int i = translated_at(host_address, &start);
    if (i < 0) {
        HMODULE mod = NULL; char path[MAX_PATH] = "";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)host_address, &mod);
        if (mod) GetModuleFileNameA(mod, path, sizeof path);
        const char *file = strrchr(path, '\\');
        snprintf(out, cap, "host code, %s offset %llx", file ? file + 1 : path, (unsigned long long)(host_address - (uint64_t)mod));
        return;
    }
    const char *name = "";
    for (int k = 0; a2c_exports[k].name; k++) if (a2c_exports[k].fn == a2c_funcs[i].fn) { name = a2c_exports[k].name; break; }
    uint64_t addr = a2c_funcs[i].addr;
    snprintf(out, cap, "function %llx (image %d) +%llu %.60s", (unsigned long long)(addr >= IMG2 ? addr - IMG2 : addr - IMG1),
             addr >= IMG2 ? 2 : 1, (unsigned long long)(host_address - start), name);
}

/* The translated functions on the host stack at `context` (an exception's CONTEXT), innermost first, as their
 * addresses in the original images: the guest's call stack, which the guest's own registers no longer hold
 * because the translation keeps them in C locals. */
void a2c_backtrace(const void *context, char *out, size_t cap) {
    CONTEXT ctx = *(const CONTEXT *)context;
    size_t len = 0; uint64_t last = 0;
    if (cap) out[0] = 0;
    for (int depth = 0; depth < 4000 && ctx.Rip && len + 24 < cap; depth++) {
        int i = translated_at(ctx.Rip, NULL);
        if (i >= 0 && a2c_funcs[i].addr != last) {
            uint64_t addr = last = a2c_funcs[i].addr;
            len += (size_t)snprintf(out + len, cap - len, "%s%llx", len ? " < " : "", (unsigned long long)(addr >= IMG2 ? addr - IMG2 : addr - IMG1));
        }
        DWORD64 base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
        if (!rf) {      /* a leaf: the return address is on top of the stack */
            ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8;
            continue;
        }
        void *handler_data = NULL; DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &handler_data, &establisher, NULL);
    }
    if (!len && cap) snprintf(out, cap, "(no translated function)");
}

/* ---- loading */

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint64_t host_address(const char *name) {
    for (int i = 0; a2c_host_names[i]; i++)
        if (!strcmp(a2c_host_names[i], name)) return A2C_HOST + ((uint64_t)i << 4);
    return 0;
}

static uint64_t lookup_export(const image_t *im, const char *name) {
    for (uint64_t i = 1; i < im->nsyms; i++) {
        const uint8_t *s = im->dynsym + i * 24;
        if (rd16(s + 6) && !strcmp((const char *)im->dynstr + rd32(s), name)) return im->base + rd64(s + 8);
    }
    return 0;
}

static uint64_t resolve(int self, uint64_t symidx) {
    const image_t *im = &g_img[self];
    const uint8_t *s = im->dynsym + symidx * 24;
    const char *name = (const char *)im->dynstr + rd32(s);
    if (rd16(s + 6)) return im->base + rd64(s + 8);
    uint64_t a = host_address(name);
    if (a) return a;
    for (int k = 1; k <= MAX_IMAGES; k++)
        if (k != self && g_img[k].file && (a = lookup_export(&g_img[k], name)) != 0) return a;
    /* data imports: the C library's few public objects, else a zeroed block */
    if ((s[4] & 15) == 1) {
        if (!strcmp(name, "__sF")) { if (!a2c_sF) a2c_sF = (uint64_t)calloc(1, 4096); return a2c_sF; }
        if (!strcmp(name, "stdout") || !strcmp(name, "stderr")) {     /* FILE *: 1 and 2 mean the host's streams */
            uint64_t *p = calloc(1, 64); *p = name[3] == 'o' ? 1 : 2; return (uint64_t)p;
        }
        if (!strcmp(name, "_ctype_")) {     /* const char *_ctype_, indexed by c + 1: bionic's class bits */
            static uint8_t tab[258]; static const uint8_t *ptr = tab;
            for (int ch = 0; ch < 128; ch++) {
                uint8_t f = 0;
                if (ch >= 'A' && ch <= 'Z') f |= 1 | ((ch <= 'F') ? 0x40 : 0);
                if (ch >= 'a' && ch <= 'z') f |= 2 | ((ch <= 'f') ? 0x40 : 0);
                if (ch >= '0' && ch <= '9') f |= 4;
                if (ch == ' ' || (ch >= 9 && ch <= 13)) f |= 8;
                if (ch > 32 && ch < 127 && !(f & 7)) f |= 0x10;
                if (ch < 32 || ch == 127) f |= 0x20;
                if (ch == ' ') f |= 0x80;
                tab[ch + 1] = f;
            }
            return (uint64_t)&ptr;
        }
        return (uint64_t)calloc(1, 4096);
    }
    for (int i = 0; i < g_nunresolved; i++)
        if (!strcmp(g_unresolved[i], name)) return A2C_UNRES + ((uint64_t)i << 4);
    if (g_nunresolved >= 4096) { fprintf(stderr, "a2c: too many unresolved imports\n"); abort(); }
    strncpy(g_unresolved[g_nunresolved], name, 95);
    return A2C_UNRES + ((uint64_t)g_nunresolved++ << 4);
}

/* Map an image at its fixed address. `d` is the ELF file in memory, which must stay there: the translation
 * step puts the files it translated into the program (gen_images.c), so the code and the data it works on
 * cannot be two different builds, and nothing has to be found on disk. */
int a2c_load(int index, const uint8_t *d, size_t n, uint64_t base) {
    if (n < 0x40 || memcmp(d, "\177ELF", 4)) { fprintf(stderr, "a2c: image %d is not an ELF file\n", index); return -1; }
    image_t *im = &g_img[index];
    uint64_t phoff = rd64(d + 0x20); int phes = rd16(d + 0x36), phn = rd16(d + 0x38);
    uint64_t span = 0;
    for (int i = 0; i < phn; i++) {
        const uint8_t *p = d + phoff + (uint64_t)i * phes;
        if (rd32(p) == 1 && rd64(p + 0x10) + rd64(p + 0x28) > span) span = rd64(p + 0x10) + rd64(p + 0x28);
    }
    span = (span + 0xffff) & ~0xffffULL;
    if (!VirtualAlloc((void *)base, span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "a2c: cannot map image %d at %llx (error %lu)\n", index, (unsigned long long)base, GetLastError());
        return -1;
    }
    for (int i = 0; i < phn; i++) {
        const uint8_t *p = d + phoff + (uint64_t)i * phes;
        if (rd32(p) == 1) memcpy((void *)(base + rd64(p + 0x10)), d + rd64(p + 8), rd64(p + 0x20));
    }
    im->base = base; im->span = span; im->file = d;

    /* sections by name */
    uint64_t shoff = rd64(d + 0x28); int shes = rd16(d + 0x3a), shn = rd16(d + 0x3c), shx = rd16(d + 0x3e);
    const uint8_t *strtab = d + rd64(d + shoff + (uint64_t)shx * shes + 0x18);
    const uint8_t *rela[2] = {0, 0}; uint64_t relan[2] = {0, 0};
    const uint8_t *relr = 0; uint64_t relrn = 0;
    for (int i = 0; i < shn; i++) {
        const uint8_t *s = d + shoff + (uint64_t)i * shes;
        const char *name = (const char *)strtab + rd32(s);
        const uint8_t *data = d + rd64(s + 0x18); uint64_t size = rd64(s + 0x20);
        if (!strcmp(name, ".dynsym")) { im->dynsym = data; im->nsyms = size / 24; }
        else if (!strcmp(name, ".dynstr")) im->dynstr = data;
        else if (!strcmp(name, ".rela.dyn")) { rela[0] = data; relan[0] = size / 24; }
        else if (!strcmp(name, ".rela.plt")) { rela[1] = data; relan[1] = size / 24; }
        else if (!strcmp(name, ".relr.dyn")) { relr = data; relrn = size / 8; }
    }
    long unknown = 0;
    for (int k = 0; k < 2; k++)
        for (uint64_t i = 0; i < relan[k]; i++) {
            const uint8_t *r = rela[k] + i * 24;
            uint64_t off = rd64(r), info = rd64(r + 8), add = rd64(r + 16);
            uint64_t *where = (uint64_t *)(base + off);
            switch ((uint32_t)info) {
            case 1027: *where = base + add; break;                          /* RELATIVE */
            case 257: *where = resolve(index, info >> 32) + add; break;     /* ABS64 */
            case 1025: case 1026: *where = resolve(index, info >> 32); break; /* GLOB_DAT, JUMP_SLOT */
            default: unknown++;
            }
        }
    {   /* RELR: an address, then bitmaps of the 63 words that follow it */
        uint64_t *where = 0;
        for (uint64_t i = 0; i < relrn; i++) {
            uint64_t e = rd64(relr + i * 8);
            if (!(e & 1)) { where = (uint64_t *)(base + e); *where++ += base; }
            else { for (int b = 0; b < 63; b++) if (e & (2ULL << b)) where[b] += base; where += 63; }
        }
    }
    if (unknown) fprintf(stderr, "a2c: image %d has %ld relocations of unhandled types\n", index, unknown);
    return 0;
}

static int recip_estimate(int a);
cpu_t *a2c_cpu_new(void) {
    cpu_t *c = _aligned_malloc(sizeof *c, 64);
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 256; i++) a2c_recip_tab[i] = recip_estimate(256 + i) & 0xff;
    /* The guest's stack, with a megabyte that cannot be touched on either side: guest code that runs off its
     * stack (too deep a recursion below, a runaway copy above) then faults at once instead of writing over
     * whatever lies next to it. */
    size_t stack = 16u << 20, fence = 1u << 20;
    uint8_t *s = VirtualAlloc(NULL, stack + 2 * fence, MEM_RESERVE, PAGE_NOACCESS);
    if (!s || !VirtualAlloc(s + fence, stack, MEM_COMMIT, PAGE_READWRITE)) { fprintf(stderr, "a2c: no memory for the stack\n"); abort(); }
    c->sp = (uint64_t)(s + fence + stack - 4096);
    uint64_t *tls = calloc(64, 8);
    tls[5] = 0x2b992ddfa23249d6ULL;     /* the stack protector's value, at tpidr_el0 + 0x28 */
    c->tls = (uint64_t)tls;
    return c;
}

/* ---- arm64 estimates and half precision */

static int recip_estimate(int a) {      /* a in 256..511, result in 256..511 */
    a = a * 2 + 1;
    int b = (1 << 19) / a;
    return (b + 1) / 2;
}

/* the low eight bits of recip_estimate(256 + i), filled in by a2c_cpu_new */
int32_t a2c_recip_tab[256];

float frecpe32(float x) {
    uint32_t u; memcpy(&u, &x, 4);
    uint32_t sign = u & 0x80000000u, exp = (u >> 23) & 0xff, frac = u & 0x7fffff;
    if (exp == 0xff) {
        if (frac) { u |= 0x00400000u; float r; memcpy(&r, &u, 4); return r; }
        float r; memcpy(&r, &sign, 4); return r;
    }
    if (exp == 0 && frac == 0) { uint32_t r = sign | 0x7f800000u; float o; memcpy(&o, &r, 4); return o; }
    if (exp == 0 && frac < 0x200000) { uint32_t r = sign | 0x7f800000u; float o; memcpy(&o, &r, 4); return o; }  /* |x| < 2^-128 overflows */
    uint64_t f52 = (uint64_t)frac << 29;
    int e = (int)exp;
    if (e == 0) {
        if (!(f52 >> 51 & 1)) { e = -1; f52 = (f52 << 2) & 0xfffffffffffffULL; }
        else f52 = (f52 << 1) & 0xfffffffffffffULL;
    }
    int scaled = 0x100 | (int)(f52 >> 44);
    int rexp = 253 - e;
    int est = recip_estimate(scaled);
    uint64_t rf = (uint64_t)(est & 0xff) << 44;
    if (rexp == 0) rf = (1ULL << 51) | (rf >> 1);
    else if (rexp == -1) { rf = (1ULL << 50) | (rf >> 2); rexp = 0; }
    uint32_t r = sign | ((uint32_t)(rexp & 0xff) << 23) | (uint32_t)(rf >> 29);
    float o; memcpy(&o, &r, 4); return o;
}

double frecpe64(double x) {
    uint64_t u; memcpy(&u, &x, 8);
    uint64_t sign = u & 0x8000000000000000ULL, frac = u & 0xfffffffffffffULL;
    int exp = (int)((u >> 52) & 0x7ff);
    if (exp == 0x7ff) {
        if (frac) { u |= 0x0008000000000000ULL; double r; memcpy(&r, &u, 8); return r; }
        double r; memcpy(&r, &sign, 8); return r;
    }
    if (exp == 0 && frac < 0x4000000000000ULL) { uint64_t r = sign | 0x7ff0000000000000ULL; double o; memcpy(&o, &r, 8); return o; }
    uint64_t f52 = frac;
    if (exp == 0) {
        if (!(f52 >> 51 & 1)) { exp = -1; f52 = (f52 << 2) & 0xfffffffffffffULL; }
        else f52 = (f52 << 1) & 0xfffffffffffffULL;
    }
    int scaled = 0x100 | (int)(f52 >> 44);
    int rexp = 2045 - exp;
    int est = recip_estimate(scaled);
    uint64_t rf = (uint64_t)(est & 0xff) << 44;
    if (rexp == 0) rf = (1ULL << 51) | (rf >> 1);
    else if (rexp == -1) { rf = (1ULL << 50) | (rf >> 2); rexp = 0; }
    uint64_t r = sign | ((uint64_t)(rexp & 0x7ff) << 52) | rf;
    double o; memcpy(&o, &r, 8); return o;
}

static int rsqrt_estimate(int a) {      /* a in 128..511 */
    if (a < 256) a = a * 2 + 1;
    else a = ((a >> 1) << 1), a = (a + 1) * 2;
    int b = 512;
    while ((int64_t)a * (b + 1) * (b + 1) < (1LL << 28)) b++;
    return (b + 1) / 2;
}

double frsqrte64(double x) {
    uint64_t u; memcpy(&u, &x, 8);
    uint64_t sign = u >> 63, frac = u & 0xfffffffffffffULL;
    int exp = (int)((u >> 52) & 0x7ff);
    if (exp == 0x7ff && frac) { u |= 0x0008000000000000ULL; double r; memcpy(&r, &u, 8); return r; }
    if (exp == 0 && frac == 0) { uint64_t r = (sign << 63) | 0x7ff0000000000000ULL; double o; memcpy(&o, &r, 8); return o; }
    if (sign) { uint64_t r = 0x7ff8000000000000ULL; double o; memcpy(&o, &r, 8); return o; }
    if (exp == 0x7ff) return 0.0;
    if (exp == 0) {
        while (!(frac >> 51 & 1)) { frac = (frac << 1) & 0xfffffffffffffULL; exp--; }
        frac = (frac << 1) & 0xfffffffffffffULL;
    }
    int scaled = (exp & 1) ? (0x80 | (int)(frac >> 45)) : (0x100 | (int)(frac >> 44));
    int rexp = (3068 - exp) / 2;
    int est = rsqrt_estimate(scaled);
    uint64_t r = ((uint64_t)(rexp & 0x7ff) << 52) | ((uint64_t)(est & 0xff) << 44);
    double o; memcpy(&o, &r, 8); return o;
}

float frsqrte32(float x) {
    uint32_t u; memcpy(&u, &x, 4);
    uint32_t sign = u >> 31; uint64_t frac = (uint64_t)(u & 0x7fffff) << 29;
    int exp = (int)((u >> 23) & 0xff);
    if (exp == 0xff && frac) { u |= 0x00400000u; float r; memcpy(&r, &u, 4); return r; }
    if (exp == 0 && frac == 0) { uint32_t r = (sign << 31) | 0x7f800000u; float o; memcpy(&o, &r, 4); return o; }
    if (sign) { uint32_t r = 0x7fc00000u; float o; memcpy(&o, &r, 4); return o; }
    if (exp == 0xff) return 0.0f;
    if (exp == 0) {
        while (!(frac >> 51 & 1)) { frac = (frac << 1) & 0xfffffffffffffULL; exp--; }
        frac = (frac << 1) & 0xfffffffffffffULL;
    }
    int scaled = (exp & 1) ? (0x80 | (int)(frac >> 45)) : (0x100 | (int)(frac >> 44));
    int rexp = (380 - exp) / 2;
    int est = rsqrt_estimate(scaled);
    uint32_t r = ((uint32_t)(rexp & 0xff) << 23) | ((uint32_t)(est & 0xff) << 15);
    float o; memcpy(&o, &r, 4); return o;
}

float half_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 0x1f, frac = h & 0x3ff, u;
    if (exp == 0) {
        if (!frac) u = sign;
        else { int e = -1; do { e++; frac <<= 1; } while (!(frac & 0x400)); u = sign | ((uint32_t)(112 - e) << 23) | ((frac & 0x3ff) << 13); }
    } else if (exp == 31) u = sign | 0x7f800000u | (frac << 13);
    else u = sign | ((exp + 112) << 23) | (frac << 13);
    float f; memcpy(&f, &u, 4); return f;
}

uint16_t f64_to_half(double d) {        /* round to nearest even */
    uint64_t u; memcpy(&u, &d, 8);
    uint16_t sign = (uint16_t)((u >> 63) << 15);
    int exp = (int)((u >> 52) & 0x7ff); uint64_t frac = u & 0xfffffffffffffULL;
    if (exp == 0x7ff) return sign | 0x7c00 | (frac ? 0x200 | (uint16_t)(frac >> 42) : 0);
    if (exp == 0 && frac == 0) return sign;
    int e = exp - 1023;
    uint64_t m = frac | (1ULL << 52);       /* 53 bits */
    if (e > 15) return sign | 0x7c00;
    int shift;
    if (e >= -14) shift = 42; else { shift = 42 + (-14 - e); if (shift > 63) return sign; }
    uint64_t q = m >> shift, rem = m & ((1ULL << shift) - 1), half = 1ULL << (shift - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    if (e >= -14) {
        uint32_t r = ((uint32_t)(e + 15) << 10) + (uint32_t)(q - 0x400);
        return r >= 0x7c00 ? (uint16_t)(sign | 0x7c00) : (uint16_t)(sign | r);
    }
    return (uint16_t)(sign | q);
}

uint16_t f32_to_half(float f) { return f64_to_half((double)f); }

/* ---- a sampler for finding where translated code is spending its time or spinning: every `period_ms` it stops
 * the calling thread, reads its instruction pointer and names the translated function that contains it. */
static HANDLE g_watch_thread;
static DWORD g_watch_period;
static DWORD WINAPI watchdog(LPVOID arg) {
    (void)arg;
    for (int n = 0; n < 40; n++) {
        Sleep(g_watch_period);
        CONTEXT ctx; memset(&ctx, 0, sizeof ctx); ctx.ContextFlags = CONTEXT_CONTROL;
        SuspendThread(g_watch_thread);
        GetThreadContext(g_watch_thread, &ctx);
        ResumeThread(g_watch_thread);
        uint64_t best = 0, addr = 0;
        for (int i = 0; i < a2c_nfuncs; i++) {
            uint64_t f = (uint64_t)a2c_funcs[i].fn;
            if (f <= ctx.Rip && f > best) { best = f; addr = a2c_funcs[i].addr; }
        }
        if (ctx.Rip - best > 0x2000)     /* not in translated code: report the address for the linker map */
            fprintf(stderr, "a2c: sample: host code at image offset %llx\n", (unsigned long long)(ctx.Rip - (uint64_t)GetModuleHandleA(NULL)));
        const char *name = "";
        for (int i = 0; a2c_exports[i].name; i++) if ((uint64_t)a2c_exports[i].fn == best) { name = a2c_exports[i].name; break; }
        fprintf(stderr, "a2c: sample: in function %llx (image %d) +%llu bytes of host code %s\n",
                (unsigned long long)(addr >= IMG2 ? addr - IMG2 : addr - IMG1), addr >= IMG2 ? 2 : 1,
                (unsigned long long)(ctx.Rip - best), name);
        fflush(stderr);
    }
    return 0;
}
void a2c_watch(unsigned period_ms) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_watch_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    g_watch_period = period_ms;
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
}

/* ---- the same as a profiler: sample the calling thread every millisecond and count per translated function.
 * a2c_profile_report prints the functions the samples fell in, most first. */
static volatile int g_prof_run;
static uint32_t *g_prof_hits;      /* per a2c_funcs entry, plus one for host code */
static uint64_t g_prof_total;
static uint64_t *g_prof_sorted; static int *g_prof_index;   /* host addresses of the functions, ascending */
static struct { uint64_t bucket; uint32_t n; } g_prof_host[4096];
static DWORD WINAPI profiler(LPVOID arg) {
    (void)arg;
    while (g_prof_run) {
        Sleep(1);
        CONTEXT ctx; memset(&ctx, 0, sizeof ctx); ctx.ContextFlags = CONTEXT_CONTROL;
        SuspendThread(g_watch_thread);
        GetThreadContext(g_watch_thread, &ctx);
        ResumeThread(g_watch_thread);
        int lo = 0, hi = a2c_nfuncs - 1, at = -1;
        while (lo <= hi) { int mid = (lo + hi) / 2; if (g_prof_sorted[mid] <= ctx.Rip) { at = mid; lo = mid + 1; } else hi = mid - 1; }
        if (at >= 0 && (at + 1 < a2c_nfuncs || ctx.Rip - g_prof_sorted[at] < 0x1000)) g_prof_hits[g_prof_index[at]]++;
        else {      /* host code: count by 64-byte bucket of image offset, for looking up in the linker map */
            g_prof_hits[a2c_nfuncs]++;
            uint64_t b = (ctx.Rip - (uint64_t)GetModuleHandleA(NULL)) >> 6;
            unsigned h = (unsigned)(b * 2654435761u) & 4095;
            while (g_prof_host[h].n && g_prof_host[h].bucket != b) h = (h + 1) & 4095;
            g_prof_host[h].bucket = b; g_prof_host[h].n++;
        }
        g_prof_total++;
    }
    return 0;
}
void a2c_profile_start(void) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_watch_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    g_prof_hits = calloc((size_t)a2c_nfuncs + 1, 4);
    g_prof_sorted = malloc((size_t)a2c_nfuncs * 8); g_prof_index = malloc((size_t)a2c_nfuncs * sizeof(int));
    for (int i = 0; i < a2c_nfuncs; i++) {      /* insertion into order by host address */
        uint64_t f = (uint64_t)a2c_funcs[i].fn; int j = i;
        while (j > 0 && g_prof_sorted[j - 1] > f) { g_prof_sorted[j] = g_prof_sorted[j - 1]; g_prof_index[j] = g_prof_index[j - 1]; j--; }
        g_prof_sorted[j] = f; g_prof_index[j] = i;
    }
    g_prof_run = 1;
    CreateThread(NULL, 0, profiler, NULL, 0, NULL);
}
void a2c_profile_report(int top) {
    g_prof_run = 0; Sleep(20);
    fprintf(stderr, "a2c: profile, %llu samples; host code (C library stand-ins, dispatch): %.1f%%\n",
            (unsigned long long)g_prof_total, g_prof_total ? 100.0 * g_prof_hits[a2c_nfuncs] / g_prof_total : 0.0);
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < 4096; i++) if (g_prof_host[i].n && (best < 0 || g_prof_host[i].n > g_prof_host[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, "  %5.1f%%  host code at image offset %llx\n", 100.0 * g_prof_host[best].n / g_prof_total,
                (unsigned long long)(g_prof_host[best].bucket << 6));
        g_prof_host[best].n = 0;
    }
    for (int k = 0; k < top; k++) {
        int best = -1;
        for (int i = 0; i < a2c_nfuncs; i++) if (g_prof_hits[i] && (best < 0 || g_prof_hits[i] > g_prof_hits[best])) best = i;
        if (best < 0) break;
        const char *name = "";
        for (int i = 0; a2c_exports[i].name; i++) if (a2c_exports[i].fn == a2c_funcs[best].fn) { name = a2c_exports[i].name; break; }
        uint64_t addr = a2c_funcs[best].addr;
        fprintf(stderr, "  %5.1f%%  %llx  %.90s\n", 100.0 * g_prof_hits[best] / g_prof_total,
                (unsigned long long)(addr >= IMG2 ? addr - IMG2 : addr - IMG1), name);
        g_prof_hits[best] = 0;
    }
}
