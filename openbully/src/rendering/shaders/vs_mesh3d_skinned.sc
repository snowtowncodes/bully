$input a_position a_normal a_texcoord0 a_indices a_weight
$output v_color0 v_normal v_texcoord0 v_fogDepth

#include <bgfx_shader.sh>

// Track 17: skinned vertex shader. Each vertex is transformed by the weighted
// sum of up to four bone matrices from the u_bones palette (the host uploads
// kMaxSkinBones column-major mat4s; unused slots are identity).

uniform mat4 u_mvp;
uniform mat4 u_bones[64];

void main()
{
	mat4 skin = a_weight.x * u_bones[int(a_indices.x)]
	          + a_weight.y * u_bones[int(a_indices.y)]
	          + a_weight.z * u_bones[int(a_indices.z)]
	          + a_weight.w * u_bones[int(a_indices.w)];

	// Skinned position: bone palette maps bind-pose model space to the
	// current pose; u_mvp carries entity transform + view + projection.
	vec4 skinnedPos = mul(skin, vec4(a_position, 1.0));

	// Compute view-space position for fog depth (positive distance in LH).
	vec4 viewPos = mul(u_view, skinnedPos);
	v_fogDepth = -viewPos.z;

	gl_Position = mul(u_mvp, skinnedPos);

	// Normals: rotate by the skinned 3x3 (no inverse-transpose yet; bone
	// matrices are near-rigid so this stays acceptable for lighting).
	mat3 skin3 = mat3(skin[0].xyz, skin[1].xyz, skin[2].xyz);
	vec3 skinnedN = mul(skin3, a_normal);
	v_normal = mul(u_view, vec4(skinnedN, 0.0)).xyz;

	v_texcoord0 = a_texcoord0.xy;

	// Same albedo stand-in as vs_mesh3d.
	float tint = 0.85 + 0.15 * sin(a_texcoord0.x * 40.0) * sin(a_texcoord0.y * 40.0);
	v_color0 = vec4(tint, tint * 0.95, tint * 0.9, 1.0);
}
