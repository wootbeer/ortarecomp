/*
 * NV2A vertex program interpreter. See vsh.h.
 *
 * Instruction layout (128 bits; word 0 carries nothing, as emulators
 * document it): word 1 holds the ILU and MAC opcodes, the constant and input
 * indices and operand A's sign and swizzle; word 2 operand A's register and
 * mux, operand B, and operand C up to the top bits of its register; word 3
 * the rest of C, the write masks, the destination and the final flag.
 *
 * Semantics, including the JSRF-specific points:
 * - Both units read the registers as they were before the instruction.
 * - When the MAC and the ILU both run, the ILU's temp destination is R1.
 * - R12 is the same storage as oPos.
 * - A relative constant index is clamped to 0..191, not wrapped.
 * - x * 0 is 0 even for infinities, as on the hardware (an unwritten or
 *   zero normal must not turn a position into NaN). This file is compiled
 *   without fused multiply-add for the same reason: results are rounded
 *   per operation, as the hardware does.
 * - Unwritten outputs: oD0 = 1, oD1 = 0, oT = 0, oPts = 1, oFog = 1.
 */

#include "guest/vsh.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <string.h>

static uint32_t bits(const uint32_t *w, int word, int pos, int size)
{
    return (w[word] >> pos) & ((1u << size) - 1u);
}

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MUX_NONE, MUX_R, MUX_V, MUX_C };

/* Fields, from the instruction layout above. */
static void decode(const uint32_t *w, pdo_vsh_op *o)
{
    static const uint8_t mac_use[16] = {
        0, 1, 3, 5, 7, 3, 3, 3, 3, 3, 3, 3, 3, 1, 0, 0,   /* bits: A B C */
    };
    o->ilu = (uint8_t)bits(w, 1, 25, 3);
    o->mac = (uint8_t)bits(w, 1, 21, 4);
    o->cidx = (uint8_t)bits(w, 1, 13, 8);
    o->vidx = (uint8_t)bits(w, 1, 9, 4);
    o->src[0].neg = (uint8_t)bits(w, 1, 8, 1);
    o->src[0].swz[0] = (uint8_t)bits(w, 1, 6, 2); o->src[0].swz[1] = (uint8_t)bits(w, 1, 4, 2);
    o->src[0].swz[2] = (uint8_t)bits(w, 1, 2, 2); o->src[0].swz[3] = (uint8_t)bits(w, 1, 0, 2);
    o->src[0].reg = (uint8_t)bits(w, 2, 28, 4); o->src[0].mux = (uint8_t)bits(w, 2, 26, 2);
    o->src[1].neg = (uint8_t)bits(w, 2, 25, 1);
    o->src[1].swz[0] = (uint8_t)bits(w, 2, 23, 2); o->src[1].swz[1] = (uint8_t)bits(w, 2, 21, 2);
    o->src[1].swz[2] = (uint8_t)bits(w, 2, 19, 2); o->src[1].swz[3] = (uint8_t)bits(w, 2, 17, 2);
    o->src[1].reg = (uint8_t)bits(w, 2, 13, 4); o->src[1].mux = (uint8_t)bits(w, 2, 11, 2);
    o->src[2].neg = (uint8_t)bits(w, 2, 10, 1);
    o->src[2].swz[0] = (uint8_t)bits(w, 2, 8, 2); o->src[2].swz[1] = (uint8_t)bits(w, 2, 6, 2);
    o->src[2].swz[2] = (uint8_t)bits(w, 2, 4, 2); o->src[2].swz[3] = (uint8_t)bits(w, 2, 2, 2);
    o->src[2].reg = (uint8_t)((bits(w, 2, 0, 2) << 2) | bits(w, 3, 30, 2));
    o->src[2].mux = (uint8_t)bits(w, 3, 28, 2);
    o->mac_mask = (uint8_t)bits(w, 3, 24, 4);
    o->out_r = (uint8_t)bits(w, 3, 20, 4);
    o->ilu_mask = (uint8_t)bits(w, 3, 16, 4);
    o->o_mask = (uint8_t)bits(w, 3, 12, 4);
    o->orb = (uint8_t)bits(w, 3, 11, 1);
    o->addr = (uint8_t)bits(w, 3, 3, 8);
    o->out_mux = (uint8_t)bits(w, 3, 2, 1);
    o->rel = (uint8_t)bits(w, 3, 1, 1);
    /* Paired with the MAC, the ILU's temp destination is R1. */
    o->ilu_r = o->mac != MAC_NOP ? 1 : o->out_r;
    o->use = (uint8_t)(mac_use[o->mac] | (o->ilu != ILU_NOP ? 4 : 0));
}

/* The fast path's register file: r0-r11, oPos (r12), v0-v15, zeros. */
enum { ROW_OPOS = 12, ROW_V = 13, ROW_ZERO = 29, FILE_ROWS = 30 };

static void compile_src(const pdo_vsh_src *sr, uint32_t vidx, pdo_vsh_fsrc *f)
{
    memset(f, 0, sizeof *f);
    f->neg = sr->neg;
    memcpy(f->swz, sr->swz, 4);
    switch (sr->mux) {
    case MUX_R: f->row = (uint8_t)(sr->reg <= 12 ? sr->reg : ROW_ZERO); break;
    case MUX_V: f->row = (uint8_t)(ROW_V + (vidx & 15)); break;
    case MUX_C: f->is_const = 1; break;
    default:    f->row = ROW_ZERO; break;
    }
    for (int k = 0; k < 4; k++)
        f->slot[k] = (uint8_t)(f->row * 4 + sr->swz[k]);
    if (sr->swz[0] == 0 && sr->swz[1] == 1 && sr->swz[2] == 2 && sr->swz[3] == 3)
        f->kind = 1;
    else if (sr->swz[0] == sr->swz[1] && sr->swz[1] == sr->swz[2] && sr->swz[2] == sr->swz[3])
        f->kind = 2;
}

static void compile(const pdo_vsh_op *o, pdo_vsh_fop *f)
{
    memset(f, 0, sizeof *f);
    f->mac = o->mac;
    f->ilu = o->ilu;
    f->use = o->use;
    f->rel = o->rel;
    f->cidx = o->cidx;
    for (int k = 0; k < 3; k++)
        compile_src(&o->src[k], o->vidx, &f->src[k]);
    f->mac_row = (o->mac != MAC_NOP && o->mac != MAC_ARL && o->mac_mask && o->out_r <= 12) ? o->out_r : 255;
    f->ilu_row = (o->ilu != ILU_NOP && o->ilu_mask && o->ilu_r <= 12) ? o->ilu_r : 255;
    f->mac_mask = o->mac_mask;
    f->ilu_mask = o->ilu_mask;
    f->o_mask = o->o_mask;
    f->out_mux = o->out_mux;
    if (o->o_mask) {
        if (o->orb) {
            if (o->addr == VSH_OPOS) { f->o_kind = 1; f->o_index = ROW_OPOS; }
            else if (o->addr < VSH_OUTPUTS) { f->o_kind = 2; f->o_index = o->addr; }
        } else if (o->addr < PDO_VSH_CONSTANTS) {
            f->o_kind = 3; f->o_index = o->addr;
        }
    }
}

static void setup_fast(void);

static uint32_t gpu_src(const pdo_vsh_fsrc *f)
{
    return f->row | (uint32_t)f->is_const << 8 | (uint32_t)f->neg << 9 | (uint32_t)f->swz[0] << 12
           | (uint32_t)f->swz[1] << 14 | (uint32_t)f->swz[2] << 16 | (uint32_t)f->swz[3] << 18;
}

bool pdo_vsh_gpu_code(const pdo_vsh_program *p, uint32_t *out)
{
    if (p->writes_constants)
        return false;
    for (uint32_t i = 0; i < p->count; i++) {
        const pdo_vsh_fop *f = &p->fop[i];
        uint32_t *w = out + 8 * i;
        w[0] = f->mac | (uint32_t)f->ilu << 4 | (uint32_t)f->use << 8 | (uint32_t)f->rel << 12
               | (uint32_t)f->cidx << 16 | (uint32_t)f->out_mux << 24;
        w[1] = f->mac_row | (uint32_t)f->ilu_row << 8 | (uint32_t)(f->o_kind == 3 ? 0 : f->o_kind) << 16
               | (uint32_t)f->o_index << 24;
        w[2] = f->mac_mask | (uint32_t)f->ilu_mask << 4 | (uint32_t)f->o_mask << 8;
        w[3] = gpu_src(&f->src[0]);
        w[4] = gpu_src(&f->src[1]);
        w[5] = gpu_src(&f->src[2]);
        w[6] = w[7] = 0;
    }
    return true;
}

/* ── Declaration and function parsing ─────────────────────────────────── */

uint32_t pdo_vsh_type_size(uint8_t type)
{
    switch (type) {
    case 0x12: case 0x22: case 0x32: case 0x42: return 4u * (type >> 4);  /* FLOATn */
    case 0x40: return 4;                                                  /* D3DCOLOR */
    case 0x15: case 0x25: case 0x35: case 0x45:                           /* SHORTn */
    case 0x11: case 0x21: case 0x31: case 0x41: return 2u * (type >> 4);  /* NORMSHORTn */
    case 0x16: return 4;                                                  /* NORMPACKED3 */
    case 0x14: case 0x24: case 0x34: case 0x44: return type >> 4;         /* PBYTEn */
    default: return 0;
    }
}

bool pdo_vsh_parse(pdo_vsh_program *p, const uint32_t *function, uint32_t function_words,
                   const uint32_t *decl, uint32_t decl_words)
{
    uint32_t head, slots, stream = 0, offset[16] = {0};

    setup_fast();
    memset(p, 0, sizeof *p);
    p->gpu_program = -1;
    if (!function || function_words < 1)
        return false;
    head = function[0];
    slots = head >> 16;
    /* XDK program header: low half 0x2078 (a vertex program) or 0x7978 (a
     * state program); only the former is drawn with. */
    if ((head & 0xFFFF) != 0x2078 || slots == 0 || slots > PDO_VSH_MAX_INSNS
            || 1 + 4 * slots > function_words)
        return false;
    p->count = slots;
    memcpy(p->insn, function + 1, slots * 16);
    for (uint32_t i = 0; i < slots; i++) {
        decode(p->insn[i], &p->op[i]);
        compile(&p->op[i], &p->fop[i]);
        for (int k = 0; k < 3; k++)
            if ((p->op[i].use & (1u << k)) && p->op[i].src[k].mux == 2)
                p->inputs_read |= (uint16_t)(1u << p->op[i].vidx);
        for (int k = 0; k < 3; k++)
            if ((p->op[i].use & (1u << k)) && p->op[i].src[k].mux == MUX_C) {
                if (p->op[i].rel)           /* c[a0.x + n]: any of them */
                    memset(p->consts_read, 0xFF, sizeof p->consts_read);
                else if (p->op[i].cidx < PDO_VSH_CONSTANTS)
                    p->consts_read[p->op[i].cidx >> 3] |= (uint8_t)(1u << (p->op[i].cidx & 7));
            }
        if (p->op[i].o_mask && !p->op[i].orb)
            p->writes_constants = true;
        if (bits(p->insn[i], 3, 0, 1)) {          /* final */
            p->count = i + 1;
            break;
        }
    }

    for (uint32_t i = 0; decl && i < decl_words; i++) {
        uint32_t t = decl[i], kind = t >> 29;
        if (t == 0xFFFFFFFFu)                    /* D3DVSD_END */
            break;
        if (kind == 1) {                         /* D3DVSD_STREAM(n) */
            stream = t & 0xF;
        } else if (kind == 2) {                  /* STREAMDATA */
            if (t & 0x10000000u) {               /* SKIP: dwords, or bytes */
                uint32_t n = (t >> 16) & 0xFF;
                offset[stream] += (t & 0x08000000u) ? n : 4u * n;
            } else {                             /* REG(reg, type) */
                uint32_t reg = t & 0x1F, type = (t >> 16) & 0xFF;
                if (reg < PDO_VSH_INPUTS) {
                    p->input[reg].stream = (uint8_t)stream;
                    p->input[reg].type = (uint8_t)type;
                    p->input[reg].offset = (uint16_t)offset[stream];
                    p->input[reg].present = pdo_vsh_type_size((uint8_t)type) != 0;
                }
                offset[stream] += pdo_vsh_type_size((uint8_t)type);
            }
        } else if (kind == 4) {                  /* CONSTMEM: count * 4 words */
            i += 4 * ((t >> 25) & 0xF);
        } else if (kind == 5) {                  /* EXT: count words */
            i += (t >> 24) & 0x1F;
        }
    }
    return true;
}

static float s16(const uint8_t *s) { int16_t v; memcpy(&v, s, 2); return (float)v; }
static float f32p(const uint8_t *s) { float v; memcpy(&v, s, 4); return v; }

void pdo_vsh_fetch(uint8_t type, const uint8_t *src, float out[4])
{
    uint32_t n = type >> 4;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type & 0x0F) {
    case 0x2:                                    /* FLOATn */
        for (uint32_t i = 0; i < n && i < 4; i++) out[i] = f32p(src + 4 * i);
        break;
    case 0x0:                                    /* D3DCOLOR: B, G, R, A */
        if (type == 0x40) {
            out[0] = src[2] / 255.0f; out[1] = src[1] / 255.0f;
            out[2] = src[0] / 255.0f; out[3] = src[3] / 255.0f;
        }
        break;
    case 0x5:                                    /* SHORTn, not normalised */
        for (uint32_t i = 0; i < n && i < 4; i++) out[i] = s16(src + 2 * i);
        break;
    case 0x1:                                    /* NORMSHORTn */
        for (uint32_t i = 0; i < n && i < 4; i++) {
            float v = s16(src + 2 * i) / 32767.0f;
            out[i] = v < -1.0f ? -1.0f : v;
        }
        break;
    case 0x6: {                                  /* NORMPACKED3: 11:11:10 signed */
        uint32_t v; memcpy(&v, src, 4);
        int32_t x = (int32_t)(v << 21) >> 21, y = (int32_t)(v << 10) >> 21, z = (int32_t)v >> 22;
        out[0] = x / 1023.0f; out[1] = y / 1023.0f; out[2] = z / 511.0f;
        break;
    }
    case 0x4:                                    /* PBYTEn */
        for (uint32_t i = 0; i < n && i < 4; i++) out[i] = src[i] / 255.0f;
        break;
    default:
        break;
    }
}

/* ── Execution ────────────────────────────────────────────────────────── */

static float mulz(float a, float b)
{
    return (a == 0.0f || b == 0.0f) ? 0.0f : a * b;
}

typedef struct vm {
    float r[13][4];          /* r12 is oPos: read and written through out[0] */
    float (*out)[4];
    float (*c)[4];
    const float (*v)[4];
    int   a0;
} vm;

static const float *reg_ptr(vm *m, uint32_t mux, uint32_t r, uint32_t vidx, int cidx)
{
    static const float zero[4] = { 0, 0, 0, 0 };
    switch (mux) {
    case MUX_R: return r == 12 ? m->out[VSH_OPOS] : (r < 12 ? m->r[r] : zero);
    case MUX_V: return m->v[vidx & 15];
    case MUX_C: return m->c[cidx];
    default:    return zero;
    }
}

/* Operand: swizzle and sign applied. */
static void operand(vm *m, const pdo_vsh_src *sr, uint32_t vidx, int cidx, float o[4])
{
    const float *src = reg_ptr(m, sr->mux, sr->reg, vidx, cidx);
    if (sr->neg) {
        o[0] = -src[sr->swz[0]]; o[1] = -src[sr->swz[1]];
        o[2] = -src[sr->swz[2]]; o[3] = -src[sr->swz[3]];
    } else {
        o[0] = src[sr->swz[0]]; o[1] = src[sr->swz[1]];
        o[2] = src[sr->swz[2]]; o[3] = src[sr->swz[3]];
    }
}

static void write_masked(float *dst, const float v[4], uint32_t mask)
{
    /* Mask bits: 8 = x, 4 = y, 2 = z, 1 = w. */
    for (int i = 0; i < 4; i++)
        if (mask & (8u >> i))
            dst[i] = v[i];
}

static float rcp(float x)
{
    return x == 1.0f ? 1.0f : 1.0f / x;
}

static float rcc(float x)
{
    float r = 1.0f / x, a = fabsf(r);
    if (a < 5.42101086e-20f) a = 5.42101086e-20f;
    else if (a > 1.884467e+19f) a = 1.884467e+19f;
    return signbit(r) ? -a : a;
}

/* Set by a caller re-running one vertex to log every instruction
 * (PDO_VSH_BAD in d3d_hle.c); single-threaded use only. */
_Thread_local bool pdo_vsh_trace;

static void run_ref(const pdo_vsh_program *p, const float in[PDO_VSH_INPUTS][4],
                    float c[PDO_VSH_CONSTANTS][4], float out[VSH_OUTPUTS][4])
{
    vm m;

    memset(&m, 0, sizeof m);
    memset(out, 0, VSH_OUTPUTS * sizeof out[0]);
    out[VSH_OPOS][3] = 1.0f;
    for (int i = 0; i < 4; i++) {
        out[VSH_OD0][i] = 1.0f;
        out[VSH_OB0][i] = 1.0f;
    }
    out[VSH_OFOG][0] = out[VSH_OFOG][1] = out[VSH_OFOG][2] = out[VSH_OFOG][3] = 1.0f;
    out[VSH_OPTS][0] = 1.0f;
    m.out = out;
    m.c = c;
    m.v = in;

    for (uint32_t pc = 0; pc < p->count; pc++) {
        const pdo_vsh_op *op = &p->op[pc];
        uint32_t ilu = op->ilu, mac = op->mac;
        int cidx = op->cidx;
        float a[4], b[4], cc[4], mr[4] = {0}, ir[4] = {0};

        if (op->rel) {                           /* relative constant: c[a0.x + n] */
            cidx += m.a0;
            cidx = cidx < 0 ? 0 : cidx > 191 ? 191 : cidx;
        }
        /* Operands read the state before this instruction writes anything. */
        if (op->use & 1) operand(&m, &op->src[0], op->vidx, cidx, a);
        if (op->use & 2) operand(&m, &op->src[1], op->vidx, cidx, b);
        if (op->use & 4) operand(&m, &op->src[2], op->vidx, cidx, cc);

        switch (mac) {
        case MAC_MOV: memcpy(mr, a, sizeof mr); break;
        case MAC_MUL: for (int i = 0; i < 4; i++) mr[i] = mulz(a[i], b[i]); break;
        case MAC_ADD: for (int i = 0; i < 4; i++) mr[i] = a[i] + cc[i]; break;
        case MAC_MAD: for (int i = 0; i < 4; i++) mr[i] = mulz(a[i], b[i]) + cc[i]; break;
        case MAC_DP3: {
            float d = mulz(a[0], b[0]) + mulz(a[1], b[1]) + mulz(a[2], b[2]);
            mr[0] = mr[1] = mr[2] = mr[3] = d;
            break;
        }
        case MAC_DPH: {
            float d = mulz(a[0], b[0]) + mulz(a[1], b[1]) + mulz(a[2], b[2]) + b[3];
            mr[0] = mr[1] = mr[2] = mr[3] = d;
            break;
        }
        case MAC_DP4: {
            float d = mulz(a[0], b[0]) + mulz(a[1], b[1]) + mulz(a[2], b[2]) + mulz(a[3], b[3]);
            mr[0] = mr[1] = mr[2] = mr[3] = d;
            break;
        }
        case MAC_DST:
            mr[0] = 1.0f; mr[1] = mulz(a[1], b[1]); mr[2] = a[2]; mr[3] = b[3];
            break;
        case MAC_MIN: for (int i = 0; i < 4; i++) mr[i] = a[i] < b[i] ? a[i] : b[i]; break;
        case MAC_MAX: for (int i = 0; i < 4; i++) mr[i] = a[i] > b[i] ? a[i] : b[i]; break;
        case MAC_SLT: for (int i = 0; i < 4; i++) mr[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
        case MAC_SGE: for (int i = 0; i < 4; i++) mr[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
        default: break;
        }

        switch (ilu) {
        case ILU_MOV: memcpy(ir, cc, sizeof ir); break;
        case ILU_RCP: ir[0] = ir[1] = ir[2] = ir[3] = rcp(cc[0]); break;
        case ILU_RCC: ir[0] = ir[1] = ir[2] = ir[3] = rcc(cc[0]); break;
        case ILU_RSQ: ir[0] = ir[1] = ir[2] = ir[3] = 1.0f / sqrtf(fabsf(cc[0])); break;
        case ILU_EXP: {
            float f = floorf(cc[0]);
            ir[0] = exp2f(f); ir[1] = cc[0] - f; ir[2] = exp2f(cc[0]); ir[3] = 1.0f;
            break;
        }
        case ILU_LOG: {
            float x = fabsf(cc[0]);
            if (x == 0.0f) {
                ir[0] = ir[1] = ir[2] = -INFINITY;
            } else {
                int e;
                float mant = frexpf(x, &e);       /* x = mant * 2^e, mant in [0.5, 1) */
                ir[0] = (float)(e - 1); ir[1] = mant * 2.0f; ir[2] = log2f(x);
            }
            ir[3] = 1.0f;
            break;
        }
        case ILU_LIT: {
            float d = cc[0] > 0 ? cc[0] : 0, s = cc[1] > 0 ? cc[1] : 0;
            float e = cc[3] < -127.9961f ? -127.9961f : cc[3] > 127.9961f ? 127.9961f : cc[3];
            ir[0] = 1.0f; ir[1] = d; ir[2] = cc[0] > 0 ? powf(s, e) : 0.0f; ir[3] = 1.0f;
            break;
        }
        default: break;
        }

        if (pdo_vsh_trace)
            SDL_Log("vsh %3u: mac %u ilu %u c%d%s a0 %d | A %g %g %g %g | B %g %g %g %g | C %g %g %g %g"
                    " | mac %g %g %g %g -> r%u/%x | ilu %g %g %g %g -> r%u/%x | o %s%u/%x",
                    pc, mac, ilu, cidx, op->rel ? "(rel)" : "", m.a0, a[0], a[1], a[2], a[3], b[0], b[1], b[2],
                    b[3], cc[0], cc[1], cc[2], cc[3], mr[0], mr[1], mr[2], mr[3], op->out_r, op->mac_mask,
                    ir[0], ir[1], ir[2], ir[3], op->ilu_r, op->ilu_mask, op->orb ? "o" : "c", op->addr,
                    op->o_mask);
        /* Writes, after both units have computed. */
        if (mac == MAC_ARL) {
            m.a0 = (int)floorf(a[0]);
        } else if (mac != MAC_NOP && op->mac_mask) {
            float *dst = op->out_r == 12 ? out[VSH_OPOS] : (op->out_r < 12 ? m.r[op->out_r] : NULL);
            if (dst) write_masked(dst, mr, op->mac_mask);
        }
        if (ilu != ILU_NOP && op->ilu_mask) {
            float *dst = op->ilu_r == 12 ? out[VSH_OPOS] : (op->ilu_r < 12 ? m.r[op->ilu_r] : NULL);
            if (dst) write_masked(dst, ir, op->ilu_mask);
        }
        if (op->o_mask) {
            const float *v = op->out_mux ? ir : mr;
            if (op->orb) {
                if (op->addr < VSH_OUTPUTS) write_masked(out[op->addr], v, op->o_mask);
            } else if (op->addr < PDO_VSH_CONSTANTS) {
                write_masked(c[op->addr], v, op->o_mask);
            }
        }
    }
}

/* ── The fast path ────────────────────────────────────────────────────────
 * The same program from its compiled form (compile above), four lanes at a
 * time with SSE2, giving the same results as run_ref bit for bit: x * 0 is
 * 0, dot products add in the same order, nothing is fused. PDO_VSH_CHECK=1
 * runs both on every vertex and logs the first differences. */

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define VSH_FAST 1
#include <emmintrin.h>

#ifdef _MSC_VER
#define VSH_INLINE static __forceinline
#else
#define VSH_INLINE static inline __attribute__((always_inline))
#endif

static __m128 mask_of[16];
static __m128 sign_bits, ones;

static void fast_init(void)
{
    for (int m = 0; m < 16; m++) {
        uint32_t w[4];
        for (int i = 0; i < 4; i++)
            w[i] = (m & (8 >> i)) ? 0xFFFFFFFFu : 0;
        memcpy(&mask_of[m], w, 16);
    }
    sign_bits = _mm_set1_ps(-0.0f);
    ones = _mm_set1_ps(1.0f);
}

static inline __m128 fetch_src(float (*file)[4], float (*c)[4], const pdo_vsh_fsrc *s, int cidx)
{
    const float *b = s->is_const ? c[cidx] : file[s->row];
    __m128 v;

    if (s->kind == 1)
        v = _mm_loadu_ps(b);
    else if (s->kind == 2)
        v = _mm_set1_ps(b[s->swz[0]]);
    else
        v = _mm_setr_ps(b[s->swz[0]], b[s->swz[1]], b[s->swz[2]], b[s->swz[3]]);
    return s->neg ? _mm_xor_ps(v, sign_bits) : v;
}

VSH_INLINE __m128 mulz4(__m128 a, __m128 b)
{
    __m128 z = _mm_setzero_ps();
    __m128 keep = _mm_and_ps(_mm_cmpneq_ps(a, z), _mm_cmpneq_ps(b, z));
    return _mm_and_ps(_mm_mul_ps(a, b), keep);
}

static inline void store_masked(float *dst, __m128 v, uint32_t mask)
{
    __m128 m = mask_of[mask & 15];
    _mm_storeu_ps(dst, _mm_or_ps(_mm_and_ps(m, v), _mm_andnot_ps(m, _mm_loadu_ps(dst))));
}

static void run_fast(const pdo_vsh_program *p, const float in[PDO_VSH_INPUTS][4],
                     float c[PDO_VSH_CONSTANTS][4], float out[VSH_OUTPUTS][4])
{
    float file[FILE_ROWS][4];
    int a0 = 0;

    memset(file, 0, sizeof file);
    file[ROW_OPOS][3] = 1.0f;
    for (int r = 0; r < PDO_VSH_INPUTS; r++)
        if (p->inputs_read & (1u << r))
            memcpy(file[ROW_V + r], in[r], 16);
    memset(out, 0, VSH_OUTPUTS * sizeof out[0]);
    for (int i = 0; i < 4; i++) {
        out[VSH_OD0][i] = 1.0f;
        out[VSH_OB0][i] = 1.0f;
        out[VSH_OFOG][i] = 1.0f;
    }
    out[VSH_OPTS][0] = 1.0f;

    for (uint32_t pc = 0; pc < p->count; pc++) {
        const pdo_vsh_fop *op = &p->fop[pc];
        int cidx = op->cidx;
        __m128 a = ones, b = ones, cc = ones, mr = _mm_setzero_ps(), ir = _mm_setzero_ps();

        if (op->rel) {
            cidx += a0;
            cidx = cidx < 0 ? 0 : cidx > 191 ? 191 : cidx;
        }
        if (op->use & 1) a = fetch_src(file, c, &op->src[0], cidx);
        if (op->use & 2) b = fetch_src(file, c, &op->src[1], cidx);
        if (op->use & 4) cc = fetch_src(file, c, &op->src[2], cidx);

        switch (op->mac) {
        case MAC_MOV: mr = a; break;
        case MAC_MUL: mr = mulz4(a, b); break;
        case MAC_ADD: mr = _mm_add_ps(a, cc); break;
        case MAC_MAD: mr = _mm_add_ps(mulz4(a, b), cc); break;
        case MAC_DP3: case MAC_DPH: case MAC_DP4: {
            float t[4], bb[4], d;
            _mm_storeu_ps(t, mulz4(a, b));
            d = t[0] + t[1] + t[2];
            if (op->mac == MAC_DP4)
                d += t[3];
            else if (op->mac == MAC_DPH) {
                _mm_storeu_ps(bb, b);
                d += bb[3];
            }
            mr = _mm_set1_ps(d);
            break;
        }
        case MAC_DST: {
            float aa[4], bb[4], m1;
            _mm_storeu_ps(aa, a);
            _mm_storeu_ps(bb, b);
            m1 = (aa[1] == 0.0f || bb[1] == 0.0f) ? 0.0f : aa[1] * bb[1];
            mr = _mm_setr_ps(1.0f, m1, aa[2], bb[3]);
            break;
        }
        case MAC_MIN: mr = _mm_min_ps(a, b); break;
        case MAC_MAX: mr = _mm_max_ps(a, b); break;
        case MAC_SLT: mr = _mm_and_ps(_mm_cmplt_ps(a, b), ones); break;
        case MAC_SGE: mr = _mm_and_ps(_mm_cmpge_ps(a, b), ones); break;
        case MAC_ARL: a0 = (int)floorf(_mm_cvtss_f32(a)); break;
        default: break;
        }

        if (op->ilu != ILU_NOP) {
            float x[4], r[4] = { 0, 0, 0, 0 };
            _mm_storeu_ps(x, cc);
            switch (op->ilu) {
            case ILU_MOV: memcpy(r, x, sizeof r); break;
            case ILU_RCP: r[0] = r[1] = r[2] = r[3] = rcp(x[0]); break;
            case ILU_RCC: r[0] = r[1] = r[2] = r[3] = rcc(x[0]); break;
            case ILU_RSQ: r[0] = r[1] = r[2] = r[3] = 1.0f / sqrtf(fabsf(x[0])); break;
            case ILU_EXP: {
                float f = floorf(x[0]);
                r[0] = exp2f(f); r[1] = x[0] - f; r[2] = exp2f(x[0]); r[3] = 1.0f;
                break;
            }
            case ILU_LOG: {
                float v = fabsf(x[0]);
                if (v == 0.0f) {
                    r[0] = r[1] = r[2] = -INFINITY;
                } else {
                    int e;
                    float mant = frexpf(v, &e);
                    r[0] = (float)(e - 1); r[1] = mant * 2.0f; r[2] = log2f(v);
                }
                r[3] = 1.0f;
                break;
            }
            case ILU_LIT: {
                float d = x[0] > 0 ? x[0] : 0, sp = x[1] > 0 ? x[1] : 0;
                float e = x[3] < -127.9961f ? -127.9961f : x[3] > 127.9961f ? 127.9961f : x[3];
                r[0] = 1.0f; r[1] = d; r[2] = x[0] > 0 ? powf(sp, e) : 0.0f; r[3] = 1.0f;
                break;
            }
            default: break;
            }
            ir = _mm_loadu_ps(r);
        }

        /* Writes, after both units have computed. */
        if (op->mac_row != 255)
            store_masked(file[op->mac_row], mr, op->mac_mask);
        if (op->ilu_row != 255)
            store_masked(file[op->ilu_row], ir, op->ilu_mask);
        if (op->o_kind) {
            __m128 v = op->out_mux ? ir : mr;
            float *dst = op->o_kind == 1 ? file[op->o_index]
                       : op->o_kind == 2 ? out[op->o_index] : c[op->o_index];
            store_masked(dst, v, op->o_mask);
        }
    }
    memcpy(out[VSH_OPOS], file[ROW_OPOS], 16);
}

/* Four vertices at once, one per lane (structure of arrays): register
 * R[row][component] holds that component of four vertices, so a swizzle
 * only picks which component to read, and every operation is the scalar
 * one done four times, giving the same bits per vertex. */
/* An operand for four lanes: from the file, four loads at precomputed
 * slots; from a constant, each component broadcast, or with a relative
 * index (a0 per lane) gathered. */
VSH_INLINE void fetch4(const __m128 *F, float (*c)[4], const pdo_vsh_fsrc *s, int cidx,
                       const int ci[4], bool rel, __m128 o[4])
{
    if (!s->is_const) {
        o[0] = F[s->slot[0]]; o[1] = F[s->slot[1]];
        o[2] = F[s->slot[2]]; o[3] = F[s->slot[3]];
    } else if (!rel) {
        const float *k = c[cidx];
        o[0] = _mm_set1_ps(k[s->swz[0]]); o[1] = _mm_set1_ps(k[s->swz[1]]);
        o[2] = _mm_set1_ps(k[s->swz[2]]); o[3] = _mm_set1_ps(k[s->swz[3]]);
    } else {
        for (int k = 0; k < 4; k++) {
            int w = s->swz[k];
            o[k] = _mm_setr_ps(c[ci[0]][w], c[ci[1]][w], c[ci[2]][w], c[ci[3]][w]);
        }
    }
    if (s->neg) {
        o[0] = _mm_xor_ps(o[0], sign_bits); o[1] = _mm_xor_ps(o[1], sign_bits);
        o[2] = _mm_xor_ps(o[2], sign_bits); o[3] = _mm_xor_ps(o[3], sign_bits);
    }
}

/* Components under a write mask (8 x, 4 y, 2 z, 1 w); all four is common. */
VSH_INLINE void write4(__m128 *dst, const __m128 *v, uint32_t mask)
{
    if (mask == 15) {
        dst[0] = v[0]; dst[1] = v[1]; dst[2] = v[2]; dst[3] = v[3];
        return;
    }
    if (mask & 8) dst[0] = v[0];
    if (mask & 4) dst[1] = v[1];
    if (mask & 2) dst[2] = v[2];
    if (mask & 1) dst[3] = v[3];
}

static float lane_rcc(float x) { return rcc(x); }

static void run_fast4(const pdo_vsh_program *p, const float in[4][PDO_VSH_INPUTS][4],
                      float c[PDO_VSH_CONSTANTS][4], float out[4][VSH_OUTPUTS][4])
{
    __m128 F[FILE_ROWS * 4], O[VSH_OUTPUTS][4];
    __m128 (*R)[4] = (__m128 (*)[4])F;
    int a0[4] = { 0, 0, 0, 0 };
    const __m128 zero = _mm_setzero_ps();

    for (int r = 0; r < FILE_ROWS * 4; r++)
        F[r] = zero;
    R[ROW_OPOS][3] = ones;
    for (int r = 0; r < PDO_VSH_INPUTS; r++)
        if (p->inputs_read & (1u << r)) {
            __m128 x = _mm_loadu_ps(in[0][r]), y = _mm_loadu_ps(in[1][r]);
            __m128 z = _mm_loadu_ps(in[2][r]), w = _mm_loadu_ps(in[3][r]);
            _MM_TRANSPOSE4_PS(x, y, z, w);
            R[ROW_V + r][0] = x; R[ROW_V + r][1] = y; R[ROW_V + r][2] = z; R[ROW_V + r][3] = w;
        }
    for (int o = 0; o < VSH_OUTPUTS; o++)
        for (int k = 0; k < 4; k++)
            O[o][k] = zero;
    for (int k = 0; k < 4; k++) {
        O[VSH_OD0][k] = ones;
        O[VSH_OB0][k] = ones;
        O[VSH_OFOG][k] = ones;
    }
    O[VSH_OPTS][0] = ones;

    for (uint32_t pc = 0; pc < p->count; pc++) {
        const pdo_vsh_fop *op = &p->fop[pc];
        __m128 A[4], B[4], C[4], M[4], I[4];
        int ci[4] = { op->cidx, op->cidx, op->cidx, op->cidx };

        if (op->rel)
            for (int l = 0; l < 4; l++) {
                int x = op->cidx + a0[l];
                ci[l] = x < 0 ? 0 : x > 191 ? 191 : x;
            }
        if (op->use & 1) fetch4(F, c, &op->src[0], op->cidx, ci, op->rel, A);
        if (op->use & 2) fetch4(F, c, &op->src[1], op->cidx, ci, op->rel, B);
        if (op->use & 4) fetch4(F, c, &op->src[2], op->cidx, ci, op->rel, C);
        M[0] = M[1] = M[2] = M[3] = zero;
        I[0] = I[1] = I[2] = I[3] = zero;

        switch (op->mac) {
        case MAC_MOV: for (int k = 0; k < 4; k++) M[k] = A[k]; break;
        case MAC_MUL: for (int k = 0; k < 4; k++) M[k] = mulz4(A[k], B[k]); break;
        case MAC_ADD: for (int k = 0; k < 4; k++) M[k] = _mm_add_ps(A[k], C[k]); break;
        case MAC_MAD: for (int k = 0; k < 4; k++) M[k] = _mm_add_ps(mulz4(A[k], B[k]), C[k]); break;
        case MAC_DP3: case MAC_DPH: case MAC_DP4: {
            __m128 d = _mm_add_ps(_mm_add_ps(mulz4(A[0], B[0]), mulz4(A[1], B[1])), mulz4(A[2], B[2]));
            if (op->mac == MAC_DP4) d = _mm_add_ps(d, mulz4(A[3], B[3]));
            else if (op->mac == MAC_DPH) d = _mm_add_ps(d, B[3]);
            M[0] = M[1] = M[2] = M[3] = d;
            break;
        }
        case MAC_DST:
            M[0] = ones; M[1] = mulz4(A[1], B[1]); M[2] = A[2]; M[3] = B[3];
            break;
        case MAC_MIN: for (int k = 0; k < 4; k++) M[k] = _mm_min_ps(A[k], B[k]); break;
        case MAC_MAX: for (int k = 0; k < 4; k++) M[k] = _mm_max_ps(A[k], B[k]); break;
        case MAC_SLT: for (int k = 0; k < 4; k++) M[k] = _mm_and_ps(_mm_cmplt_ps(A[k], B[k]), ones); break;
        case MAC_SGE: for (int k = 0; k < 4; k++) M[k] = _mm_and_ps(_mm_cmpge_ps(A[k], B[k]), ones); break;
        case MAC_ARL: {
            float x[4];
            _mm_storeu_ps(x, A[0]);
            for (int l = 0; l < 4; l++)
                a0[l] = (int)floorf(x[l]);
            break;
        }
        default: break;
        }

        switch (op->ilu) {
        case ILU_NOP: break;
        case ILU_MOV: for (int k = 0; k < 4; k++) I[k] = C[k]; break;
        case ILU_RCP:   /* 1 / x, with 1 / 1 = 1 as the reference has it (it is, in IEEE) */
            I[0] = I[1] = I[2] = I[3] = _mm_div_ps(ones, C[0]);
            break;
        case ILU_RSQ:
            I[0] = I[1] = I[2] = I[3] =
                _mm_div_ps(ones, _mm_sqrt_ps(_mm_andnot_ps(sign_bits, C[0])));
            break;
        default: {      /* RCC, EXP, LOG, LIT: per lane, as the reference */
            float x[4][4], r[4][4];
            for (int k = 0; k < 4; k++)
                _mm_storeu_ps(x[k], C[k]);
            for (int l = 0; l < 4; l++) {
                float v0 = x[0][l], v1 = x[1][l], v3 = x[3][l];
                switch (op->ilu) {
                case ILU_RCC: r[0][l] = r[1][l] = r[2][l] = r[3][l] = lane_rcc(v0); break;
                case ILU_EXP: {
                    float f = floorf(v0);
                    r[0][l] = exp2f(f); r[1][l] = v0 - f; r[2][l] = exp2f(v0); r[3][l] = 1.0f;
                    break;
                }
                case ILU_LOG: {
                    float v = fabsf(v0);
                    if (v == 0.0f) {
                        r[0][l] = r[1][l] = r[2][l] = -INFINITY;
                    } else {
                        int e;
                        float mant = frexpf(v, &e);
                        r[0][l] = (float)(e - 1); r[1][l] = mant * 2.0f; r[2][l] = log2f(v);
                    }
                    r[3][l] = 1.0f;
                    break;
                }
                case ILU_LIT: {
                    float d = v0 > 0 ? v0 : 0, sp = v1 > 0 ? v1 : 0;
                    float e = v3 < -127.9961f ? -127.9961f : v3 > 127.9961f ? 127.9961f : v3;
                    r[0][l] = 1.0f; r[1][l] = d; r[2][l] = v0 > 0 ? powf(sp, e) : 0.0f; r[3][l] = 1.0f;
                    break;
                }
                default:
                    r[0][l] = r[1][l] = r[2][l] = r[3][l] = 0.0f;
                    break;
                }
            }
            for (int k = 0; k < 4; k++)
                I[k] = _mm_loadu_ps(r[k]);
            break;
        }
        }

        /* Writes, after both units have computed. Mask bits: 8 x .. 1 w. */
        if (op->mac_row != 255)
            write4(R[op->mac_row], M, op->mac_mask);
        if (op->ilu_row != 255)
            write4(R[op->ilu_row], I, op->ilu_mask);
        if (op->o_kind == 1 || op->o_kind == 2)
            write4(op->o_kind == 1 ? R[op->o_index] : O[op->o_index], op->out_mux ? I : M, op->o_mask);
    }
    for (int k = 0; k < 4; k++)
        O[VSH_OPOS][k] = R[ROW_OPOS][k];
    for (int o = 0; o < VSH_OUTPUTS; o++) {
        __m128 x = O[o][0], y = O[o][1], z = O[o][2], w = O[o][3];
        _MM_TRANSPOSE4_PS(x, y, z, w);
        _mm_storeu_ps(out[0][o], x);
        _mm_storeu_ps(out[1][o], y);
        _mm_storeu_ps(out[2][o], z);
        _mm_storeu_ps(out[3][o], w);
    }
}
#endif

static bool same_bits(const float *a, const float *b, int n)
{
    for (int i = 0; i < n; i++)
        if (memcmp(&a[i], &b[i], 4) && !(a[i] != a[i] && b[i] != b[i]))
            return false;
    return true;
}

static int check_fast;

/* Once, from pdo_vsh_parse on the game's thread, before any program runs. */
static void setup_fast(void)
{
    static bool done;

    if (done)
        return;
    done = true;
#ifdef VSH_FAST
    fast_init();
#endif
    check_fast = SDL_getenv("PDO_VSH_CHECK") && SDL_atoi(SDL_getenv("PDO_VSH_CHECK")) != 0;
}

void pdo_vsh_run(const pdo_vsh_program *p, const float in[PDO_VSH_INPUTS][4],
                 float c[PDO_VSH_CONSTANTS][4], float out[VSH_OUTPUTS][4])
{
#ifdef VSH_FAST
    if (pdo_vsh_trace) {
        run_ref(p, in, c, out);
        return;
    }
    run_fast(p, in, c, out);
    if (check_fast && !p->writes_constants) {
        static SDL_AtomicInt reported;
        float ref[VSH_OUTPUTS][4];
        run_ref(p, in, c, ref);
        if (!same_bits(&out[0][0], &ref[0][0], VSH_OUTPUTS * 4) && SDL_AddAtomicInt(&reported, 1) < 16) {
            for (int o = 0; o < VSH_OUTPUTS; o++)
                if (!same_bits(out[o], ref[o], 4))
                    SDL_Log("vsh check: %u insns, output %d fast %g %g %g %g ref %g %g %g %g", p->count, o,
                            out[o][0], out[o][1], out[o][2], out[o][3], ref[o][0], ref[o][1], ref[o][2], ref[o][3]);
        }
    }
#else
    run_ref(p, in, c, out);
#endif
}

void pdo_vsh_run4(const pdo_vsh_program *p, const float in[4][PDO_VSH_INPUTS][4],
                  float c[PDO_VSH_CONSTANTS][4], float out[4][VSH_OUTPUTS][4])
{
#ifdef VSH_FAST
    if (!p->writes_constants && !pdo_vsh_trace) {
        run_fast4(p, in, c, out);
        if (check_fast) {
            static SDL_AtomicInt reported;
            for (int l = 0; l < 4; l++) {
                float ref[VSH_OUTPUTS][4];
                run_ref(p, in[l], c, ref);
                if (!same_bits(&out[l][0][0], &ref[0][0], VSH_OUTPUTS * 4) && SDL_AddAtomicInt(&reported, 1) < 16)
                    for (int o = 0; o < VSH_OUTPUTS; o++)
                        if (!same_bits(out[l][o], ref[o], 4))
                            SDL_Log("vsh check4: %u insns lane %d output %d fast %g %g %g %g ref %g %g %g %g",
                                    p->count, l, o, out[l][o][0], out[l][o][1], out[l][o][2], out[l][o][3],
                                    ref[o][0], ref[o][1], ref[o][2], ref[o][3]);
            }
        }
        return;
    }
#endif
    for (int l = 0; l < 4; l++)
        pdo_vsh_run(p, in[l], c, out[l]);
}

/* ── Worker pool ──────────────────────────────────────────────────────── */

#define MAX_WORKERS 15

static struct {
    SDL_Semaphore *go, *done;
    SDL_AtomicInt  next;              /* next chunk to take */
    uint32_t       count, chunk;
    void         (*fn)(void *, uint32_t, uint32_t);
    void          *ctx;
    int            workers;
} pool;

static void run_chunks(void)
{
    for (;;) {
        uint32_t c = (uint32_t)SDL_AddAtomicInt(&pool.next, 1);
        uint32_t first = c * pool.chunk;
        if (first >= pool.count)
            return;
        pool.fn(pool.ctx, first, first + pool.chunk < pool.count ? first + pool.chunk : pool.count);
    }
}

static int SDLCALL worker(void *unused)
{
    (void)unused;
    for (;;) {
        SDL_WaitSemaphore(pool.go);
        run_chunks();
        SDL_SignalSemaphore(pool.done);
    }
    return 0;
}

void pdo_vsh_parallel(uint32_t count, void (*fn)(void *, uint32_t, uint32_t), void *ctx)
{
    /* Small draws are not worth waking anyone for. */
    pdo_parallel(count, 64, 256, fn, ctx);
}

void pdo_parallel(uint32_t count, uint32_t chunk, uint32_t min_count,
                  void (*fn)(void *, uint32_t, uint32_t), void *ctx)
{
    static bool started;

    if (count < min_count || count <= chunk) {
        fn(ctx, 0, count);
        return;
    }
    if (!started) {
        int cores = SDL_GetNumLogicalCPUCores();
        started = true;
        pool.go = SDL_CreateSemaphore(0);
        pool.done = SDL_CreateSemaphore(0);
        /* Leave a core for the game's own threads. */
        pool.workers = cores - 2;
        if (pool.workers > MAX_WORKERS) pool.workers = MAX_WORKERS;
        for (int i = 0; i < pool.workers; i++)
            if (!SDL_CreateThread(worker, "pdo_vsh", NULL)) {
                pool.workers = i;
                break;
            }
        SDL_Log("vsh: %d worker threads", pool.workers);
    }
    if (pool.workers <= 0) {
        fn(ctx, 0, count);
        return;
    }
    pool.fn = fn;
    pool.ctx = ctx;
    pool.count = count;
    pool.chunk = chunk;
    SDL_SetAtomicInt(&pool.next, 0);
    /* Only as many workers as there are chunks besides the caller's own:
     * one woken with nothing to do costs a wake and a sleep (on phones,
     * power) for nothing. */
    int wake = (int)((count + chunk - 1) / chunk) - 1;
    if (wake > pool.workers)
        wake = pool.workers;
    for (int i = 0; i < wake; i++)
        SDL_SignalSemaphore(pool.go);
    run_chunks();
    for (int i = 0; i < wake; i++)
        SDL_WaitSemaphore(pool.done);
}
