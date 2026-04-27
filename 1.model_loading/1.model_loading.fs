#version 330 core
out vec4 FragColor;

in vec2 TexCoords;
in vec3 vNormal;

uniform sampler2D texture_diffuse1;
uniform vec3 uFallbackColor;   // used if no diffuse texture was bound

void main()
{
    vec3 N = normalize(vNormal);
    vec3 L = normalize(vec3(0.4, 1.0, 0.6));
    float diff = max(dot(N, L), 0.0);

    vec3 sampled = texture(texture_diffuse1, TexCoords).rgb;
    // If sampler is unbound (or texture is fully black), use the fallback tint
    // so the geometry is still visible.
    vec3 albedo = (dot(sampled, sampled) < 0.0001) ? uFallbackColor : sampled;

    vec3 color = albedo * (0.30 + 0.70 * diff);
    FragColor = vec4(color, 1.0);
}
