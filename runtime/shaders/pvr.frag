#version 450

// One PowerVR pixel, as pvr.c's software renderer shades it (which follows
// Flycast's fragment shader).

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_col;
layout(location = 2) in vec4 v_off;
layout(location = 3) flat in vec4 f_col;
layout(location = 4) flat in vec4 f_off;
layout(location = 5) noperspective in float v_z;

layout(set = 2, binding = 0) uniform sampler2D tex;

// mode.x flags: 1 textured, 2 gouraud, 4 offset colour, 8 use alpha,
//               16 ignore texture alpha, 32 punch-through (alpha test),
//               64 colour clamp
// mode.y shading: 0 decal, 1 modulate, 2 decal alpha, 3 modulate alpha
// mode.z fog: 0 table, 1 per vertex, 2 none, 3 table mode 2
// ref.x: punch-through alpha threshold
layout(set = 3, binding = 0) uniform Strip { ivec4 mode; vec4 ref; };

// the frame's fog and clamp registers
layout(set = 3, binding = 1) uniform Fog {
    vec4 fog_ram;             // table fog colour
    vec4 fog_vert;            // per-vertex fog colour
    vec4 clamp_min;
    vec4 clamp_max;
    vec4 fog_param;           // x: density
    vec4 fog_table[64];       // entry i: [i / 2].xy or .zw = its two values, 0..255
};

layout(location = 0) out vec4 o;

// Flycast's fog_mode2: density x 1/w, as a float with a 4-bit mantissa, picks
// one of 128 entries; the fraction blends its two values
float fog_coef(float z)
{
    float v = clamp(fog_param.x * z, 1.0, 255.9999);
    float e = floor(log2(v));
    float m = v * 16.0 / exp2(e) - 16.0;
    int idx = clamp(int(floor(m)) + int(e) * 16, 0, 127);
    float f = m - floor(m);
    vec4 t = fog_table[idx >> 1];
    vec2 hl = (idx & 1) != 0 ? t.zw : t.xy;
    return (hl.x * (1.0 - f) + hl.y * f) / 255.0;
}

void main()
{
    int flags = mode.x;
    bool gouraud = (flags & 2) != 0;
    vec4 c = gouraud ? v_col : f_col;
    vec4 off = gouraud ? v_off : f_off;
    if ((flags & 8) == 0) c.a = 1.0;
    if (mode.z == 3) c = vec4(fog_ram.rgb, fog_coef(v_z));

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
        if ((flags & 4) != 0) r.rgb += off.rgb;   // offset colour: textured polygons only
    }
    if ((flags & 64) != 0) r = clamp(r, clamp_min, clamp_max);
    if (mode.z == 0) {
        float f = fog_coef(v_z);
        r.rgb = r.rgb * (1.0 - f) + fog_ram.rgb * f;
    } else if (mode.z == 1 && (flags & 4) != 0) {
        r.rgb = r.rgb * (1.0 - off.a) + fog_vert.rgb * off.a;
    }
    if ((flags & 32) != 0) {   // punch-through: alpha test on 8-bit alpha, then opaque
        r.a = round(r.a * 255.0) / 255.0;
        if (ref.x > r.a) discard;
        r.a = 1.0;
    }
    o = clamp(r, 0.0, 1.0);
}
