#version 450
// Fixed-function vertex stage. Positions arrive already in clip space
// (transformed on the CPU from the game's matrices or screen coordinates);
// colors arrive as D3DCOLOR bytes (B, G, R, A in memory).

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_diffuse;
layout(location = 2) in vec4 in_specular;
layout(location = 3) in vec4 in_tc0;
layout(location = 4) in vec4 in_tc1;
layout(location = 5) in vec4 in_tc2;
layout(location = 6) in vec4 in_tc3;
layout(location = 7) in float in_fog;
// Frame interpolation: the matching draw's positions in the previous game
// frame, and how far to blend (renderer.c, match_frames): x the blend, y 1
// when paired, z 1 to tint the draw (PDO_INTERP_TINT: it could not pair).
layout(location = 8) in vec4 in_prev_pos;
layout(set = 1, binding = 0) uniform Blend { vec4 blend; };

layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec4 v_specular;
layout(location = 2) out vec4 v_tc0;
layout(location = 3) out vec4 v_tc1;
layout(location = 4) out vec4 v_tc2;
layout(location = 5) out vec4 v_tc3;
layout(location = 6) out float v_fog;

void main()
{
    // Not across the eye: a vertex on its two sides in the
    // two frames stays as the newest frame has it (blended through w 0, its
    // triangles covered the screen).
    gl_Position = (blend.y != 0.0 && blend.x < 1.0 && (in_prev_pos.w > 0.0) == (in_pos.w > 0.0))
                ? mix(in_prev_pos, in_pos, blend.x) : in_pos;
    if (blend.w != 0.0) gl_Position.z = gl_Position.w - gl_Position.z;   // reversed depth
    v_diffuse = blend.z != 0.0 ? vec4(1.0, 0.0, 1.0, 1.0) : in_diffuse.bgra;
    v_specular = in_specular.bgra;
    v_tc0 = in_tc0;
    v_tc1 = in_tc1;
    v_tc2 = in_tc2;
    v_tc3 = in_tc3;
    v_fog = in_fog;
}
