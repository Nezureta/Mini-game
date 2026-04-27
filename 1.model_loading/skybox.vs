#version 330 core
layout (location = 0) in vec3 aPos;

out vec3 TexCoords;

uniform mat4 projection;
uniform mat4 view;

void main()
{
    TexCoords = aPos;
    vec4 pos = projection * view * vec4(aPos, 1.0);
    // Force depth = 1.0 (far plane) so the skybox renders behind everything
    // when used with glDepthFunc(GL_LEQUAL).
    gl_Position = pos.xyww;
}
