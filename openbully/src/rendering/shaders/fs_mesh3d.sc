$input v_color0 v_normal v_texcoord0

#include <bgfx_shader.sh>

// Single directional light (sun). bgfx has no vec3 uniform type, so the CPU
// side packs each vec3 into a Vec4; only .xyz is consumed here.
uniform vec4 u_lightDir;     // xyz: direction the light travels (into the scene)
uniform vec4 u_lightColor;   // rgb: light tint (default white), a unused
uniform vec4 u_ambientColor; // rgb: ambient tint (default gray), a unused
SAMPLER2D(s_matBase, 0);     // Track 11: per-submesh base color texture

void main()
{
	vec3 n        = normalize(v_normal);
	vec3 lightDir = normalize(u_lightDir.xyz);
	float diff    = max(dot(n, -lightDir), 0.0);

	// Albedo: sampled texture when bound, multiplied by the vertex tint
	// (checkered UV pattern keeps untextured geometry readable).
	vec3 albedo = texture2D(s_matBase, v_texcoord0.xy).rgb * v_color0.rgb;

	// Ambient floor + Lambert diffuse; texture-less meshes shade the same way.
	vec3 color   = (u_ambientColor.rgb + u_lightColor.rgb * diff) * albedo;
	gl_FragColor = vec4(color, 1.0);
}
