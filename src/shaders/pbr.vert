#version 450

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec3 inColor;

layout (row_major, binding = 0) uniform UBO
{
	mat4 projectionMatrix;
	mat4 modelMatrix;
	mat4 viewMatrix;
	vec4 cameraPositionRoughness;
	vec4 lightDirectionMetallic;
	vec4 lightColorAmbient;
	vec4 material;
} ubo;

layout (location = 0) out vec3 outWorldPos;
layout (location = 1) out vec3 outNormal;
layout (location = 2) out vec3 outAlbedo;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main()
{
	vec4 worldPos = ubo.modelMatrix * vec4(inPos.xyz, 1.0);
	outWorldPos = worldPos.xyz;
	outNormal = normalize(transpose(inverse(mat3(ubo.modelMatrix))) * inNormal);
	outAlbedo = inColor;
	gl_Position = ubo.projectionMatrix * ubo.viewMatrix * worldPos;
}
