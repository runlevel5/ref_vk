#version 450
#extension GL_ARB_separate_shader_objects : enable

#define MAX_DLIGHTS 32

struct DynLight {
    vec3 origin;
    float _padding;
    vec4 color;     // rgb = color, a = intensity
};

layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(set = 2, binding = 0) uniform sampler2D sLightmap;
layout(set = 3, binding = 0) uniform DynLightUBO
{
    DynLight dynLights[MAX_DLIGHTS];
} dl;

layout(push_constant) uniform PushConstant
{
    layout(offset = 68) uint numDynLights;
} pc;

layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec2 texCoordLmap;
layout(location = 2) in float viewLightmaps;
layout(location = 3) in vec3 worldCoord;
layout(location = 4) in vec3 worldNormal;
layout(location = 5) flat in uint lightFlags;

layout(location = 0) out vec4 fragmentColor;

void main()
{
    vec4 color = texture(sTexture, texCoord);
    vec4 light = texture(sLightmap, texCoordLmap);

    // Per-pixel dynamic light contribution (mirrors GL3's lightmapped path):
    // distance falloff with a normal-bias to soften grazing angles, and a
    // dot(normal, lightDir) directional term. The CPU sets lightFlags to a
    // bitmask of the dynamic lights that touch this surface, so the loop
    // skips lights that don't affect this poly.
    vec3 dynamic = vec3(0.0);
    vec3 N = normalize(worldNormal);
    uint count = pc.numDynLights;
    if (count > uint(MAX_DLIGHTS))
        count = uint(MAX_DLIGHTS);

    for (uint i = 0u; i < count; ++i)
    {
        if ((lightFlags & (1u << i)) == 0u)
            continue;

        vec3 toLight = dl.dynLights[i].origin - worldCoord;
        float dist = length(toLight);
        float intensity = dl.dynLights[i].color.a;
        float fact = intensity - dist - 52.0;
        if (fact <= 0.0)
            continue;

        // Bias the light vector by the surface normal to avoid harsh
        // termination at grazing angles, then attenuate by the dot product.
        vec3 biased = toLight + N * 32.0;
        float ndotl = max(0.0, dot(N, normalize(biased)));
        fact *= ndotl * (1.0 / 256.0);

        dynamic += dl.dynLights[i].color.rgb * fact;
    }

    vec4 lit = color * (light + vec4(dynamic, 0.0));
    fragmentColor = (1.0 - viewLightmaps) * lit + viewLightmaps * light;
}
