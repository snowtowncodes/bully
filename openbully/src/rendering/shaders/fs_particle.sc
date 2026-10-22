$input v_fogDepth

#include <bgfx_shader.sh>

// Track 34: particle fragment shader. Unlit particles with exponential fog.
// Particle color is set per-draw via u_particleColor; fog fades particles
// into the fog color at distance.

uniform vec4 u_fogDensity;    // x: fog density (0 = no fog)
uniform vec4 u_fogColor;      // rgb: fog color
uniform vec4 u_particleColor; // rgba: particle tint + alpha

void main()
{
	float fogFactor = exp(-max(v_fogDepth, 0.0) * u_fogDensity.x);
	vec3 color = mix(u_fogColor.rgb, u_particleColor.rgb, fogFactor);
	gl_FragColor = vec4(color, u_particleColor.a * fogFactor);
}
