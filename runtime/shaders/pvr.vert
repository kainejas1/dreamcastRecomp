#version 450

// One PowerVR vertex: screen pixels and 1/w as the TA got them.

layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 col;
layout(location = 3) in vec4 off;

// x, y: 2 / render width and height in pixels; z: depth scale (a power of two)
layout(set = 1, binding = 0) uniform Screen { vec4 screen; };

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_col;
layout(location = 2) out vec4 v_off;
// flat shading takes the colour of the triangle's last vertex; gpu.c puts that
// vertex first in each triangle, where the GPU takes flat values from
layout(location = 3) flat out vec4 f_col;
layout(location = 4) flat out vec4 f_off;
// 1/w, linear on screen as the PVR's own: fog reads it
layout(location = 5) noperspective out float v_z;

void main()
{
    // pos.z is 1/w. With w back in clip space the GPU clips in 3D and
    // interpolates in perspective, as the PVR does; a constant clip z makes the
    // depth it stores screen.z / w, that is 1/w scaled into 0..1, linear on
    // screen like the PVR's own depth
    float w = 1.0 / pos.z;
    vec2 ndc = vec2(pos.x * screen.x - 1.0, 1.0 - pos.y * screen.y);
    gl_Position = vec4(ndc * w, screen.z, w);
    v_uv = uv;
    v_col = col;
    v_off = off;
    f_col = col;
    f_off = off;
    v_z = pos.z;
}
