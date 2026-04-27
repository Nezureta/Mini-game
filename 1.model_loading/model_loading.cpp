// =====================================================================
// Endless Runner — DEBUG BUILD
// Renders reference markers (cubes) every 10 units forward + lane walls,
// so SOMETHING is always visible even if the .glb models fail to load.
// Prints heavy diagnostics to the console.
// =====================================================================
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <learnopengl/filesystem.h>
#include <learnopengl/shader_m.h>
#include <learnopengl/model.h>

#include <iostream>
#include <vector>
#include <random>
#include <algorithm>
#include <cmath>
#include <limits>

void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void processInput(GLFWwindow *window);
void debugPrintModelBounds(const std::string& name, Model& m);
unsigned int createReferenceCubeVAO();   // pos+normal+texcoord (matches Mesh layout)
unsigned int createSkyboxVAO();          // positions only (used by skybox shader)
unsigned int loadCubemap(const std::vector<std::string>& faces);
unsigned int buildShaderFromSource(const char* vsSrc, const char* fsSrc);

const unsigned int SCR_WIDTH  = 1024;
const unsigned int SCR_HEIGHT = 768;

glm::vec3 playerPos(0.0f, 0.0f, 0.0f);
const float PLAYER_FORWARD_SPEED = 18.0f;
const float PLAYER_LATERAL_SPEED = 12.0f;
const float LANE_LIMIT           = 6.0f;

// Tunable display scale for the shuttle.
// Native shuttle extent is (4.3, 2.8, 9.4); at 0.15 the visible size becomes
// roughly 1.4 units across — matches the collider (PLAYER_COLLIDER_HALF*2).
const float PLAYER_SCALE         = 0.15f;
const float PLAYER_COLLIDER_HALF = 0.7f;

struct Asteroid {
    glm::vec3 pos;
    float     radius;
    float     visualScale;
    glm::vec3 spinAxis;
    float     spinSpeed;
    float     spinAngle;
};
std::vector<Asteroid> asteroids;

float spawnTimer    = 0.0f;
float spawnInterval = 0.45f;
const float SPAWN_AHEAD = 90.0f;
// Asteroid model scale per "unit of collision radius". Same logic as PLAYER_SCALE
// — Sketchfab asteroid is huge in its native units so we shrink ~100×.
const float ASTEROID_VISUAL_BASE = 0.01f;

bool  gameOver = false;
float score    = 0.0f;
float deltaTime = 0.0f;
float lastFrame = 0.0f;

std::mt19937 rng{ std::random_device{}() };
std::uniform_real_distribution<float> distX(-LANE_LIMIT, LANE_LIMIT);
std::uniform_real_distribution<float> distR(0.6f, 1.3f);
std::uniform_real_distribution<float> distSpin(-1.5f, 1.5f);
std::uniform_real_distribution<float> distAxis(-1.0f, 1.0f);

bool sphereAABBCollision(const glm::vec3& boxCenter, float boxHalf,
                         const glm::vec3& sphereCenter, float sphereR)
{
    float cx = std::max(boxCenter.x - boxHalf, std::min(sphereCenter.x, boxCenter.x + boxHalf));
    float cy = std::max(boxCenter.y - boxHalf, std::min(sphereCenter.y, boxCenter.y + boxHalf));
    float cz = std::max(boxCenter.z - boxHalf, std::min(sphereCenter.z, boxCenter.z + boxHalf));
    float dx = cx - sphereCenter.x;
    float dy = cy - sphereCenter.y;
    float dz = cz - sphereCenter.z;
    return (dx*dx + dy*dy + dz*dz) < (sphereR * sphereR);
}

int main()
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT,
                                          "Dodging Asteroid", NULL, NULL);
    if (!window) { std::cout << "Failed to create GLFW window\n"; glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Failed to initialize GLAD\n"; return -1;
    }

    // GL info — confirms the window/context exists
    std::cout << "GL Vendor:   " << glGetString(GL_VENDOR)   << "\n";
    std::cout << "GL Renderer: " << glGetString(GL_RENDERER) << "\n";
    std::cout << "GL Version:  " << glGetString(GL_VERSION)  << "\n";

    stbi_set_flip_vertically_on_load(true);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL); // needed for the skybox depth=1.0 trick

    Shader shader("1.model_loading.vs", "1.model_loading.fs");
    if (shader.ID == 0) {
        std::cout << "!! Main shader failed to compile/link. Check console for details above.\n";
    } else {
        std::cout << "Main shader program ID = " << shader.ID << "\n";
    }

    // Skybox shader compiled directly from inline source — avoids depending on
    // CMake having re-globbed for skybox.vs / skybox.fs since those were added
    // after the project was last configured.
    const char* SKYBOX_VS = R"GLSL(
#version 330 core
layout (location = 0) in vec3 aPos;
out vec3 TexCoords;
uniform mat4 projection;
uniform mat4 view;
void main() {
    TexCoords = aPos;
    vec4 pos = projection * view * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
)GLSL";
    const char* SKYBOX_FS = R"GLSL(
#version 330 core
out vec4 FragColor;
in vec3 TexCoords;
uniform samplerCube skybox;
void main() {
    FragColor = texture(skybox, TexCoords);
}
)GLSL";
    unsigned int skyboxProgram = buildShaderFromSource(SKYBOX_VS, SKYBOX_FS);
    std::cout << "Skybox shader program ID = " << skyboxProgram << "\n";

    // Reference cube — positions+normals+texcoords, matching Mesh's vertex layout.
    unsigned int refCubeVAO = createReferenceCubeVAO();
    unsigned int skyboxVAO  = createSkyboxVAO();

    // Load galaxy cubemap. Cubemaps follow OpenGL's texture coordinate origin
    // (top-left of each face), so we DON'T flip vertically when loading these.
    stbi_set_flip_vertically_on_load(false);
    std::vector<std::string> galaxyFaces = {
        FileSystem::getPath("resources/textures/galaxy/galaxy+X.tga"), // +X (right)
        FileSystem::getPath("resources/textures/galaxy/galaxy-X.tga"), // -X (left)
        FileSystem::getPath("resources/textures/galaxy/galaxy+Y.tga"), // +Y (top)
        FileSystem::getPath("resources/textures/galaxy/galaxy-Y.tga"), // -Y (bottom)
        FileSystem::getPath("resources/textures/galaxy/galaxy+Z.tga"), // +Z (back)
        FileSystem::getPath("resources/textures/galaxy/galaxy-Z.tga"), // -Z (front)
    };
    unsigned int galaxyCubemap = loadCubemap(galaxyFaces);
    stbi_set_flip_vertically_on_load(true); // restore for any subsequent model loads
    if (skyboxProgram) {
        glUseProgram(skyboxProgram);
        glUniform1i(glGetUniformLocation(skyboxProgram, "skybox"), 0);
    }

    std::cout << "Loading shuttle (.obj)...\n";
    Model shipModel(FileSystem::getPath("resources/objects/shuttle/shuttle.obj"));
    debugPrintModelBounds("shuttle", shipModel);

    std::cout << "Loading asteroid (.obj)...\n";
    Model asteroidModel(FileSystem::getPath("resources/objects/asteroid/asteroid.obj"));
    debugPrintModelBounds("asteroid", asteroidModel);

    std::cout << "\nControls: A/D or LEFT/RIGHT to dodge, ESC to quit, R to restart.\n";
    std::cout << "If you see colored reference cubes scrolling past you, rendering works.\n\n";

    int   frame = 0;
    float debugTimer = 0.0f;

    while (!glfwWindowShouldClose(window))
    {
        float currentFrame = static_cast<float>(glfwGetTime());
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        if (deltaTime > 0.05f) deltaTime = 0.05f;
        ++frame;

        processInput(window);

        if (!gameOver) {
            playerPos.z -= PLAYER_FORWARD_SPEED * deltaTime;
            score       += deltaTime * 10.0f;

            spawnTimer += deltaTime;
            if (spawnTimer >= spawnInterval) {
                spawnTimer = 0.0f;
                Asteroid a;
                a.radius      = distR(rng);
                a.visualScale = a.radius * ASTEROID_VISUAL_BASE;
                a.pos         = glm::vec3(distX(rng), 0.0f, playerPos.z - SPAWN_AHEAD);
                a.spinAxis    = glm::normalize(glm::vec3(distAxis(rng), distAxis(rng), distAxis(rng)) + glm::vec3(0.001f));
                a.spinSpeed   = distSpin(rng);
                a.spinAngle   = 0.0f;
                asteroids.push_back(a);
            }
            for (auto& a : asteroids) a.spinAngle += a.spinSpeed * deltaTime;
            asteroids.erase(std::remove_if(asteroids.begin(), asteroids.end(),
                [](const Asteroid& a){ return a.pos.z > playerPos.z + 8.0f; }),
                asteroids.end());

            for (const auto& a : asteroids) {
                if (sphereAABBCollision(playerPos, PLAYER_COLLIDER_HALF, a.pos, a.radius)) {
                    gameOver = true;
                    std::cout << "=== GAME OVER === score: " << (int)score << " (R to restart)\n";
                    break;
                }
            }
        }

        if (gameOver && glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
            gameOver = false; playerPos = glm::vec3(0.0f); asteroids.clear();
            score = 0.0f; spawnTimer = 0.0f;
        }

        // periodic debug print
        debugTimer += deltaTime;
        if (debugTimer > 1.0f) {
            debugTimer = 0.0f;
            std::cout << "[t=" << (int)glfwGetTime() << "s frame=" << frame
                      << "] playerPos=(" << playerPos.x << "," << playerPos.y << "," << playerPos.z
                      << ") asteroids=" << asteroids.size()
                      << " gameOver=" << gameOver << "\n";
        }

        // -------- render --------
        // Bright dark teal so it CANNOT be confused with black.
        glClearColor(0.05f, 0.20f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        shader.use();

        glm::vec3 camOffset(0.0f, 3.0f, 8.0f);
        glm::vec3 camPos     = playerPos + camOffset;
        glm::vec3 lookTarget = playerPos + glm::vec3(0.0f, 0.0f, -6.0f);
        glm::mat4 view       = glm::lookAt(camPos, lookTarget, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 projection = glm::perspective(glm::radians(60.0f),
                                                (float)SCR_WIDTH / (float)SCR_HEIGHT,
                                                0.1f, 500.0f);
        shader.setMat4("view", view);
        shader.setMat4("projection", projection);

        glBindVertexArray(refCubeVAO);

        // 1) Lane-edge yellow lines on the floor — thin flat strips that scroll
        //    past as the player flies forward, marking both lane boundaries.
        for (int i = 0; i < 30; ++i) {
            float zOff = std::floor(playerPos.z / 10.0f) * 10.0f - i * 10.0f + 100.0f;
            for (int side = 0; side < 2; ++side) {
                float xPos = (side == 0) ? -LANE_LIMIT : LANE_LIMIT;
                glm::mat4 m = glm::translate(glm::mat4(1.0f),
                    glm::vec3(xPos, -0.5f, zOff));
                // 0.2 wide, 0.04 tall (thin strip), 9.5 long (≈ continuous between segments)
                m = glm::scale(m, glm::vec3(0.20f, 0.04f, 9.5f));
                shader.setMat4("model", m);
                shader.setVec3("uFallbackColor", glm::vec3(1.0f, 0.85f, 0.20f)); // yellow
                glDrawArrays(GL_TRIANGLES, 0, 36);
            }
        }

        // -------- Player: real shuttle if loaded, fallback cube otherwise.
        if (!shipModel.meshes.empty()) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), playerPos);
            m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            m = glm::scale(m, glm::vec3(PLAYER_SCALE));
            shader.setMat4("model", m);
            shader.setVec3("uFallbackColor", glm::vec3(0.30f, 0.75f, 1.00f));
            shipModel.Draw(shader);
        } else {
            glBindVertexArray(refCubeVAO);
            glm::mat4 m = glm::translate(glm::mat4(1.0f), playerPos);
            m = glm::scale(m, glm::vec3(0.8f));
            shader.setMat4("model", m);
            shader.setVec3("uFallbackColor", glm::vec3(0.20f, 1.00f, 0.30f));
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }

        // -------- Asteroids: real model if loaded, fallback cube otherwise.
        for (const auto& a : asteroids) {
            if (!asteroidModel.meshes.empty()) {
                glm::mat4 m = glm::translate(glm::mat4(1.0f), a.pos);
                m = glm::rotate(m, a.spinAngle, a.spinAxis);
                m = glm::scale(m, glm::vec3(a.visualScale));
                shader.setMat4("model", m);
                shader.setVec3("uFallbackColor", glm::vec3(0.65f, 0.50f, 0.35f));
                asteroidModel.Draw(shader);
            } else {
                glBindVertexArray(refCubeVAO);
                glm::mat4 m = glm::translate(glm::mat4(1.0f), a.pos);
                m = glm::rotate(m, a.spinAngle, a.spinAxis);
                m = glm::scale(m, glm::vec3(a.radius));
                shader.setMat4("model", m);
                shader.setVec3("uFallbackColor", glm::vec3(1.00f, 0.55f, 0.10f));
                glDrawArrays(GL_TRIANGLES, 0, 36);
            }
        }

        // -------- Skybox last, with depth=LEQUAL trick so it fills only the
        //          pixels that no foreground geometry has written to.
        if (galaxyCubemap != 0 && skyboxProgram != 0) {
            glDepthFunc(GL_LEQUAL);
            glUseProgram(skyboxProgram);
            // Strip translation from the view matrix so the skybox is always
            // centered on the camera (i.e. infinite distance).
            glm::mat4 skyboxView = glm::mat4(glm::mat3(view));
            glUniformMatrix4fv(glGetUniformLocation(skyboxProgram, "view"),
                               1, GL_FALSE, glm::value_ptr(skyboxView));
            glUniformMatrix4fv(glGetUniformLocation(skyboxProgram, "projection"),
                               1, GL_FALSE, glm::value_ptr(projection));
            glBindVertexArray(skyboxVAO);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_CUBE_MAP, galaxyCubemap);
            glDrawArrays(GL_TRIANGLES, 0, 36);
            glBindVertexArray(0);
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwTerminate();
    return 0;
}

void processInput(GLFWwindow *window)
{
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
    if (gameOver) return;

    bool moved = false;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) {
        playerPos.x -= PLAYER_LATERAL_SPEED * deltaTime; moved = true;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) {
        playerPos.x += PLAYER_LATERAL_SPEED * deltaTime; moved = true;
    }
    if (playerPos.x < -LANE_LIMIT) playerPos.x = -LANE_LIMIT;
    if (playerPos.x >  LANE_LIMIT) playerPos.x =  LANE_LIMIT;
    (void)moved;
}

void framebuffer_size_callback(GLFWwindow*, int width, int height) { glViewport(0, 0, width, height); }

// Build a unit cube with positions (loc 0), normals (loc 1), texcoords (loc 2).
// Layout matches the Mesh class so the same shader works for cube + .glb meshes.
unsigned int createReferenceCubeVAO()
{
    // 36 verts; each row: pos.xyz, n.xyz, uv.xy
    float v[] = {
        // back face (-Z), normal (0,0,-1)
        -0.5f,-0.5f,-0.5f,  0.0f,0.0f,-1.0f,  0.0f,0.0f,
         0.5f,-0.5f,-0.5f,  0.0f,0.0f,-1.0f,  1.0f,0.0f,
         0.5f, 0.5f,-0.5f,  0.0f,0.0f,-1.0f,  1.0f,1.0f,
         0.5f, 0.5f,-0.5f,  0.0f,0.0f,-1.0f,  1.0f,1.0f,
        -0.5f, 0.5f,-0.5f,  0.0f,0.0f,-1.0f,  0.0f,1.0f,
        -0.5f,-0.5f,-0.5f,  0.0f,0.0f,-1.0f,  0.0f,0.0f,
        // front face (+Z), normal (0,0,1)
        -0.5f,-0.5f, 0.5f,  0.0f,0.0f, 1.0f,  0.0f,0.0f,
         0.5f,-0.5f, 0.5f,  0.0f,0.0f, 1.0f,  1.0f,0.0f,
         0.5f, 0.5f, 0.5f,  0.0f,0.0f, 1.0f,  1.0f,1.0f,
         0.5f, 0.5f, 0.5f,  0.0f,0.0f, 1.0f,  1.0f,1.0f,
        -0.5f, 0.5f, 0.5f,  0.0f,0.0f, 1.0f,  0.0f,1.0f,
        -0.5f,-0.5f, 0.5f,  0.0f,0.0f, 1.0f,  0.0f,0.0f,
        // left face (-X), normal (-1,0,0)
        -0.5f, 0.5f, 0.5f, -1.0f,0.0f, 0.0f,  1.0f,0.0f,
        -0.5f, 0.5f,-0.5f, -1.0f,0.0f, 0.0f,  1.0f,1.0f,
        -0.5f,-0.5f,-0.5f, -1.0f,0.0f, 0.0f,  0.0f,1.0f,
        -0.5f,-0.5f,-0.5f, -1.0f,0.0f, 0.0f,  0.0f,1.0f,
        -0.5f,-0.5f, 0.5f, -1.0f,0.0f, 0.0f,  0.0f,0.0f,
        -0.5f, 0.5f, 0.5f, -1.0f,0.0f, 0.0f,  1.0f,0.0f,
        // right face (+X), normal (1,0,0)
         0.5f, 0.5f, 0.5f,  1.0f,0.0f, 0.0f,  1.0f,0.0f,
         0.5f, 0.5f,-0.5f,  1.0f,0.0f, 0.0f,  1.0f,1.0f,
         0.5f,-0.5f,-0.5f,  1.0f,0.0f, 0.0f,  0.0f,1.0f,
         0.5f,-0.5f,-0.5f,  1.0f,0.0f, 0.0f,  0.0f,1.0f,
         0.5f,-0.5f, 0.5f,  1.0f,0.0f, 0.0f,  0.0f,0.0f,
         0.5f, 0.5f, 0.5f,  1.0f,0.0f, 0.0f,  1.0f,0.0f,
        // bottom (-Y), normal (0,-1,0)
        -0.5f,-0.5f,-0.5f,  0.0f,-1.0f,0.0f,  0.0f,1.0f,
         0.5f,-0.5f,-0.5f,  0.0f,-1.0f,0.0f,  1.0f,1.0f,
         0.5f,-0.5f, 0.5f,  0.0f,-1.0f,0.0f,  1.0f,0.0f,
         0.5f,-0.5f, 0.5f,  0.0f,-1.0f,0.0f,  1.0f,0.0f,
        -0.5f,-0.5f, 0.5f,  0.0f,-1.0f,0.0f,  0.0f,0.0f,
        -0.5f,-0.5f,-0.5f,  0.0f,-1.0f,0.0f,  0.0f,1.0f,
        // top (+Y), normal (0,1,0)
        -0.5f, 0.5f,-0.5f,  0.0f, 1.0f,0.0f,  0.0f,1.0f,
         0.5f, 0.5f,-0.5f,  0.0f, 1.0f,0.0f,  1.0f,1.0f,
         0.5f, 0.5f, 0.5f,  0.0f, 1.0f,0.0f,  1.0f,0.0f,
         0.5f, 0.5f, 0.5f,  0.0f, 1.0f,0.0f,  1.0f,0.0f,
        -0.5f, 0.5f, 0.5f,  0.0f, 1.0f,0.0f,  0.0f,0.0f,
        -0.5f, 0.5f,-0.5f,  0.0f, 1.0f,0.0f,  0.0f,1.0f,
    };
    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    GLsizei stride = 8 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);                        // pos
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));      // normal
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));      // uv
    return VAO;
}

// Compile + link a vertex+fragment shader pair from in-memory source strings.
// Returns 0 on failure (and prints the GL info log).
unsigned int buildShaderFromSource(const char* vsSrc, const char* fsSrc)
{
    auto compile = [](GLenum type, const char* src) -> unsigned int {
        unsigned int s = glCreateShader(type);
        glShaderSource(s, 1, &src, NULL);
        glCompileShader(s);
        int ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(s, sizeof(log), NULL, log);
            std::cout << "Shader compile error ("
                      << (type == GL_VERTEX_SHADER ? "vs" : "fs") << "): " << log << "\n";
            glDeleteShader(s);
            return 0;
        }
        return s;
    };

    unsigned int vs = compile(GL_VERTEX_SHADER, vsSrc);
    unsigned int fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    unsigned int p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    int ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        std::cout << "Shader link error: " << log << "\n";
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

// Skybox cube — positions only (the skybox shader uses position as TexCoords).
unsigned int createSkyboxVAO()
{
    float v[] = {
        -1.0f,  1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,   1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,  -1.0f, -1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,

         1.0f, -1.0f, -1.0f,   1.0f, -1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,   1.0f,  1.0f, -1.0f,   1.0f, -1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,   1.0f, -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,

        -1.0f,  1.0f, -1.0f,   1.0f,  1.0f, -1.0f,   1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f
    };
    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    return VAO;
}

// Load a cubemap from 6 face image paths, in OpenGL order: +X, -X, +Y, -Y, +Z, -Z.
// Returns 0 if any face failed (caller can detect and skip rendering).
unsigned int loadCubemap(const std::vector<std::string>& faces)
{
    unsigned int textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);

    int width = 0, height = 0, nrChannels = 0;
    int loaded = 0;
    for (unsigned int i = 0; i < faces.size(); ++i) {
        unsigned char* data = stbi_load(faces[i].c_str(), &width, &height, &nrChannels, 0);
        if (data) {
            GLenum fmt = (nrChannels == 4) ? GL_RGBA : GL_RGB;
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, fmt, width, height, 0,
                         fmt, GL_UNSIGNED_BYTE, data);
            stbi_image_free(data);
            ++loaded;
        } else {
            std::cout << "Cubemap face failed to load: " << faces[i] << "\n";
        }
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R,     GL_CLAMP_TO_EDGE);

    std::cout << "  [galaxy cubemap] " << loaded << "/6 faces loaded ("
              << width << "x" << height << ")\n";
    return (loaded == 6) ? textureID : 0;
}

void debugPrintModelBounds(const std::string& name, Model& m)
{
    glm::vec3 mn( std::numeric_limits<float>::max());
    glm::vec3 mx(-std::numeric_limits<float>::max());
    size_t totalVerts = 0, totalTextures = 0;
    for (auto& mesh : m.meshes) {
        totalVerts    += mesh.vertices.size();
        totalTextures += mesh.textures.size();
        for (auto& v : mesh.vertices) {
            mn = glm::min(mn, v.Position);
            mx = glm::max(mx, v.Position);
        }
    }
    glm::vec3 ext = mx - mn;
    std::cout << "  [" << name << "] meshes=" << m.meshes.size()
              << "  verts=" << totalVerts
              << "  textures=" << totalTextures;
    if (!m.meshes.empty())
        std::cout << "  extent=(" << ext.x << "," << ext.y << "," << ext.z << ")";
    std::cout << "\n";
}
