#version 450

// One PowerVR pixel, as pvr.c's software renderer shades it.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_col;
layout(location = 2) in vec4 v_off;
layout(location = 3) flat in vec4 f_col;
layout(location = 4) flat in vec4 f_off;

layout(set = 2, binding = 0) uniform sampler2D tex;

// mode.x flags: 1 textured, 2 gouraud, 4 offset colour, 8 use alpha,
//               16 ignore texture alpha, 32 punch-through (alpha test)
// mode.y shading: 0 decal, 1 modulate, 2 decal alpha, 3 modulate alpha
// ref.x: punch-through alpha threshold
layout(set = 3, binding = 0) uniform Strip { ivec4 mode; vec4 ref; };

layout(location = 0) out vec4 o;

void main()
{
    int flags = mode.x;
    bool gouraud = (flags & 2) != 0;
    vec4 c = gouraud ? v_col : f_col;
    vec4 off = gouraud ? v_off : f_off;
    if ((flags & 8) == 0) c.a = 1.0;

    vec4 r = c;
    if ((flags & 1) != 0) {
        vec4 t = texture(tex, v_uv);
        if ((flags & 16) != 0) t.a = 1.0;
        switch (mode.y) {
        case 0: r = t; break;
        case 1: r = vec4(t.rgb * c.rgb, t.a); break;
        case 2: r = vec4(t.rgb * t.a + c.rgb * (1.0 - t.a), c.a); break;
        default: r = t * c; break;
        }
    }
    if ((flags & 4) != 0) r.rgb += off.rgb;
    if ((flags & 32) != 0 && r.a < ref.x) discard;
    o = clamp(r, 0.0, 1.0);
}
