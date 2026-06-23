#version 450

layout (location = 0) in vec3 inWorldPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec3 inAlbedo;

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

layout (location = 0) out vec4 outFragColor;

const float PI = 3.14159265359;

float distributionGGX(vec3 normal, vec3 halfway, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float nDotH = max(dot(normal, halfway), 0.0);
	float nDotH2 = nDotH * nDotH;
	float denom = nDotH2 * (a2 - 1.0) + 1.0;
	return a2 / max(PI * denom * denom, 0.0001);
}

float geometrySchlickGGX(float nDotV, float roughness)
{
	float r = roughness + 1.0;
	float k = (r * r) / 8.0;
	return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

float geometrySmith(vec3 normal, vec3 viewDir, vec3 lightDir, float roughness)
{
	float nDotV = max(dot(normal, viewDir), 0.0);
	float nDotL = max(dot(normal, lightDir), 0.0);
	return geometrySchlickGGX(nDotV, roughness) *
		geometrySchlickGGX(nDotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0)
{
	return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 acesTonemap(vec3 color)
{
	const float a = 2.51;
	const float b = 0.03;
	const float c = 2.43;
	const float d = 0.59;
	const float e = 0.14;
	return clamp((color * (a * color + b)) /
		(color * (c * color + d) + e), 0.0, 1.0);
}

void main()
{
	vec3 albedo = pow(clamp(inAlbedo * ubo.material.rgb, 0.0, 1.0), vec3(2.2));
	float roughness = clamp(ubo.cameraPositionRoughness.w, 0.04, 1.0);
	float metallic = clamp(ubo.lightDirectionMetallic.w, 0.0, 1.0);
	vec3 lightColor = ubo.lightColorAmbient.rgb;
	float ambientStrength = ubo.lightColorAmbient.w;

	vec3 normal = normalize(inNormal);
	vec3 viewDir = normalize(ubo.cameraPositionRoughness.xyz - inWorldPos);
	vec3 lightDir = normalize(-ubo.lightDirectionMetallic.xyz);
	vec3 halfway = normalize(viewDir + lightDir);

	vec3 f0 = mix(vec3(0.04), albedo, metallic);
	vec3 fresnel = fresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
	float normalDistribution = distributionGGX(normal, halfway, roughness);
	float geometry = geometrySmith(normal, viewDir, lightDir, roughness);

	float nDotV = max(dot(normal, viewDir), 0.0);
	float nDotL = max(dot(normal, lightDir), 0.0);
	vec3 specular = (normalDistribution * geometry * fresnel) /
		max(4.0 * nDotV * nDotL, 0.0001);
	vec3 diffuse = (vec3(1.0) - fresnel) * (1.0 - metallic) * albedo / PI;

	vec3 direct = (diffuse + specular) * lightColor * nDotL;
	vec3 ambient = albedo * ambientStrength;
	vec3 color = ambient + direct;
	color = acesTonemap(color);
	color = pow(color, vec3(1.0 / 2.2));
	outFragColor = vec4(color, ubo.material.a);
}
