$input a_position
$output v_fogDepth

#include <bgfx_shader.sh>

// Track 34: particle vertex shader. World-space positions, transformed by
// the view-projection matrix (model = identity). Outputs view-space depth
// for fog computation.

uniform mat4 u_mvp;

void main()
{
	vec4 viewPos = mul(u_view, vec4(a_position, 1.0));
	gl_Position = mul(u_mvp, vec4(a_position, 1.0));
	v_fogDepth = -viewPos.z;
}
