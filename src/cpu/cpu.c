#include "cpu.h"

cpu_info_t g_cpu;

/* ---- low-level CPUID wrapper ---- */
static void cpuid(cpu_u32 leaf, cpu_u32 *eax, cpu_u32 *ebx, cpu_u32 *ecx, cpu_u32 *edx)
{
    cpu_u32 a, b, c, d;
    __asm__ volatile (
        "cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(leaf), "c"(0)
    );
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

/* Some very old 486s lack CPUID. We probe the ID bit in EFLAGS. */
static cpu_bool cpu_has_cpuid(void)
{
    /* In long mode every modern CPU has CPUID. Always true. */
    return CPU_TRUE;
}

static void cpu_copy_vendor(char *dst, cpu_u32 ebx, cpu_u32 edx, cpu_u32 ecx)
{
    /* Vendor string is stored as EBX:EDX:ECX (12 bytes) */
    dst[0]  = (char)(ebx & 0xFF);
    dst[1]  = (char)((ebx >> 8) & 0xFF);
    dst[2]  = (char)((ebx >> 16) & 0xFF);
    dst[3]  = (char)((ebx >> 24) & 0xFF);
    dst[4]  = (char)(edx & 0xFF);
    dst[5]  = (char)((edx >> 8) & 0xFF);
    dst[6]  = (char)((edx >> 16) & 0xFF);
    dst[7]  = (char)((edx >> 24) & 0xFF);
    dst[8]  = (char)(ecx & 0xFF);
    dst[9]  = (char)((ecx >> 8) & 0xFF);
    dst[10] = (char)((ecx >> 16) & 0xFF);
    dst[11] = (char)((ecx >> 24) & 0xFF);
    dst[12] = '\0';
}

static void cpu_copy_brand_chunk(char *dst, cpu_u32 eax, cpu_u32 ebx, cpu_u32 ecx, cpu_u32 edx)
{
    cpu_u32 regs[4] = { eax, ebx, ecx, edx };
    for (int i = 0; i < 4; i++) {
        dst[i*4 + 0] = (char)(regs[i] & 0xFF);
        dst[i*4 + 1] = (char)((regs[i] >> 8) & 0xFF);
        dst[i*4 + 2] = (char)((regs[i] >> 16) & 0xFF);
        dst[i*4 + 3] = (char)((regs[i] >> 24) & 0xFF);
    }
}

void cpu_init(void)
{
    if (g_cpu.initialized) return;

    /* zero everything first */
    for (int i = 0; i < CPU_VENDOR_LEN; i++) g_cpu.vendor[i] = 0;
    for (int i = 0; i < CPU_BRAND_LEN; i++)  g_cpu.brand[i]  = 0;
    g_cpu.max_basic = 0;
    g_cpu.max_ext   = 0;
    g_cpu.family    = 0;
    g_cpu.model     = 0;
    g_cpu.stepping  = 0;
    g_cpu.features_edx = 0;
    g_cpu.features_ecx = 0;
    g_cpu.has_cpuid = CPU_FALSE;
    g_cpu.initialized = CPU_TRUE;

    if (!cpu_has_cpuid()) {
        /* extremely old CPU */
        g_cpu.vendor[0] = '?';
        g_cpu.brand[0]  = '?';
        return;
    }
    g_cpu.has_cpuid = CPU_TRUE;

    cpu_u32 eax, ebx, ecx, edx;

    /* Leaf 0: max basic leaf + vendor */
    cpuid(0, &eax, &ebx, &ecx, &edx);
    g_cpu.max_basic = eax;
    cpu_copy_vendor(g_cpu.vendor, ebx, edx, ecx);

    /* Leaf 1: family / model / stepping + feature flags */
    if (g_cpu.max_basic >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        g_cpu.stepping = eax & 0xF;
        g_cpu.model    = (eax >> 4) & 0xF;
        g_cpu.family   = (eax >> 8) & 0xF;
        /* extended model/family for modern CPUs */
        if (g_cpu.family == 0xF) {
            g_cpu.family += (eax >> 20) & 0xFF;
        }
        if (g_cpu.family == 0x6 || g_cpu.family == 0xF) {
            g_cpu.model += ((eax >> 16) & 0xF) << 4;
        }
        g_cpu.features_edx = edx;
        g_cpu.features_ecx = ecx;
    }

    /* Extended leaves for brand string */
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    g_cpu.max_ext = eax;

    if (g_cpu.max_ext >= 0x80000004) {
        char brand[48 + 1];
        for (int i = 0; i < 49; i++) brand[i] = 0;

        cpuid(0x80000002, &eax, &ebx, &ecx, &edx);
        cpu_copy_brand_chunk(brand + 0,  eax, ebx, ecx, edx);
        cpuid(0x80000003, &eax, &ebx, &ecx, &edx);
        cpu_copy_brand_chunk(brand + 16, eax, ebx, ecx, edx);
        cpuid(0x80000004, &eax, &ebx, &ecx, &edx);
        cpu_copy_brand_chunk(brand + 32, eax, ebx, ecx, edx);

        /* trim leading spaces (common on some AMD/Intel strings) */
        int start = 0;
        while (brand[start] == ' ') start++;
        int j = 0;
        while (brand[start + j] && j < CPU_BRAND_LEN - 1) {
            g_cpu.brand[j] = brand[start + j];
            j++;
        }
        g_cpu.brand[j] = '\0';
    } else {
        /* no brand string available */
        int i = 0;
        const char *v = g_cpu.vendor;
        while (v[i] && i < CPU_BRAND_LEN - 1) {
            g_cpu.brand[i] = v[i];
            i++;
        }
        g_cpu.brand[i] = '\0';
    }
}

/* Feature bit helpers (leaf 1) */
#define CPUID_FEAT_EDX_FPU    (1u << 0)
#define CPUID_FEAT_EDX_TSC    (1u << 4)
#define CPUID_FEAT_EDX_MSR    (1u << 5)
#define CPUID_FEAT_EDX_CX8    (1u << 8)
#define CPUID_FEAT_EDX_APIC   (1u << 9)
#define CPUID_FEAT_EDX_SEP    (1u << 11)
#define CPUID_FEAT_EDX_CMOV   (1u << 15)
#define CPUID_FEAT_EDX_CLFSH  (1u << 19)
#define CPUID_FEAT_EDX_MMX    (1u << 23)
#define CPUID_FEAT_EDX_FXSR   (1u << 24)
#define CPUID_FEAT_EDX_SSE    (1u << 25)
#define CPUID_FEAT_EDX_SSE2   (1u << 26)
#define CPUID_FEAT_EDX_HTT    (1u << 28)

#define CPUID_FEAT_ECX_SSE3   (1u << 0)
#define CPUID_FEAT_ECX_SSSE3  (1u << 9)
#define CPUID_FEAT_ECX_SSE41  (1u << 19)
#define CPUID_FEAT_ECX_SSE42  (1u << 20)
#define CPUID_FEAT_ECX_AES    (1u << 25)
#define CPUID_FEAT_ECX_AVX    (1u << 28)
#define CPUID_FEAT_ECX_XSAVE  (1u << 26)
#define CPUID_FEAT_ECX_OSXSAVE (1u << 27)

void cpu_cmd_info(void (*printfn)(const char *fmt, ...))
{
    if (!g_cpu.initialized)
        cpu_init();

    if (!g_cpu.has_cpuid) {
        printfn("CPU: CPUID not supported (very old CPU)\n");
        return;
    }

    printfn("CPU Information\n");
    printfn("---------------\n");
    printfn("  Vendor   : %s\n", g_cpu.vendor);
    printfn("  Brand    : %s\n", g_cpu.brand[0] ? g_cpu.brand : "(none)");
    printfn("  Family   : %u\n", g_cpu.family);
    printfn("  Model    : %u\n", g_cpu.model);
    printfn("  Stepping : %u\n", g_cpu.stepping);
    printfn("  Max leaf : %x (ext %x)\n", g_cpu.max_basic, g_cpu.max_ext);

    printfn("  Features :");
    cpu_u32 edx = g_cpu.features_edx;
    cpu_u32 ecx = g_cpu.features_ecx;
    int any = 0;

    if (edx & CPUID_FEAT_EDX_FPU)   { printfn(" FPU"); any=1; }
    if (edx & CPUID_FEAT_EDX_TSC)   { printfn(" TSC"); any=1; }
    if (edx & CPUID_FEAT_EDX_MSR)   { printfn(" MSR"); any=1; }
    if (edx & CPUID_FEAT_EDX_CX8)   { printfn(" CX8"); any=1; }
    if (edx & CPUID_FEAT_EDX_APIC)  { printfn(" APIC"); any=1; }
    if (edx & CPUID_FEAT_EDX_SEP)   { printfn(" SEP"); any=1; }
    if (edx & CPUID_FEAT_EDX_CMOV)  { printfn(" CMOV"); any=1; }
    if (edx & CPUID_FEAT_EDX_MMX)   { printfn(" MMX"); any=1; }
    if (edx & CPUID_FEAT_EDX_FXSR)  { printfn(" FXSR"); any=1; }
    if (edx & CPUID_FEAT_EDX_SSE)   { printfn(" SSE"); any=1; }
    if (edx & CPUID_FEAT_EDX_SSE2)  { printfn(" SSE2"); any=1; }
    if (edx & CPUID_FEAT_EDX_HTT)   { printfn(" HTT"); any=1; }

    if (ecx & CPUID_FEAT_ECX_SSE3)  { printfn(" SSE3"); any=1; }
    if (ecx & CPUID_FEAT_ECX_SSSE3) { printfn(" SSSE3"); any=1; }
    if (ecx & CPUID_FEAT_ECX_SSE41) { printfn(" SSE4.1"); any=1; }
    if (ecx & CPUID_FEAT_ECX_SSE42) { printfn(" SSE4.2"); any=1; }
    if (ecx & CPUID_FEAT_ECX_AES)   { printfn(" AES"); any=1; }
    if (ecx & CPUID_FEAT_ECX_AVX)   { printfn(" AVX"); any=1; }
    if (ecx & CPUID_FEAT_ECX_XSAVE) { printfn(" XSAVE"); any=1; }

    if (!any) printfn(" (none detected)");
    printfn("\n");
}
