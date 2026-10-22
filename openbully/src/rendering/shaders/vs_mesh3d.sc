$input a_position a_normal a_texcoord0
$output v_color0 v_normal v_texcoord0 v_fogDepth

#include <bgfx_shader.sh>

// Model-view-projection supplied by the renderer per mesh.
uniform mat4 u_mvp;

void main()
{
	// Compute view-space position for fog depth (positive distance in LH).
	vec4 viewPos = mul(u_view, vec4(a_position, 1.0));
	v_fogDepth = -viewPos.z;

	gl_Position = mul(u_mvp, vec4(a_position, 1.0));

	// Pass the normal to the fragment shader in view space: transform by the
	// view matrix (bgfx provides u_view from setViewTransform). The model
	// transform is identity today, so u_view alone is the correct normal
	// matrix; revisit with inverse-transpose when node transforms land.
	v_normal = mul(u_view, vec4(a_normal, 0.0)).xyz;

	// Track 11: pass UVs through for the base-color texture sample.
	v_texcoord0 = a_texcoord0.xy;

	// Albedo stand-in: checkered tint from UV to break up flat surfaces.
	// Lighting itself happens per-fragment in fs_mesh3d.
	float tint = 0.85 + 0.15 * sin(a_texcoord0.x * 40.0) * sin(a_texcoord0.y * 40.0);
	v_color0 = vec4(tint, tint * 0.95, tint * 0.9, 1.0);
}
