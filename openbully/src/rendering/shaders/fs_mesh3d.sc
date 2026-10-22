$input v_color0 v_normal v_texcoord0 v_fogDepth

#include <bgfx_shader.sh>

// Single directional light (sun). bgfx has no vec3 uniform type, so the CPU
// side packs each vec3 into a Vec4; only .xyz is consumed here.
uniform vec4 u_lightDir;     // xyz: direction the light travels (into the scene)
uniform vec4 u_lightColor;   // rgb: light tint (default white), a unused
uniform vec4 u_ambientColor; // rgb: ambient tint (default gray), a unused
uniform vec4 u_fogDensity;   // x: fog density (0 = no fog)
uniform vec4 u_fogColor;     // rgb: fog color (gray for weather fog)
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

	// Exponential fog (Track 34): fogFactor = exp(-distance * density).
	// When density is 0, fogFactor = 1 and no fog is applied.
	float fogFactor = exp(-max(v_fogDepth, 0.0) * u_fogDensity.x);
	gl_FragColor = vec4(mix(u_fogColor.rgb, color, fogFactor), 1.0);
}
