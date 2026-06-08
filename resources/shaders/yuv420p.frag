#version 130

in vec2 v_texcoord;
out vec4 fragColor;

uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;

void main() {
    float y = texture2D(tex_y, v_texcoord).r;
    float u = texture2D(tex_u, v_texcoord).r - 0.5;
    float v = texture2D(tex_v, v_texcoord).r - 0.5;

    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;

    fragColor = vec4(r, g, b, 1.0);
}
