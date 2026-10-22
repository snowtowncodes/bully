$input a_position a_texcoord0 a_color0
$output v_color0 v_texcoord0

#include <bgfx_shader.sh>

// Track 20: screen-space UI. Positions arrive in pixel coordinates with the
// origin at the top-left; the view (10) carries an orthographic projection
// via setViewTransform, so u_viewProj (identity view * ortho) maps pixels
// straight to clip space. No custom uniforms needed.
void main()
{
	gl_Position = mul(u_viewProj, vec4(a_position.xy, 0.0, 1.0));
	v_texcoord0 = a_texcoord0.xy;
	v_color0    = a_color0;
}
