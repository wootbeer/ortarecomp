#version 450
// Vertex-program draws from the vertex pre-pass (renderer.c, vout_prepass):
// each vertex's outputs were made once a game frame by the program's own
// compute shader (vsh.vert built with PDO_VSH_COMPUTE). Here they are only
// read -- the frame's, and blending (frame interpolation) also its
// partner's in the frame before, mixed as vsh.vert mixes its two runs --
// and a point list's points made squares as vsh.vert makes them.

struct VOut { vec4 pos; vec4 tc[4]; uint d0, d1; float fog, psize; };
layout(set = 0, binding = 0, std430) readonly buffer Outs { VOut cv[]; };
layout(set = 0, binding = 1, std430) readonly buffer PrevOuts { VOut pv[]; };

layout(set = 1, binding = 0, std140) uniform Draw {
    // x this frame's first output, y the partner's in the frame before
    // (-1: not blended), z the partner's vertex count, w 1 to tint.
    ivec4 base;
    // x how far from the frame before (1: this frame); y a point list (1;
    // 2 with point sprites' texture coordinates); z, w the viewport's
    // width and height in the game's pixels.
    vec4  t;
    // x, y the smallest and largest point sizes (y 0: none).
    vec4  pt;
};

layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec4 v_specular;
layout(location = 2) out vec4 v_tc0;
layout(location = 3) out vec4 v_tc1;
layout(location = 4) out vec4 v_tc2;
layout(location = 5) out vec4 v_tc3;
layout(location = 6) out float v_fog;

void main()
{
    int vi = t.y != 0.0 ? gl_VertexIndex >> 2 : gl_VertexIndex;
    VOut c = cv[base.x + vi];
    vec4 pos = c.pos, tc0 = c.tc[0], tc1 = c.tc[1], tc2 = c.tc[2], tc3 = c.tc[3];
    vec4 d0 = unpackUnorm4x8(c.d0), d1 = unpackUnorm4x8(c.d1);
    float fog = c.fog, psize = c.psize;

    if (base.y >= 0 && t.x < 1.0 && vi < base.z) {
        VOut p = pv[base.y + vi];
        float b = t.x;
        // A vertex on the two sides of the eye in the two frames stays as
        // the newest frame has it: blended, it went through w 0 and its
        // triangles covered the screen.
        pos = (p.pos.w > 0.0) == (pos.w > 0.0) ? mix(p.pos, pos, b) : pos;
        tc0 = mix(p.tc[0], tc0, b);
        tc1 = mix(p.tc[1], tc1, b);
        tc2 = mix(p.tc[2], tc2, b);
        tc3 = mix(p.tc[3], tc3, b);
        // To 8 bits again, as the CPU packs them.
        d0 = floor(mix(unpackUnorm4x8(p.d0), d0, b) * 255.0 + 0.5) / 255.0;
        d1 = floor(mix(unpackUnorm4x8(p.d1), d1, b) * 255.0 + 0.5) / 255.0;
        fog = mix(p.fog, fog, b);
        psize = mix(p.psize, psize, b);
    }
    if (t.y != 0.0) {
        // A point's square, as draw_points (d3d_hle.c) makes it: oPts
        // pixels (within the smallest and largest sizes) around it.
        int k = gl_VertexIndex & 3;
        float sx = float(k & 1), sy = float(k >> 1);
        float ps = max(psize, pt.x);
        if (pt.y > 0.0) ps = min(ps, pt.y);
        pos.x += (sx * 2.0 - 1.0) * (ps / t.z * pos.w);
        pos.y -= (sy * 2.0 - 1.0) * (ps / t.w * pos.w);
        if (t.y >= 2.0)
            tc0 = tc1 = tc2 = tc3 = vec4(sx, sy, 0.0, 1.0);
    }
    gl_Position = pos;
    v_diffuse = base.w != 0 ? vec4(1.0, 0.0, 1.0, 1.0) : d0;
    v_specular = d1;
    v_tc0 = tc0;
    v_tc1 = tc1;
    v_tc2 = tc2;
    v_tc3 = tc3;
    v_fog = fog;
}
