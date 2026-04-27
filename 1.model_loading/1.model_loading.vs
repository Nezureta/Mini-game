#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec2 TexCoords;
out vec3 vNormal;

void main()
{
    TexCoords = aTexCoords;
    // transform normal to world space (no non-uniform scaling assumed)
    vNormal = mat3(model) * aNormal;
    gl_Position = projection * view * model * vec4(aPos, 1.0);
}
