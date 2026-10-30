#version 450

// The sky: a cube around the camera, sampled by direction. Same uniform block
// as pbr so the scene fills one layout for every model.

layout (location = 0) in vec3 inPos;

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

layout (location = 0) out vec3 outDirection;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main()
{
	outDirection = inPos;
	// Rotation only, so the sky stays put as the camera moves; z = w puts it
	// on the far plane, behind everything (the pipeline tests LEQUAL).
	vec4 clip = ubo.projectionMatrix * mat4(mat3(ubo.viewMatrix)) * vec4(inPos, 1.0);
	gl_Position = clip.xyww;
}
