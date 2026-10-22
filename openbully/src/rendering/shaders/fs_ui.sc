$input v_color0 v_texcoord0

#include <bgfx_shader.sh>

// Track 20: unlit UI fragment path — texture * vertex color. Solid rects
// bind a 1x1 white texture and carry their color in v_color0; text and
// textured quads bind the font/skin atlas.
SAMPLER2D(s_texUi, 0);

void main()
{
	vec4 tex = texture2D(s_texUi, v_texcoord0.xy);
	gl_FragColor = tex * v_color0;
}
