#version 450
// NV2A vertex programs on the GPU: the same instruction set and results as
// src/guest/vsh.c (the CPU interpreter is the reference), one vertex per
// invocation. The program comes compiled (vsh.c, compile: every operand a
// row of one register file or a constant), the constants per draw, and the
// vertex streams as the game's raw bytes, decoded here per input type.
//
// Register file rows: r0-r11, oPos (r12), v0-v15 (13-28), zeros (29).
// Semantics kept from the CPU: both units read before either writes;
// x * 0 is 0 even for infinities; dot products add left to right; relative
// constants clamp to 0..191; unwritten outputs are oD0 1, oD1 0, oT 0,
// oFog 1, oPos (0, 0, 0, 1).
//
// Built with PDO_VSH_SPEC (src/gfx/vsh_spec.c, at run time), the program
// is not interpreted: run_spec, generated from one program's code, follows
// this file's run() instruction by instruction in straight-line code, its
// registers plain variables. Both runs then read the same program; only
// a draw paired with another program (prev_prog) needs the interpreter.

layout(set = 0, binding = 0, std430) readonly buffer Streams { uint blob[]; };
layout(set = 0, binding = 1, std430) readonly buffer Programs { uvec4 prog[]; };
layout(set = 0, binding = 2, std430) readonly buffer Constants { vec4 cbuf[]; };
// Frame interpolation: the previous game frame's streams and constants.
layout(set = 0, binding = 3, std430) readonly buffer PrevStreams { uint pblob[]; };
layout(set = 0, binding = 4, std430) readonly buffer PrevConstants { vec4 pcbuf[]; };

// The draw's 192 constants and its partner's in the frame before (kp,
// blending), pushed with each draw (renderer.c, push_consts): read from a
// uniform block at fixed places, not from the frame's whole storage buffer
// (cbuf, pcbuf: bound still, unread) -- a phone GPU keeps them on chip.
layout(set = 1, binding = 1, std140) uniform Consts { vec4 kc[192]; vec4 kp[192]; };

layout(set = 1, binding = 0, std140) uniform Draw {
    ivec4 info;          // program base (uvec4 units), instructions, constant base (vec4 units),
                         // reversed depth (renderer.c)
    ivec4 inputs[16];    // per v register: byte base in blob, stride, offset, D3DVSDT type (0: absent)
    vec4  vp_scale;      // the viewport transform the program ends with (c[58]), w unused
    vec4  vp_offset;     // c[59]; w: the depth scale
    // Interpolation (renderer.c, match_frames): x the matching draw's
    // constant base in the previous frame (-1: none), y 1 if its streams
    // match too, z 1 to tint it (PDO_INTERP_TINT: it could not pair);
    // prev_in its v registers' byte bases; blend_t.x how far from
    // the previous frame (1: this frame, as drawn without interpolation).
    // w: a point list's largest point size (float bits; 0 none).
    ivec4 interp;
    ivec4 prev_in[4];
    // y: a point list (1; 2 with point sprites' texture coordinates), each
    // point drawn as a square of its oPts pixels: four vertices a point,
    // vertex index point * 4 + corner; z, w the viewport's width and
    // height in the game's pixels; vp_scale.w the smallest point size.
    vec4  blend_t;
    // Paired with a draw on another program: its base and count (y 0: the
    // same program); w 0. Each program then runs on its own frame's constants and
    // streams, and the outputs are blended. z 1: blending the same program,
    // run it once on blended constants and streams instead (half the work;
    // renderer.c, interp_once).
    ivec4 prev_prog;
    // The paired draw's viewport transform (c[58], c[59]): its positions
    // are in its own pixels (the game renders a boost's and a brake's end
    // at another size).
    vec4  prev_vp_scale;
    vec4  prev_vp_offset;
};

layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec4 v_specular;
layout(location = 2) out vec4 v_tc0;
layout(location = 3) out vec4 v_tc1;
layout(location = 4) out vec4 v_tc2;
layout(location = 5) out vec4 v_tc3;
layout(location = 6) out float v_fog;

// Reads from this frame's streams, or the previous frame's (from_prev).
bool from_prev;
int vidx;       // the vertex: gl_VertexIndex, or its point's in a point list
uint word(uint i) { return from_prev ? pblob[i] : blob[i]; }
uint rd8(uint a)  { return (word(a >> 2) >> ((a & 3u) * 8u)) & 0xFFu; }
uint rd16(uint a) { return rd8(a) | (rd8(a + 1u) << 8); }
uint rd32(uint a)
{
    if ((a & 3u) == 0u) return word(a >> 2);
    return rd8(a) | (rd8(a + 1u) << 8) | (rd8(a + 2u) << 16) | (rd8(a + 3u) << 24);
}
float s16(uint a) { return float(int(rd16(a) << 16) >> 16); }

// One input attribute, as pdo_vsh_fetch decodes it, from byte base 'base'.
vec4 fetch_at(int r, int base)
{
    ivec4 d = inputs[r];
    vec4 o = vec4(0.0, 0.0, 0.0, 1.0);
    uint t = uint(d.w), n = t >> 4;
    uint a = uint(base) + uint(vidx) * uint(d.y) + uint(d.z);
    if (t == 0u) return o;
    uint k = t & 15u;
    // Whole words where the data is aligned (the usual case): one load a
    // word, not one a byte (a phone GPU spent much of a scene's vertex work
    // on these loads). The same values as the byte-wise reads below.
    if (t == 0x40u && (a & 3u) == 0u) {             // D3DCOLOR: B, G, R, A
        vec4 c = unpackUnorm4x8(word(a >> 2));
        return vec4(c.z, c.y, c.x, c.w);
    }
    if ((k == 5u || k == 1u) && (a & 1u) == 0u) {   // SHORTn, NORMSHORTn
        uint w0 = word(a >> 2), w1 = n > 1u ? word((a >> 2) + 1u) : 0u, w2 = n > 2u ? word((a >> 2) + 2u) : 0u;
        uint h = (a >> 1) & 1u;                     // starting in a word's upper half
        for (uint i = 0u; i < n && i < 4u; i++) {
            uint j = i + h, w = j < 2u ? w0 : j < 4u ? w1 : w2;
            float v = float(int((j & 1u) != 0u ? w : w << 16) >> 16);
            o[i] = k == 5u ? v : max(v / 32767.0, -1.0);
        }
        return o;
    }
    if (k == 2u) {                                  // FLOATn
        for (uint i = 0u; i < n && i < 4u; i++) o[i] = uintBitsToFloat(rd32(a + 4u * i));
    } else if (t == 0x40u) {                        // D3DCOLOR: B, G, R, A
        o = vec4(float(rd8(a + 2u)), float(rd8(a + 1u)), float(rd8(a)), float(rd8(a + 3u))) / 255.0;
    } else if (k == 5u) {                           // SHORTn
        for (uint i = 0u; i < n && i < 4u; i++) o[i] = s16(a + 2u * i);
    } else if (k == 1u) {                           // NORMSHORTn
        for (uint i = 0u; i < n && i < 4u; i++) o[i] = max(s16(a + 2u * i) / 32767.0, -1.0);
    } else if (t == 0x16u) {                        // NORMPACKED3: 11:11:10 signed
        uint v = rd32(a);
        o.x = float(int(v << 21) >> 21) / 1023.0;
        o.y = float(int(v << 10) >> 21) / 1023.0;
        o.z = float(int(v) >> 22) / 511.0;
    } else if (k == 4u) {                           // PBYTEn
        for (uint i = 0u; i < n && i < 4u; i++) o[i] = float(rd8(a + i)) / 255.0;
    }
    return o;
}

// 0: one program, constants and streams blended; 1: the earlier frame's
// alone; 2: this frame's alone (prev_prog).
int mode = 0;

vec4 fetch(int r)
{
    if (mode == 1 && interp.y != 0 && inputs[r].w != 0) {
        from_prev = true;
        return fetch_at(r, prev_in[r >> 2][r & 3]);
    }
    from_prev = false;
    if (mode != 0)
        return fetch_at(r, inputs[r].x);
    vec4 cur = fetch_at(r, inputs[r].x);
    if (blend_t.x >= 1.0 || interp.y == 0 || inputs[r].w == 0)
        return cur;
    from_prev = true;
    vec4 prev = fetch_at(r, prev_in[r >> 2][r & 3]);
    return mix(prev, cur, blend_t.x);
}

vec4 mulz(vec4 a, vec4 b)
{
    bvec4 z = bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0))));
    return mix(a * b, vec4(0.0), z);
}

float mulz1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }

bvec4 mask_of(uint m) { return bvec4((m & 8u) != 0u, (m & 4u) != 0u, (m & 2u) != 0u, (m & 1u) != 0u); }

float rcc(float x)
{
    float r = 1.0 / x, a = abs(r);
    a = a < 5.42101086e-20 ? 5.42101086e-20 : a > 1.884467e+19 ? 1.884467e+19 : a;
    return (r < 0.0 || (r == 0.0 && 1.0 / r < 0.0)) ? -a : a;
}

vec4 O[16];

#ifdef PDO_VSH_SPEC
vec4 R[13];                     // run_spec leaves oPos in R[12]

// Constant i of this frame's, the earlier frame's or both blended.
vec4 cread(int i)
{
    if (mode == 1)
        return kp[i];
    if (mode == 2 || blend_t.x >= 1.0 || interp.x < 0)
        return kc[i];
    return mix(kp[i], kc[i], blend_t.x);
}

// A zero the compiler cannot see through (prev_prog.w is always 0): with
// registers starting at a literal zero, a driver folds 0 + x to x and
// -0 comes out where the interpreter has +0 (and 1 / -0 is -inf).
vec4 opaque_zero() { return vec4(intBitsToFloat(prev_prog.w)); }

void run_spec();
#else
vec4 R[30];

vec4 operand(uint w, int cidx)
{
    vec4 s;
    if (((w >> 8) & 1u) == 0u)
        s = R[w & 0xFFu];
    else if (mode == 1)
        s = kp[cidx];
    else if (mode == 2 || blend_t.x >= 1.0 || interp.x < 0)
        s = kc[cidx];
    else
        s = mix(kp[cidx], kc[cidx], blend_t.x);
    vec4 v = vec4(s[(w >> 12) & 3u], s[(w >> 14) & 3u], s[(w >> 16) & 3u], s[(w >> 18) & 3u]);
    return ((w >> 9) & 1u) != 0u ? -v : v;
}

void run(int base, int count)
{
    int a0 = 0;

    for (int i = 0; i < 30; i++) R[i] = vec4(0.0);
    R[12] = vec4(0.0, 0.0, 0.0, 1.0);
    for (int r = 0; r < 16; r++) R[13 + r] = fetch(r);
    for (int i = 0; i < 16; i++) O[i] = vec4(0.0);
    O[3] = vec4(1.0);           // oD0
    O[7] = vec4(1.0);           // oB0
    O[5] = vec4(1.0);           // oFog
    O[6] = vec4(1.0, 0.0, 0.0, 0.0);

    for (int pc = 0; pc < count; pc++) {
        uvec4 h = prog[base + pc * 2], s = prog[base + pc * 2 + 1];
        uint mac = h.x & 15u, ilu = (h.x >> 4) & 7u, use = (h.x >> 8) & 7u;
        int cidx = int((h.x >> 16) & 0xFFu);
        vec4 A = vec4(1.0), B = vec4(1.0), C = vec4(1.0), M = vec4(0.0), I = vec4(0.0);

        if (((h.x >> 12) & 1u) != 0u)
            cidx = clamp(cidx + a0, 0, 191);
        if ((use & 1u) != 0u) A = operand(h.w, cidx);
        if ((use & 2u) != 0u) B = operand(s.x, cidx);
        if ((use & 4u) != 0u) C = operand(s.y, cidx);

        if (mac == 1u) M = A;                                   // MOV
        else if (mac == 2u) M = mulz(A, B);                     // MUL
        else if (mac == 3u) M = A + C;                          // ADD
        else if (mac == 4u) M = mulz(A, B) + C;                 // MAD
        else if (mac == 5u || mac == 6u || mac == 7u) {         // DP3, DPH, DP4
            float d = mulz1(A.x, B.x) + mulz1(A.y, B.y);
            d = d + mulz1(A.z, B.z);
            if (mac == 7u) d = d + mulz1(A.w, B.w);
            else if (mac == 6u) d = d + B.w;
            M = vec4(d);
        }
        else if (mac == 8u) M = vec4(1.0, mulz1(A.y, B.y), A.z, B.w);  // DST
        else if (mac == 9u) M = mix(B, A, lessThan(A, B));             // MIN
        else if (mac == 10u) M = mix(B, A, greaterThan(A, B));         // MAX
        else if (mac == 11u) M = vec4(lessThan(A, B));                 // SLT
        else if (mac == 12u) M = vec4(greaterThanEqual(A, B));         // SGE
        else if (mac == 13u) a0 = int(floor(A.x));                     // ARL

        if (ilu == 1u) I = C;                                          // MOV
        else if (ilu == 2u) I = vec4(C.x == 1.0 ? 1.0 : 1.0 / C.x);    // RCP
        else if (ilu == 3u) I = vec4(rcc(C.x));                        // RCC
        else if (ilu == 4u) I = vec4(1.0 / sqrt(abs(C.x)));           // RSQ
        else if (ilu == 5u) {                                          // EXP
            float f = floor(C.x);
            I = vec4(exp2(f), C.x - f, exp2(C.x), 1.0);
        } else if (ilu == 6u) {                                        // LOG
            float x = abs(C.x);
            if (x == 0.0) {
                I = vec4(uintBitsToFloat(0xFF800000u));
                I.w = 1.0;
            } else {
                int e;
                float m = frexp(x, e);
                I = vec4(float(e - 1), m * 2.0, log2(x), 1.0);
            }
        } else if (ilu == 7u) {                                        // LIT
            float d = max(C.x, 0.0), sp = max(C.y, 0.0), e = clamp(C.w, -127.9961, 127.9961);
            I = vec4(1.0, d, C.x > 0.0 ? pow(sp, e) : 0.0, 1.0);
        }

        // Writes, after both units have computed.
        uint mac_row = h.y & 0xFFu, ilu_row = (h.y >> 8) & 0xFFu;
        uint o_kind = (h.y >> 16) & 0xFFu, o_index = h.y >> 24;
        if (mac_row != 255u) R[mac_row] = mix(R[mac_row], M, mask_of(h.z & 15u));
        if (ilu_row != 255u) R[ilu_row] = mix(R[ilu_row], I, mask_of((h.z >> 4) & 15u));
        if (o_kind == 1u)
            R[o_index] = mix(R[o_index], ((h.x >> 24) & 1u) != 0u ? I : M, mask_of((h.z >> 8) & 15u));
        else if (o_kind == 2u)
            O[o_index] = mix(O[o_index], ((h.x >> 24) & 1u) != 0u ? I : M, mask_of((h.z >> 8) & 15u));
    }
}
#endif

// Undo the viewport transform the program ended with (scale sc, offset of),
// as fetch_vsh does. Reversed depth (renderer.c, info.w): 1 - z from the
// program's own screen z, the subtraction exact.
vec4 to_clip(vec4 p, vec4 sc, vec4 of)
{
    float cw = p.w;
    vec4 c = vec4((p.x - of.x) / sc.x * cw, (p.y - of.y) / sc.y * cw, (p.z - of.z) / of.w * cw, cw);
    if (info.w != 0)
        c.z = ((of.w + of.z) - p.z) / of.w * cw;
    return c;
}

void main()
{
    vidx = blend_t.y != 0.0 ? gl_VertexIndex >> 2 : gl_VertexIndex;
    bool blending = interp.x >= 0 && blend_t.x < 1.0;
    if (blending && prev_prog.z != 0 && prev_prog.y == 0) {
        // Once, on both frames' constants and streams blended (mode 0):
        // for the linear math that places vertices (a matrix, bones'
        // matrices weighted) the same as blending the two runs.
#ifdef PDO_VSH_SPEC
        run_spec();
#else
        run(info.x, info.y);
#endif
        gl_Position = to_clip(R[12], mix(prev_vp_scale, vp_scale, blend_t.x), mix(prev_vp_offset, vp_offset, blend_t.x));
    } else if (blending) {
        // Each frame's run as drawn -- the earlier frame's program (another
        // one across a switch) on its constants and streams, then this
        // frame's -- each through its own viewport, and the outputs blend
        // (one program run on blended constants gave mixed
        // bone matrices, poses of neither frame).
        mode = 1;
#ifdef PDO_VSH_SPEC
        run_spec();
#else
        if (prev_prog.y > 0) run(prev_prog.x, prev_prog.y);
        else run(info.x, info.y);
#endif
        vec4 pc = to_clip(R[12], prev_vp_scale, prev_vp_offset);
        vec4 po[16];
        for (int i = 0; i < 16; i++) po[i] = O[i];
        mode = 2;
#ifdef PDO_VSH_SPEC
        run_spec();
#else
        run(info.x, info.y);
#endif
        vec4 nc = to_clip(R[12], vp_scale, vp_offset);
        // A vertex on the two sides of the eye in the two frames stays as the
        // newest frame has it: blended, it went through w 0 and its triangles
        // covered the screen.
        gl_Position = (pc.w > 0.0) == (nc.w > 0.0) ? mix(pc, nc, blend_t.x) : nc;
        for (int i = 0; i < 16; i++) O[i] = mix(po[i], O[i], blend_t.x);
    } else {
#ifdef PDO_VSH_SPEC
        run_spec();
#else
        run(info.x, info.y);
#endif
        gl_Position = to_clip(R[12], vp_scale, vp_offset);
    }
    if (blend_t.y != 0.0) {
        // A point's square, as draw_points (d3d_hle.c) makes it: oPts
        // pixels (within the smallest and largest sizes) around it.
        int c = gl_VertexIndex & 3;
        float sx = float(c & 1), sy = float(c >> 1);
        float ps = max(O[6].x, vp_scale.w), mx = intBitsToFloat(interp.w);
        if (mx > 0.0) ps = min(ps, mx);
        gl_Position.x += (sx * 2.0 - 1.0) * (ps / blend_t.z * gl_Position.w);
        gl_Position.y -= (sy * 2.0 - 1.0) * (ps / blend_t.w * gl_Position.w);
        if (blend_t.y >= 2.0)
            for (int i = 9; i <= 12; i++) O[i] = vec4(sx, sy, 0.0, 1.0);
    }
    // Colors as the CPU packs them: NaN as 0, clamped, to 8 bits.
    vec4 d0 = mix(clamp(O[3], 0.0, 1.0), vec4(0.0), isnan(O[3]));
    vec4 d1 = mix(clamp(O[4], 0.0, 1.0), vec4(0.0), isnan(O[4]));
    v_diffuse = interp.z != 0 ? vec4(1.0, 0.0, 1.0, 1.0) : floor(d0 * 255.0 + 0.5) / 255.0;
    v_specular = floor(d1 * 255.0 + 0.5) / 255.0;
    v_tc0 = O[9];
    v_tc1 = O[10];
    v_tc2 = O[11];
    v_tc3 = O[12];
    v_fog = O[5].x;
}
