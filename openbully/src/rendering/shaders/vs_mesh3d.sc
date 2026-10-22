$input a_position a_normal a_texcoord0
$output v_color0

#include <bgfx_shader.sh>

// Model-view-projection supplied by the renderer per mesh.
uniform mat4 u_mvp;

void main()
{
	gl_Position = mul(u_mvp, vec4(a_position, 1.0));

	vec3 n = normalize(a_normal);
	vec3 lightDir = normalize(vec3(0.4, -1.0, 0.3)); // shining down (-Y is down in Gamebryo LH)
	float diff = clamp(dot(n, -lightDir), 0.0, 1.0);
	float light = 0.35 + 0.65 * diff; // ambient + diffuse

	// Checkered-ish tint from UV to break up flat surfaces.
	float tint = 0.85 + 0.15 * sin(a_texcoord0.x * 40.0) * sin(a_texcoord0.y * 40.0);

	v_color0 = vec4(light * tint, light * tint * 0.95, light * tint * 0.9, 1.0);
}
