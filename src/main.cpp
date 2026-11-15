#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include <cstdlib>

void glfw_error_callback(int error, const char* description) {
    std::cerr << "GLFW Fehler (" << error << "): " << description << std::endl;
}

GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        std::cerr << "Shader Fehler (Typ " << type << "): " << infoLog << std::endl;
    }
    return shader;
}

int main() {
    unsetenv("WAYLAND_DISPLAY");    
    glfwSetErrorCallback(glfw_error_callback);
    
    if (!glfwInit()) {
        std::cerr << "Fehler beim Initialisieren von GLFW!" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE); 

    GLFWwindow* window = glfwCreateWindow(1920, 1440, "Supermassives Schwarzes Loch - Galaxie Fix", NULL, NULL);
    if (!window) {
        std::cerr << "Fenster konnte nicht erstellt werden!" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwFocusWindow(window); 

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "GLAD konnte nicht initialisiert werden!" << std::endl;
        return -1;
    }

    // --- 1. VERTEX SHADER (Fullscreen Quad) ---
    const char* vertexShaderSource = R"(
        #version 430 core
        out vec2 uv;
        void main() {
            uint id = gl_VertexID;
            float x = -1.0 + float((id & 1U) << 2U);
            float y = -1.0 + float((id & 2U) << 1U);
            uv = vec2(x, y);
            gl_Position = vec4(x, y, 0.0, 1.0);
        }
    )";

    // --- 2. FRAGMENT SHADER (Bugfreie Galaxie-Version) ---
    const char* fragmentShaderSource = R"(
        #version 430 core
        in vec2 uv;
        out vec4 FragColor;

        uniform vec3 camPos;
        uniform vec2 camRot;
        uniform float aspect;

        // Eine mathematische Pseudo-Zufallsfunktion für 3D-Koordinaten
        float hash3(vec3 p) {
            return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
        }

        void main() {
            float fov = 1.2; 
            vec3 rd = normalize(vec3(uv.x * aspect * fov, uv.y * fov, -1.0));
            
            // Kamera-Rotation
            float p_c = cos(camRot.x); float p_s = sin(camRot.x);
            rd = vec3(rd.x, rd.y * p_c - rd.z * p_s, rd.y * p_s + rd.z * p_c);
            float y_c = cos(camRot.y); float y_s = sin(camRot.y);
            rd = vec3(rd.x * y_c - rd.z * y_s, rd.y, rd.x * y_s + rd.z * y_c);

            vec3 rp = camPos;

            float Rs = 1.0;          // Schwarzschild-Radius
            float stepSize = 0.032;  // Schrittweite für präzise Lichtkrümmung
            vec3 totalColor = vec3(0.0);
            float totalAlpha = 0.0;
            bool hitBH = false;

            // Integrations-Schleife (Nur für physikalische Krümmung und Akkretionsscheibe)
            for (int i = 0; i < 380; i++) { 
                float r2 = dot(rp, rp);
                float r = sqrt(r2);

                // Licht fällt in den Ereignishorizont
                if (r < Rs * 1.002) {
                    hitBH = true;
                    break;
                }

                // --- RELATIVISTISCHE SCHWERKRAFT (LICHTKRÜMMUNG) ---
                vec3 toBH = -rp;
                vec3 gravity = (2.85 * Rs / (r2 * r)) * toBH; 
                rd = normalize(rd + gravity * stepSize);

                vec3 nextRp = rp + rd * stepSize;

                // --- AKKRETIONSSCHEIBE ---
                if (rp.y * nextRp.y < 0.0) {
                    float t = -rp.y / (nextRp.y - rp.y);
                    vec3 intersectP = rp + rd * stepSize * t;
                    float dist = length(intersectP);

                    float diskMin = Rs * 1.6;
                    float diskMax = Rs * 8.0;

                    if (dist >= diskMin && dist <= diskMax) {
                        float angle = atan(intersectP.z, intersectP.x);
                        float noise = sin(dist * 16.0 - angle * 5.0) * 0.4 + sin(dist * 32.0 + angle * 2.0) * 0.2 + 0.4;

                        float intensity = exp(-0.25 * (dist - diskMin)) * (0.15 + 0.85 * noise);
                        vec3 diskColor = mix(vec3(1.5, 1.3, 1.0), vec3(1.0, 0.4, 0.01), (dist - diskMin) / (diskMax - diskMin));
                        diskColor *= intensity * 6.5; 

                        float alpha = intensity * 0.88;
                        totalColor += diskColor * (1.0 - totalAlpha) * alpha;
                        totalAlpha += (1.0 - totalAlpha) * alpha;

                        if (totalAlpha > 0.99) break;
                    }
                }

                rp = nextRp;
            }

            // --- NEU & BUGFREI: HINTERGRUND-GALAXIE ERST NACH DER SCHLEIFE ANWENDEN ---
            if (!hitBH) {
                // Schräg liegende Galaxie-Ebene basierend auf der ENDGÜLTIGEN (gekrümmten) Blickrichtung rd
                float galacticPlane = abs(rd.y * 0.7 + rd.x * 0.5 - rd.z * 0.5);
                float bandDensity = exp(-3.5 * galacticPlane); 

                // Kosmischer Hintergrundnebel (Staubwolken)
                float n1 = sin(rd.x * 4.0) * cos(rd.y * 5.0) * sin(rd.z * 4.0);
                float n2 = sin(rd.x * 14.0 + rd.z * 9.0) * cos(rd.y * 11.0);
                float nebulaTex = clamp((n1 * 0.6 + n2 * 0.4) * 0.5 + 0.5, 0.0, 1.0);

                vec3 nebBaseColor = mix(vec3(0.005, 0.002, 0.03), vec3(0.02, 0.08, 0.25), bandDensity);
                vec3 nebulaGlow = vec3(0.35, 0.05, 0.45) * nebulaTex * bandDensity;
                vec3 finalNebula = nebBaseColor + nebulaGlow;

                // --- QUANTEN-STERNENFELDER (Millionen Lichtpunkte) ---
                // Ebene 1: Große helle Sterne
                vec3 grid1 = floor(rd * 200.0);
                float h1 = hash3(grid1);
                float s1 = step(0.994, h1) * 2.5;

                // Ebene 2: Mittlere dichte Sternenpopulation
                vec3 grid2 = floor(rd * 450.0);
                float h2 = hash3(grid2);
                float s2 = step(0.982, h2) * 1.2;

                // Ebene 3: Extrem dichte Mikro-Hintergrundsterne
                vec3 grid3 = floor(rd * 850.0);
                float h3 = hash3(grid3);
                float s3 = step(0.955, h3) * 0.55; 

                // Konzentration im Galaxienband verstärken
                s2 *= (0.3 + 1.7 * bandDensity);
                s3 *= (0.1 + 2.9 * bandDensity);

                vec3 starColor = vec3(s1 + s2 + s3);
                
                // Sternenfarben mischen
                vec3 starTint = mix(vec3(0.75, 0.88, 1.0), vec3(1.0, 0.75, 0.55), hash3(grid2 * 1.5));
                starColor *= starTint;

                // Gesamter Hintergrund
                vec3 galaxyBackground = finalNebula + starColor;

                // Mit dem restlichen Licht (das nicht von der Scheibe blockiert wurde) vermischen
                totalColor += galaxyBackground * (1.0 - totalAlpha);
            }

            // Post-Processing: Filigraner Kontrastausgleich
            totalColor = pow(totalColor, vec3(0.95)); 
            FragColor = vec4(totalColor, 1.0);
        }
    )";

    GLuint vertexShader = compile_shader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fragmentShader = compile_shader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    GLuint rayProgram = glCreateProgram();
    glAttachShader(rayProgram, vertexShader);
    glAttachShader(rayProgram, fragmentShader);
    glLinkProgram(rayProgram);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLuint emptyVao;
    glGenVertexArrays(1, &emptyVao);

    GLint camPosLoc = glGetUniformLocation(rayProgram, "camPos");
    GLint camRotLoc = glGetUniformLocation(rayProgram, "camRot");
    GLint aspectLoc = glGetUniformLocation(rayProgram, "aspect");

    // Startposition
    float camX = 0.0f, camY = 1.2f, camZ = 13.0f;
    float pitch = -0.12f; 
    float yaw = 0.0f;     

    while (!glfwWindowShouldClose(window)) {
        float moveSpeed = 0.08f;
        float rotSpeed  = 0.02f;

        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)    pitch += rotSpeed;
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)  pitch -= rotSpeed;
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)  yaw -= rotSpeed;
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) yaw += rotSpeed;

        float fx = sin(yaw);  float fz = -cos(yaw);
        float rx = cos(yaw);  float rz = sin(yaw);

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { camX += fx * moveSpeed; camZ += fz * moveSpeed; }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) { camX -= fx * moveSpeed; camZ -= fz * moveSpeed; }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) { camX -= rx * moveSpeed; camZ -= rz * moveSpeed; }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) { camX += rx * moveSpeed; camZ += rz * moveSpeed; }
        
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)      camY += moveSpeed;
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) camY -= moveSpeed;

        int width, height;
        glfwGetWindowSize(window, &width, &height);
        glViewport(0, 0, width, height);
        float aspect = (height > 0) ? (float)width / (float)height : 1.0f;

        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(rayProgram);
        glUniform3f(camPosLoc, camX, camY, camZ);
        glUniform2f(camRotLoc, pitch, yaw);
        glUniform1f(aspectLoc, aspect);

        glBindVertexArray(emptyVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteProgram(rayProgram);
    glDeleteVertexArrays(1, &emptyVao);
    glfwTerminate();
    return 0;
}