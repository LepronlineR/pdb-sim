#version 450

// An equirectangular (2:1) panorama looked up by view direction, adapted from
// the Terrain Generator's sky. The texture is sRGB, so samples are linear.

layout (location = 0) in vec3 inDirection;

layout (row_major, binding = 0) uniform UBO
{
	mat4 projectionMatrix;
	mat4 modelMatrix;
	mat4 viewMatrix;
	vec4 cameraPositionRoughness;
	vec4 lightDirectionMetallic;
	vec4 lightColorAmbient;
	vec4 material;
	vec4 textureParams;  // x: exposure, y: rotation (radians)
} ubo;

layout (binding = 1) uniform sampler2D skyPanorama;

layout (location = 0) out vec4 outFragColor;

const float PI = 3.14159265358979323846;

vec3 samplePanoramaSeamless(vec2 unwrappedUv)
{
	vec2 gradientX = dFdx(unwrappedUv);
	vec2 gradientY = dFdy(unwrappedUv);

	// atan jumps by one texture width at the longitude seam. Remove the jump
	// from the gradients so mip selection does not draw a line there.
	gradientX.x -= round(gradientX.x);
	gradientY.x -= round(gradientY.x);

	vec2 panoramaUv = vec2(fract(unwrappedUv.x), unwrappedUv.y);
	vec3 primary = textureGrad(skyPanorama, panoramaUv, gradientX, gradientY).rgb;

	// The panorama's left and right edges do not match exactly: crossfade a
	// narrow band with its mirror so both sides meet at the same color.
	float seamDistance = min(panoramaUv.x, 1.0 - panoramaUv.x);
	float seamBlend = 1.0 - smoothstep(0.0, 0.035, seamDistance);
	vec2 mirroredUv = vec2(1.0 - panoramaUv.x, panoramaUv.y);
	vec3 mirrored = textureGrad(skyPanorama, mirroredUv,
		vec2(-gradientX.x, gradientX.y), vec2(-gradientY.x, gradientY.y)).rgb;

	return mix(primary, 0.5 * (primary + mirrored), seamBlend);
}

void main()
{
	vec3 direction = normalize(inDirection);
	float longitude = atan(direction.z, direction.x) + ubo.textureParams.y;
	// The ground is a finite slab, so the horizon is lowered a little to keep
	// the cloud band in view past its edge; below that the sky fades to haze.
	float skyElevation = clamp((direction.y + 0.2) / 1.2, 0.0, 1.0);
	vec2 panoramaUv = vec2(longitude / (2.0 * PI) + 0.5, acos(skyElevation) / PI);

	vec3 panoramaColor = samplePanoramaSeamless(panoramaUv);
	vec3 lowerAtmosphere = mix(vec3(0.20, 0.27, 0.33), vec3(0.55, 0.66, 0.75),
		smoothstep(-1.0, -0.2, direction.y));
	float panoramaWeight = smoothstep(-0.3, -0.15, direction.y);
	vec3 skyColor = mix(lowerAtmosphere, panoramaColor, panoramaWeight);
	skyColor *= max(ubo.textureParams.x, 0.0);

	// Linear to the display, like pbr.frag.
	outFragColor = vec4(pow(clamp(skyColor, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
