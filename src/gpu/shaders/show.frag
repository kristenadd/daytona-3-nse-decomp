#version 450
// The frame (or the overlay, premultiplied: its alpha is kept for the blend).
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D frame;
void main() { color = texture(frame, uv); }
