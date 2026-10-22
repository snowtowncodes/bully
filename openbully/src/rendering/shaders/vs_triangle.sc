$input a_position a_texcoord0
$output v_color0
#include <bgfx_shader.sh>
uniform vec4 u_transform;
void main()
{
	gl_Position = vec4(a_position.xy * u_transform.zw + u_transform.xy, 0.0, 1.0);
	v_color0 = vec4(a_texcoord0.xy, 1.0 - a_texcoord0.x - a_texcoord0.y, 1.0);
}

