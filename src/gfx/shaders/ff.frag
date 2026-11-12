#version 450
// Direct3D 8 fixed-function texture stages, interpreted from uniforms so one
// shader covers every combination the game sets up. Values are the XDK's:
// D3DTOP_* operations and D3DTA_* arguments.
//
// Built twice (tools/compile_shaders.py): as is, the texture stages only;
// with PIXEL_SHADER, the register-combiner pixel shaders only. One shader
// holding both made the GPU reserve the pixel shader's registers for every
// draw, and at high resolution that alone made the GPU the bottleneck.

layout(location = 0) in vec4 v_diffuse;
layout(location = 1) in vec4 v_specular;
layout(location = 2) in vec4 v_tc0;
layout(location = 3) in vec4 v_tc1;
layout(location = 4) in vec4 v_tc2;
layout(location = 5) in vec4 v_tc3;
layout(location = 6) in float v_fog;

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform sampler2D tex2;
layout(set = 2, binding = 3) uniform sampler2D tex3;

layout(set = 3, binding = 0, std140) uniform FF {
    ivec4 color_op[4];   // op, arg1, arg2, arg0
    ivec4 alpha_op[4];   // op, arg1, arg2, arg0
    ivec4 stage_misc[4]; // result arg, has texture, texcoord set, shadow map
                         // (8 | D3DCMP_* - 0x200, + 16 for 24-bit depth)
    vec4  tfactor;
    ivec4 alpha_test;    // enable, func, ref (0-255), specular enable
    ivec4 ps_rgb_in[2];  // pixel shader (register combiners), 8 stages each
    ivec4 ps_rgb_out[2];
    ivec4 ps_a_in[2];
    ivec4 ps_a_out[2];
    vec4  ps_c0[8];
    vec4  ps_c1[8];
    ivec4 ps_final;      // final ABCD, final EFG, PSCombinerCount, texture modes
    vec4  ps_fc0;
    vec4  ps_fc1;
    vec4  tc_scale[4];   // xy: texel -> normalised for linear textures; z: no alpha;
                         // w: red and green signed (V8U8), stored offset by 128
    ivec4 fog_mode;      // enable, D3DFOGMODE (0 NONE, 1 EXP, 2 EXP2, 3 LINEAR)
    vec4  fog_color;
    vec4  fog_params;    // start, end, density
    vec4  bump_mat[4];   // BUMPENVMAT00, 01, 11, 10
    vec4  bump_lum[4];   // BUMPENVLSCALE, BUMPENVLOFFSET
};

vec4 tc_for(int set)
{
    if (set == 1) return v_tc1;
    if (set == 2) return v_tc2;
    if (set == 3) return v_tc3;
    return v_tc0;
}

// Fog factor: 1 = no fog. The coordinate is an eye distance, or with table
// mode NONE the factor itself (vertex fog).
float fog_factor()
{
    float d = abs(v_fog);
    if (fog_mode.x == 0) return 1.0;
    if (fog_mode.y == 1) return clamp(exp(-fog_params.z * d), 0.0, 1.0);
    if (fog_mode.y == 2) return clamp(exp(-(fog_params.z * d) * (fog_params.z * d)), 0.0, 1.0);
    if (fog_mode.y == 3) {
        float range = fog_params.y - fog_params.x;
        return range != 0.0 ? clamp((fog_params.y - d) / range, 0.0, 1.0) : 1.0;
    }
    return clamp(v_fog, 0.0, 1.0);
}

vec3 apply_fog(vec3 c)
{
    return fog_mode.x != 0 ? mix(fog_color.rgb, c, fog_factor()) : c;
}

vec4 sample_tex(int s, vec2 uv)
{
    vec4 c;
    if (s == 0) c = texture(tex0, uv);
    else if (s == 1) c = texture(tex1, uv);
    else if (s == 2) c = texture(tex2, uv);
    else c = texture(tex3, uv);
    if (tc_scale[s].z != 0.0) c.a = 1.0;
    if (tc_scale[s].w != 0.0) c.rg = clamp((c.rg * 255.0 - 128.0) / 127.0, -1.0, 1.0);
    return c;
}

// Shadow maps. On the Xbox a depth-format texture is not read as color:
// the texel is the outcome of comparing the stored depth with r/q, in that
// order (stored SHADOWFUNC r/q), 1 or 0, filtered between the four nearest
// texels. The order is the reverse of OpenGL's. Orta sets GREATER and
// offsets its shadow pass's depth back: a lit surface stores more than its
// own r/q and reads 1, and its pixel shader dims what reads 0.
// The depth is read with texelFetch and blended here, so it works whatever
// filtering the GPU allows on depth formats.
ivec2 tex_size(int s)
{
    if (s == 0) return textureSize(tex0, 0);
    if (s == 1) return textureSize(tex1, 0);
    if (s == 2) return textureSize(tex2, 0);
    return textureSize(tex3, 0);
}

float fetch_depth_raw(int s, ivec2 p)
{
    p = clamp(p, ivec2(0), tex_size(s) - 1);
    if (s == 0) return texelFetch(tex0, p, 0).r;
    if (s == 1) return texelFetch(tex1, p, 0).r;
    if (s == 2) return texelFetch(tex2, p, 0).r;
    return texelFetch(tex3, p, 0).r;
}

// Reversed depth (stage_misc w + 32, renderer.c): stored as 1 - z.
float fetch_depth(int s, ivec2 p)
{
    float d = fetch_depth_raw(s, p);
    return (stage_misc[s].w & 32) != 0 ? 1.0 - d : d;
}

float shadow_test(int f, float d, float ref)
{
    if (f == 0) return 0.0;                     // NEVER
    if (f == 1) return d < ref ? 1.0 : 0.0;     // LESS
    if (f == 2) return d == ref ? 1.0 : 0.0;    // EQUAL
    if (f == 3) return d <= ref ? 1.0 : 0.0;    // LESSEQUAL
    if (f == 4) return d > ref ? 1.0 : 0.0;     // GREATER
    if (f == 5) return d != ref ? 1.0 : 0.0;    // NOTEQUAL
    if (f == 6) return d >= ref ? 1.0 : 0.0;    // GREATEREQUAL
    return 1.0;                                 // ALWAYS
}

vec4 shadow_stage(int s, vec4 tc)
{
    int m = stage_misc[s].w;
    if (tc.w != 0.0) tc.xyz /= tc.w;
    vec2 p = tc.xy * tc_scale[s].xy * vec2(tex_size(s)) - 0.5;
    vec2 f = fract(p);
    ivec2 i = ivec2(floor(p));
    float ref = tc.z / ((m & 16) != 0 ? 16777215.0 : 65535.0);
    float a = shadow_test(m & 7, fetch_depth(s, i), ref);
    float b = shadow_test(m & 7, fetch_depth(s, i + ivec2(1, 0)), ref);
    float c = shadow_test(m & 7, fetch_depth(s, i + ivec2(0, 1)), ref);
    float d = shadow_test(m & 7, fetch_depth(s, i + ivec2(1, 1)), ref);
    return vec4(mix(mix(a, b, f.x), mix(c, d, f.x), f.y));
}

vec4 sample_stage(int s)
{
    vec4 tc = tc_for(stage_misc[s].z);
    if (stage_misc[s].y == 0) return vec4(1.0);
    if ((stage_misc[s].w & 8) != 0) return shadow_stage(s, tc);
    // Fixed-function stages run as PROJECT2D too: divide by q (see
    // ps_texture; q = 0 is a component nobody wrote).
    if (tc.w != 0.0) tc.xy /= tc.w;
    vec2 uv = tc.xy * tc_scale[s].xy;
    return sample_tex(s, uv);
}

// Whether a stage's op reads D3DTA_TEXTURE (2) in an argument it uses:
// (op, arg1, arg2, arg0); SELECTARG1 (2) reads only arg1, SELECTARG2 (3)
// only arg2, and only MULTIPLYADD (23) and LERP (24) read arg0.
bool uses_texture(ivec4 o)
{
    if (o.x <= 1) return false;
    return (o.x != 3 && (o.y & 0xF) == 2) || (o.x != 2 && (o.z & 0xF) == 2) ||
           ((o.x == 23 || o.x == 24) && (o.w & 0xF) == 2);
}

vec4 arg(int a, vec4 current, vec4 tex, vec4 temp)
{
    vec4 v;
    int sel = a & 0xF;
    if (sel == 0) v = v_diffuse;
    else if (sel == 1) v = current;
    else if (sel == 2) v = tex;
    else if (sel == 3) v = tfactor;
    else if (sel == 4) v = v_specular;
    else v = temp;
    if ((a & 0x10) != 0) v = vec4(1.0) - v;     // D3DTA_COMPLEMENT
    if ((a & 0x20) != 0) v = vec4(v.a);         // D3DTA_ALPHAREPLICATE
    return v;
}

vec4 op(int o, vec4 a1, vec4 a2, vec4 a0, vec4 current, vec4 tex)
{
    if (o == 2)  return a1;                                  // SELECTARG1
    if (o == 3)  return a2;                                  // SELECTARG2
    if (o == 4)  return a1 * a2;                             // MODULATE
    if (o == 5)  return a1 * a2 * 2.0;                       // MODULATE2X
    if (o == 6)  return a1 * a2 * 4.0;                       // MODULATE4X
    if (o == 7)  return a1 + a2;                             // ADD
    if (o == 8)  return a1 + a2 - 0.5;                       // ADDSIGNED
    if (o == 9)  return (a1 + a2 - 0.5) * 2.0;               // ADDSIGNED2X
    if (o == 10) return a1 - a2;                             // SUBTRACT
    if (o == 11) return a1 + a2 - a1 * a2;                   // ADDSMOOTH
    if (o == 12) return mix(a2, a1, v_diffuse.a);            // BLENDDIFFUSEALPHA
    // The Xbox numbers these four differently from PC Direct3D.
    if (o == 13) return mix(a2, a1, current.a);              // BLENDCURRENTALPHA
    if (o == 14) return mix(a2, a1, tex.a);                  // BLENDTEXTUREALPHA
    if (o == 15) return mix(a2, a1, tfactor.a);              // BLENDFACTORALPHA
    if (o == 16) return a1 + a2 * (1.0 - tex.a);             // BLENDTEXTUREALPHAPM
    if (o == 18) return vec4(a1.rgb + a1.a * a2.rgb, a1.a);  // MODULATEALPHA_ADDCOLOR
    if (o == 19) return vec4(a1.rgb * a2.rgb + a1.a, a1.a);  // MODULATECOLOR_ADDALPHA
    if (o == 20) return vec4(a1.rgb + (1.0 - a1.a) * a2.rgb, a1.a);
    if (o == 21) return vec4((1.0 - a1.rgb) * a2.rgb + a1.a, a1.a);
    if (o == 22) return vec4(clamp(4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5), 0.0, 1.0)); // DOTPRODUCT3
    if (o == 23) return a0 + a1 * a2;                        // MULTIPLYADD
    if (o == 24) return mix(a2, a1, a0);                     // LERP
    return a1;
}

bool alpha_pass(float a)
{
    float ref = float(alpha_test.z) / 255.0;
    int f = alpha_test.y;               // GL-valued: 0x200 NEVER .. 0x207 ALWAYS
    if (f == 0x200) return false;
    if (f == 0x201) return a < ref;
    if (f == 0x202) return a == ref;
    if (f == 0x203) return a <= ref;
    if (f == 0x204) return a > ref;
    if (f == 0x205) return a != ref;
    if (f == 0x206) return a >= ref;
    return true;
}

// ── Pixel shaders: NV2A register combiners ────────────────────────────
// Registers: 0 zero, 1 C0, 2 C1, 3 fog, 4 V0 (diffuse), 5 V1 (specular),
// 8-11 T0-T3, 12 R0, 13 R1; in the final combiner also 14 V1+R0, 15 E*F.

#ifdef PIXEL_SHADER
// The registers as separate variables, read and written through switches:
// a register number is the same for every pixel of a draw, so these are
// uniform branches, where an indexed array would live in scratch memory.
vec4 r_c0, r_c1, r_fog, r_v0, r_v1, r_t0, r_t1, r_t2, r_t3, r_r0, r_r1, r_vr, r_ef;

vec4 reg(int i)
{
    switch (i) {
    case 1:  return r_c0;
    case 2:  return r_c1;
    case 3:  return r_fog;
    case 4:  return r_v0;
    case 5:  return r_v1;
    case 8:  return r_t0;
    case 9:  return r_t1;
    case 10: return r_t2;
    case 11: return r_t3;
    case 12: return r_r0;
    case 13: return r_r1;
    case 14: return r_vr;
    case 15: return r_ef;
    }
    return vec4(0.0);           // 0 zero; 6 and 7 unused, never written
}

void set_rgb(int i, vec3 v)
{
    switch (i) {
    case 1:  r_c0.rgb = v; break;
    case 2:  r_c1.rgb = v; break;
    case 3:  r_fog.rgb = v; break;
    case 4:  r_v0.rgb = v; break;
    case 5:  r_v1.rgb = v; break;
    case 8:  r_t0.rgb = v; break;
    case 9:  r_t1.rgb = v; break;
    case 10: r_t2.rgb = v; break;
    case 11: r_t3.rgb = v; break;
    case 12: r_r0.rgb = v; break;
    case 13: r_r1.rgb = v; break;
    case 14: r_vr.rgb = v; break;
    case 15: r_ef.rgb = v; break;
    }
}

void set_a(int i, float v)
{
    switch (i) {
    case 1:  r_c0.a = v; break;
    case 2:  r_c1.a = v; break;
    case 3:  r_fog.a = v; break;
    case 4:  r_v0.a = v; break;
    case 5:  r_v1.a = v; break;
    case 8:  r_t0.a = v; break;
    case 9:  r_t1.a = v; break;
    case 10: r_t2.a = v; break;
    case 11: r_t3.a = v; break;
    case 12: r_r0.a = v; break;
    case 13: r_r1.a = v; break;
    case 14: r_vr.a = v; break;
    case 15: r_ef.a = v; break;
    }
}

vec4 tex_reg(int s)
{
    if (s == 0) return r_t0;
    if (s == 1) return r_t1;
    if (s == 2) return r_t2;
    return r_t3;
}

int ps_word(ivec4 v[2], int i) { return v[i >> 2][i & 3]; }

// One input byte: register (bits 0-3), channel (bit 4: in the RGB portion
// alpha replicated instead of RGB; in the alpha portion alpha instead of
// blue), mapping (bits 5-7).
float ps_map(float x, int m)
{
    if (m == 0) return max(x, 0.0);                       // UNSIGNED_IDENTITY
    if (m == 1) return 1.0 - clamp(x, 0.0, 1.0);          // UNSIGNED_INVERT
    if (m == 2) return 2.0 * max(x, 0.0) - 1.0;           // EXPAND_NORMAL
    if (m == 3) return -2.0 * max(x, 0.0) + 1.0;          // EXPAND_NEGATE
    if (m == 4) return max(x, 0.0) - 0.5;                 // HALFBIAS_NORMAL
    if (m == 5) return -max(x, 0.0) + 0.5;                // HALFBIAS_NEGATE
    if (m == 6) return x;                                 // SIGNED_IDENTITY
    return -x;                                            // SIGNED_NEGATE
}

vec3 ps_in_rgb(int b)
{
    vec4 r = reg(b & 15);
    vec3 v = (b & 16) != 0 ? vec3(r.a) : r.rgb;
    int m = (b >> 5) & 7;
    return vec3(ps_map(v.r, m), ps_map(v.g, m), ps_map(v.b, m));
}

float ps_in_alpha(int b)
{
    vec4 r = reg(b & 15);
    return ps_map((b & 16) != 0 ? r.a : r.b, (b >> 5) & 7);   // channel: blue or alpha
}

vec4 ps_scale(vec4 v, int mapping)
{
    if (mapping == 1) v = v - 0.5;                        // BIAS
    else if (mapping == 2) v = v * 2.0;                   // SHIFTLEFT_1
    else if (mapping == 3) v = (v - 0.5) * 2.0;           // SHIFTLEFT_1_BIAS
    else if (mapping == 4) v = v * 4.0;                   // SHIFTLEFT_2
    else if (mapping == 6) v = v * 0.5;                   // SHIFTRIGHT_1
    return clamp(v, -1.0, 1.0);
}

vec4 ps_texture(int s, int mode)
{
    if (mode == 0) return vec4(0.0, 0.0, 0.0, 1.0);       // no texture
    if (mode == 4) return clamp(tc_for(s), 0.0, 1.0);     // PASSTHRU
    if (stage_misc[s].y == 0) return vec4(0.0, 0.0, 0.0, 1.0);
    vec4 tc = tc_for(s);
    if ((stage_misc[s].w & 8) != 0) return shadow_stage(s, tc);
    // PROJECT2D divides by q: how a vertex program hands over screen
    // coordinates (x, y, -, w) into a copy of the frame. q = 0 is an
    // output the program never wrote.
    if (mode == 1 && tc.w != 0.0) tc.xy /= tc.w;
    vec2 uv = tc.xy * tc_scale[s].xy;
    if ((mode == 6 || mode == 7) && s > 0) {
        // BUMPENVMAP(_LUM): the previous stage's texel (du, dv), through
        // this stage's bump matrix, offsets these coordinates:
        // u += M00 du + M10 dv, v += M01 du + M11 dv. The offset is in the
        // coordinates' own units, so on a linear texture (a copy of the
        // frame: texel coordinates) it is scaled with them.
        vec4 prev = tex_reg(s - 1);
        vec4 m = bump_mat[s];
        vec2 off = vec2(m.x * prev.r + m.w * prev.g, m.y * prev.r + m.z * prev.g);
        uv = (tc.xy + off) * tc_scale[s].xy;
        vec4 c = sample_tex(s, uv);
        if (mode == 7)          // luminance from the previous texel's blue
            c.rgb *= clamp(prev.b * bump_lum[s].x + bump_lum[s].y, 0.0, 1.0);
        return c;
    }
    return sample_tex(s, uv);
}

vec4 pixel_shader()
{
    int count = ps_final.z & 0xFF;
    bool mux_msb = (ps_final.z & 0x100) != 0;
    bool unique_c0 = (ps_final.z & 0x1000) != 0;
    bool unique_c1 = (ps_final.z & 0x10000) != 0;

    r_c0 = r_c1 = r_vr = r_ef = r_r1 = vec4(0.0);
    r_t0 = r_t1 = r_t2 = r_t3 = vec4(0.0);
    r_fog = vec4(fog_color.rgb, fog_factor());
    r_v0 = v_diffuse;
    r_v1 = v_specular;
    // In order: a bump-mapped stage reads the one before it.
    r_t0 = ps_texture(0, ps_final.w & 31);
    r_t1 = ps_texture(1, (ps_final.w >> 5) & 31);
    r_t2 = ps_texture(2, (ps_final.w >> 10) & 31);
    r_t3 = ps_texture(3, (ps_final.w >> 15) & 31);
    r_r0 = vec4(0.0, 0.0, 0.0, r_t0.a);                    // R0 starts with T0's alpha

    for (int i = 0; i < 8; i++) {
        if (i >= count) break;
        r_c0 = ps_c0[unique_c0 ? i : 0];
        r_c1 = ps_c1[unique_c1 ? i : 0];

        // RGB portion
        int ri = ps_word(ps_rgb_in, i), ro = ps_word(ps_rgb_out, i);
        vec3 A = ps_in_rgb(ri >> 24), B = ps_in_rgb(ri >> 16), C = ps_in_rgb(ri >> 8), D = ps_in_rgb(ri);
        int rf = ro >> 12;
        vec3 ab = (rf & 2) != 0 ? vec3(dot(A, B)) : A * B;
        vec3 cd = (rf & 1) != 0 ? vec3(dot(C, D)) : C * D;
        bool mux_r0 = mux_msb ? r_r0.a >= 0.5 : (int(r_r0.a * 255.0 + 0.5) & 1) != 0;
        vec3 sum = (rf & 4) != 0 ? (mux_r0 ? cd : ab) : ab + cd;
        int rmap = (rf >> 3) & 7;
        ab = ps_scale(vec4(ab, 0.0), rmap).rgb;
        cd = ps_scale(vec4(cd, 0.0), rmap).rgb;
        sum = ps_scale(vec4(sum, 0.0), rmap).rgb;

        // Alpha portion
        int ai = ps_word(ps_a_in, i), ao = ps_word(ps_a_out, i);
        float aA = ps_in_alpha(ai >> 24), aB = ps_in_alpha(ai >> 16);
        float aC = ps_in_alpha(ai >> 8), aD = ps_in_alpha(ai);
        int af = ao >> 12;
        float aab = aA * aB, acd = aC * aD;
        float asum = (af & 4) != 0 ? (mux_r0 ? acd : aab) : aab + acd;
        int amap = (af >> 3) & 7;
        aab = ps_scale(vec4(aab), amap).x;
        acd = ps_scale(vec4(acd), amap).x;
        asum = ps_scale(vec4(asum), amap).x;

        // Writes, after everything in the stage has been read.
        set_rgb(ro & 15, cd);
        set_rgb((ro >> 4) & 15, ab);
        set_rgb((ro >> 8) & 15, sum);
        if ((rf & 0x40) != 0) set_a(ro & 15, cd.b);         // CD blue to alpha
        if ((rf & 0x80) != 0) set_a((ro >> 4) & 15, ab.b);
        set_a(ao & 15, acd);
        set_a((ao >> 4) & 15, aab);
        set_a((ao >> 8) & 15, asum);
    }

    // Final combiner: A*B + (1-A)*C + D; alpha from G.
    if (ps_final.x == 0 && ps_final.y == 0) {
        vec4 r = clamp(r_r0, 0.0, 1.0);
        return vec4(apply_fog(r.rgb), r.a);
    }
    r_c0 = ps_fc0;
    r_c1 = ps_fc1;
    int fa = ps_final.x, fe = ps_final.y, fflags = fe & 0xFF;
    vec4 v1 = r_v1, r0 = r_r0;
    if ((fflags & 0x40) != 0) v1 = 1.0 - clamp(v1, 0.0, 1.0);   // COMPLEMENT_V1
    if ((fflags & 0x20) != 0) r0 = 1.0 - clamp(r0, 0.0, 1.0);   // COMPLEMENT_R0
    vec4 s = v1 + r0;
    if ((fflags & 0x80) != 0) s = clamp(s, 0.0, 1.0);           // CLAMP_SUM
    r_vr = vec4(s.rgb, 0.0);
    vec3 E = ps_in_rgb(fe >> 24), F = ps_in_rgb(fe >> 16);
    r_ef = vec4(E * F, 0.0);
    vec3 A = clamp(ps_in_rgb(fa >> 24), 0.0, 1.0), B = clamp(ps_in_rgb(fa >> 16), 0.0, 1.0);
    vec3 C = clamp(ps_in_rgb(fa >> 8), 0.0, 1.0), D = clamp(ps_in_rgb(fa), 0.0, 1.0);
    float G = clamp(ps_in_alpha(fe >> 8), 0.0, 1.0);
    return clamp(vec4(A * B + (1.0 - A) * C + D, G), 0.0, 1.0);
}

#endif

void main()
{
#ifdef PIXEL_SHADER
    vec4 c = pixel_shader();
    if (alpha_test.x != 0 && !alpha_pass(c.a))
        discard;
    out_color = c;
#else

    vec4 current = v_diffuse;
    vec4 temp = vec4(0.0);

    for (int s = 0; s < 4; s++) {
        int cop = color_op[s].x;
        if (cop <= 1) break;                         // D3DTOP_DISABLE
        // A stage with no texture that reads one passes the result on: it
        // read opaque white and replaced it (JSRF leaves
        // stage 1 at SELECTARG1 TEXTURE, unbound, on car shadows and
        // outlines, which turned white). A first stage modulating a missing
        // texture with the diffuse color still gives the diffuse color
        // (Orta's menu highlights).
        if (stage_misc[s].y == 0 && (uses_texture(color_op[s]) || uses_texture(alpha_op[s])))
            continue;
        vec4 tex = sample_stage(s);
        vec4 c = op(cop, arg(color_op[s].y, current, tex, temp), arg(color_op[s].z, current, tex, temp),
                    arg(color_op[s].w, current, tex, temp), current, tex);
        float a = current.a;
        int aop = alpha_op[s].x;
        if (aop > 1)
            a = op(aop, arg(alpha_op[s].y, current, tex, temp), arg(alpha_op[s].z, current, tex, temp),
                   arg(alpha_op[s].w, current, tex, temp), current, tex).a;
        vec4 result = clamp(vec4(c.rgb, a), 0.0, 1.0);
        if (stage_misc[s].x == 5) temp = result;     // D3DTA_TEMP
        else current = result;
    }
    if (alpha_test.w != 0)
        current.rgb = clamp(current.rgb + v_specular.rgb, 0.0, 1.0);
    current.rgb = apply_fog(current.rgb);
    if (alpha_test.x != 0 && !alpha_pass(current.a))
        discard;
    out_color = current;
#endif
}
