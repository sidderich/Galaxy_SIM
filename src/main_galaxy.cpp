#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// Struktur für unsere Sterne (16-Byte Aligned für die GPU)
struct Particle {
    float x, y, z, mass;   // Entspricht vec4 im Shader (posMass)
    float vx, vy, vz, pad;  // Entspricht vec4 im Shader (velPad)
};

// Hinweis zur Struktur:
// - GPUs (und GLSL's std430) arbeiten am besten mit 16-Byte-Alignment. Deshalb
//   gruppieren wir Position+Masse und Geschwindigkeit+Padding in je 4 floats
//   (jeweils vec4 im Shader). Das macht direkten Memory-Mapping zwischen C++
//   und dem Shader möglich ohne zusätzliche Konvertierungen.
// - Das 'pad' am Ende stellt sicher, dass sizeof(Particle) ein Vielfaches von
//   16 Bytes ist, was bei Zugriffen im SSBO wichtig ist.
void glfw_error_callback(int error, const char* description) {
    std::cerr << "GLFW Fehler (" << error << "): " << description << std::endl;
}

GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    // glCreateShader: Erstellt ein Shader-Objekt im OpenGL-Kontext.
    // - 'type' bestimmt, ob es ein Vertex/Fragment/Compute/... Shader ist.
    // - Das Objekt lebt im GPU/Treiber und erhält Quelltext + Kompilat.
    glShaderSource(shader, 1, &source, NULL);
    // glShaderSource: Bindet die Quelltext-Strings an das Shader-Objekt.
    // - Mehrere Strings sind möglich; hier nutzen wir nur einen.
    glCompileShader(shader);
    // glCompileShader: Kompiliert den GLSL-Quelltext in GPU-internen Code.
    // - Fehler werden im InfoLog gemeldet (siehe glGetShaderInfoLog).
    
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

    GLFWwindow* window = glfwCreateWindow(1920, 1440, "Kosmische 3D Simulation", NULL, NULL);
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

    // --- 1. COMPUTE SHADER (Bleibt gleich für die Physik) ---
    // Compute-Shader: Läuft auf der GPU und berechnet die N-Body-Physik.
    // Warum Compute-Shader?
    // - Massive Parallelität: Jeder Partikel-Update kann für viele Partikel
    //   gleichzeitig ausgeführt werden. Das ist deutlich schneller als CPU-Loop.
    // - Direkter Zugriff auf große Buffers (SSBOs) mit effizienter Speicherbandbreite.
    // Design-Entscheidungen:
    // - local_size_x = 256: Anzahl Threads pro Workgroup; sinnvoller Kompromiss
    //   zwischen Hardwareauslastung und Register-/Shared-Memory-Nutzung.
    const char* computeShaderSource = R"(
        #version 430 core
        layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

        struct Particle {
            vec4 posMass;
            vec4 velPad;
        };

        layout(std430, binding = 0) buffer ParticleBuffer {
            Particle particles[];
        };

        uniform float dt;
        uniform int numParticles;

        void main() {
            uint id = gl_GlobalInvocationID.x;
            if (id >= numParticles) return;

            vec3 pos = particles[id].posMass.xyz;
            vec3 vel = particles[id].velPad.xyz;
            vec3 acceleration = vec3(0.0);

            float G = 0.1; 
            float softeningSq = 0.05; 

            for (int i = 0; i < numParticles; i++) {
                if (i == int(id)) continue;

                vec3 otherPos = particles[i].posMass.xyz;
                float otherMass = particles[i].posMass.w;

                vec3 dir = otherPos - pos;
                float distSq = dot(dir, dir) + softeningSq;
                float dist = sqrt(distSq);

                acceleration += G * otherMass * dir / (distSq * dist);
            }

            vel += acceleration * dt;
            pos += vel * dt;

            particles[id].posMass.xyz = pos;
            particles[id].velPad.xyz = vel;
        }
    )";

    // --- 2. VERTEX SHADER (Jetzt mit echter 3D-Kamera-Trigonometrie!) ---
    // Vertex-Shader: Transformiert Partikel-Positionen in Bildschirmraum.
    // Warum eigene Transformation statt Matrizengleichungen?
    // - Hier wollen wir Kontrolle über einfache Kamera-Rotation/Translation
    //   und eine sehr leichte, handgeschriebene Perspektivprojektion für Punkte.
    // - Die Verwendung von manuellem viewZ als W-Komponente ist ein Trick:
    //   Indem wir die W-Komponente auf die Entfernung setzen, können wir die
    //   perspektivische Division effektiv durch die GPU erledigen, sodass
    //   tiefere Punkte kleiner erscheinen und Clipping einfacher wird.
    const char* vertexShaderSource = R"(
        #version 430 core
        layout(location = 0) in vec4 aPosMass; // Position des Sterns
        layout(location = 1) in vec4 aVelPad;  // Geschwindigkeit des Sterns

        uniform vec3 camPos;
        uniform vec2 camRot; // x = Pitch (Sicht nach oben/unten), y = Yaw (Sicht nach links/rechts)
        uniform float aspect;

        out float vSpeed;
        out float vDist;

        void main() {
            // Berechne Geschwindigkeit und Abstand fürs Einfärben im Fragment Shader
            vSpeed = length(aVelPad.xyz);
            vDist = length(aPosMass.xyz);

            // 1. Translation: Verschiebe die Welt relativ zur Kamera
            vec3 p = aPosMass.xyz - camPos;

            // 2. Rotation Yaw (um die Y-Achse drehen)
            float cY = cos(-camRot.y);
            float sY = sin(-camRot.y);
            float x1 = p.x * cY - p.z * sY;
            float z1 = p.x * sY + p.z * cY;

            // 3. Rotation Pitch (um die X-Achse neigen)
            float cX = cos(-camRot.x);
            float sX = sin(-camRot.x);
            float y2 = p.y * cX - z1 * sX;
            float z2 = p.y * sX + z1 * cX;

            // In diesem Raum schaut die Kamera standardmäßig nach -Z.
            // viewZ ist die positive Tiefe vor der Kamera.
            float viewZ = -z2;

            // 4. Perspektivische Projektion (60 Grad Field of View)
            float f = 1.0 / tan(30.0 * 3.14159265 / 180.0);

            // Frustum-Clipping: Wenn der Stern hinter der Kamera ist, wirf ihn aus dem Bildschirm
            if (viewZ <= 0.1) {
                gl_Position = vec4(-999.0, -999.0, -999.0, 1.0);
            } else {
                // Perspektivische Division wird durch das Zuweisen von 'viewZ' an die W-Komponente automatisiert!
                gl_Position = vec4((x1 / aspect) * f, y2 * f, 0.0, viewZ);
            }

            // Dynamische Punktgröße: Sterne, die näher an der Kamera sind, wirken größer!
            gl_PointSize = clamp(25.0 / viewZ, 1.0, 8.0);
        }
    )";

    // --- 3. FRAGMENT SHADER (Kosmische Farben basierend auf Position & Speed) ---
    // Fragment-Shader: Färbt die Punkte basierend auf Distanz & Geschwindigkeit.
    // Kleine Anmerkung: Da wir mit GL_POINTS rendern, ist der Fragment-Shader
    // für jeden Pixel innerhalb des Punkts zuständig. Die Farbe hängt hier
    // nur von vSpeed und vDist ab — das ist kostengünstig und produziert
    // visuell ansprechende Ergebnisse ohne Beleuchtung.
    const char* fragmentShaderSource = R"(
        #version 430 core
        in float vSpeed;
        in float vDist;
        out vec4 FragColor;

        void main() {
            // Normiere den Radius (0 im Zentrum, 1 am äußeren Rand der Galaxie)
            float factor = clamp(vDist / 6.5, 0.0, 1.0);

            vec3 coreColor = vec3(1.0, 0.85, 0.5);  // Glühendes Gelb-Weiß im Kern
            vec3 armColor  = vec3(0.15, 0.4, 1.0);  // Tiefes Kosmisches Blau in den Armen

            // Lineare Interpolation zwischen Kern und Armen
            vec3 finalColor = mix(coreColor, armColor, factor);

            // Extra-Effekt: Extrem schnelle Sterne fangen an, rötlich-violett zu glühen (Akkretionsscheibe)
            finalColor += vec3(0.8, 0.1, 0.0) * clamp((vSpeed - 12.0) / 20.0, 0.0, 0.6);

            FragColor = vec4(finalColor, 1.0);
        }
    )";

    // Shader kompilieren und verknüpfen
    // Compile & Link Compute-Shader
    GLuint computeShader = compile_shader(GL_COMPUTE_SHADER, computeShaderSource);
    // glCreateProgram: Erstellt ein Programm-Objekt, das mehrere Shader-Stages
    // zusammenfasst (z.B. Vertex+Fragment oder nur ein Compute-Shader).
    GLuint computeProgram = glCreateProgram();
    // glAttachShader: Hängt das kompilierten Shader-Objekt an das Program.
    // - Beim Linken werden die Shader-Stages zusammengeführt.
    glAttachShader(computeProgram, computeShader);
    glLinkProgram(computeProgram);
    // glDeleteShader: Entfernt das Shader-Objekt. Das Programm behält die
    // verlinkte Binärform; das Shader-Objekt ist nur noch für Debugging nötig.
    glDeleteShader(computeShader);

    // Compile & Link Render-Shader (Vertex + Fragment)
    GLuint vertexShader = compile_shader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fragmentShader = compile_shader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    GLuint renderProgram = glCreateProgram();
    glAttachShader(renderProgram, vertexShader);
    glAttachShader(renderProgram, fragmentShader);
    glLinkProgram(renderProgram);
    // Nach dem Linken können die Shader-Objekte gelöscht werden.
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    // Hinweis: Shader-Compilation und -Linking sind getrennte Schritte.
    // Nach dem Linken können die Shader-Objekte gelöscht werden; das Program
    // hält die verlinkten Binärdaten intern.

    // --- 4. STERNE INITIALISIEREN ---
    const int NUM_PARTICLES = 50000;
    std::vector<Particle> particles(NUM_PARTICLES);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> distRadius(1.5f, 6.5f);
    std::uniform_real_distribution<float> distAngle(0.0f, 2.0f * M_PI);
    std::uniform_real_distribution<float> distHeight(-0.08f, 0.08f);

    // Das supermassive Schwarze Loch im Zentrum
    particles[0].x = 0.0f; particles[0].y = 0.0f; particles[0].z = 0.0f;
    particles[0].mass = 4500.0f;
    particles[0].vx = 0.0f; particles[0].vy = 0.0f; particles[0].vz = 0.0f;

    for (int i = 1; i < NUM_PARTICLES; ++i) {
        float r = distRadius(gen);
        float theta = distAngle(gen);
        
        particles[i].x = r * cos(theta);
        particles[i].y = distHeight(gen);
        particles[i].z = r * sin(theta);
        particles[i].mass = 0.5f;

        float speed = sqrt((0.1f * 4500.0f) / r); 
        particles[i].vx = -speed * sin(theta);
        particles[i].vy = 0.0f;
        particles[i].vz = speed * cos(theta);
    }

    // --- 5. GPU BUFFER & VAO SETUP ---
    GLuint vao, ssbo;
    glGenVertexArrays(1, &vao);
    // glGenBuffers: Reserviert einen Buffer-Namen (Handle) im OpenGL-Kontext.
    glGenBuffers(1, &ssbo);

    // glBindVertexArray: Aktiviert das VAO. Das VAO speichert Vertex-Attrib-
    // Bindings und Zustand für späteres Rendering.
    glBindVertexArray(vao);
    // glBindBuffer(GL_SHADER_STORAGE_BUFFER,...): Bindet unseren Buffer als
    // SSBO (Shader Storage Buffer Object), sodass der Compute-Shader darauf
    // lesend/schreibend zugreifen kann.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    // glBufferData: Allokiert GPU-Speicher und füllt ihn optional mit Daten.
    // - GL_DYNAMIC_DRAW signalisiert dem Treiber, dass wir die Daten oft
    //   aktualisieren (Compute-Shader schreibt), aber auch lesen (Render).
    glBufferData(GL_SHADER_STORAGE_BUFFER, particles.size() * sizeof(Particle), particles.data(), GL_DYNAMIC_DRAW);

    // Für das Rendering binden wir den SSBO als Array Buffer
    // Wir binden denselben Buffer auch als ARRAY_BUFFER, damit die
    // Vertex-Attribute direkt daraus gelesen werden können.
    glBindBuffer(GL_ARRAY_BUFFER, ssbo);
    
    // glVertexAttribPointer: Definiert, wie aus dem gebundenen ARRAY_BUFFER
    // ein Vertex-Attribut aufgebaut ist (size, type, stride, offset).
    // - location 0 entspricht `layout(location = 0) in vec4 aPosMass;` im Shader.
    // - stride muss sizeof(Particle) sein, damit der Treiber weiß, wie weit
    //   zum nächsten Eintrag im Array zu springen ist.
    // Layout-Location 0: Positionen (ersten 4 Floats, da vec4 im Shader)
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Particle), (void*)0);
    glEnableVertexAttribArray(0);

    // Layout-Location 1: Geschwindigkeiten (nächsten 4 Floats, Offset beachten!)
    // Layout-Location 1: Geschwindigkeit + Padding (Offset = 4 floats)
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Particle), (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // glBindBufferBase: Bindet den Buffer an ein Binding-Point (binding = 0)
    // für SSBOs. Im GLSL referenzieren wir `layout(std430, binding = 0)`.
    // Dadurch weiß der Shader, welcher GPU-Buffer mit `particles[]` verknüpft ist.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glEnable(GL_PROGRAM_POINT_SIZE);

    // Warum binden wir den SSBO auch als GL_ARRAY_BUFFER?
    // - Der Compute-Shader benötigt das Speicherlayout als Shader Storage Buffer
    //   (SSBO) für Lese-/Schreibzugriffe. Um die gleichen Daten direkt zu
    //   rendern, binden wir denselben Buffer zusätzlich als Array Buffer und
    //   weisen Vertex-Attribs mit gleichem Memory-Layout zu. Dadurch sparen wir
    //   Kopien: Die GPU schreibt in den SSBO und die Vertex-Pipeline liest
    //   direkt daraus.
    // - Wichtig: Das Layout (stride/offset) muss exakt zu `struct Particle`
    //   passen, damit die Attribute im Vertex-Shader die richtigen Werte sehen.

    // GL_PROGRAM_POINT_SIZE erlaubt dem Vertex-Shader, `gl_PointSize` zu setzen,
    // was wir für variable Sterngrößen (je nach Entfernung) nutzen.

    // Uniform-Locations abfragen
    GLint dtLocation = glGetUniformLocation(computeProgram, "dt");
    GLint numParticlesLocation = glGetUniformLocation(computeProgram, "numParticles");
    
    GLint camPosLoc = glGetUniformLocation(renderProgram, "camPos");
    GLint camRotLoc = glGetUniformLocation(renderProgram, "camRot");
    GLint aspectLoc = glGetUniformLocation(renderProgram, "aspect");

    float dt = 0.005f;

    // --- KAMERA-STARTWERTE ---
    // Wir starten leicht erhöht und blicken mit Abstand auf die Galaxie
    float camX = 0.0f, camY = 6.0f, camZ = 14.0f;
    float pitch = -0.4f; // Leicht nach unten geneigt
    float yaw = 0.0f;    // Blick nach vorne

    // --- 6. HAUPTSCHLEIFE ---
    while (!glfwWindowShouldClose(window)) {
        // --- TASTATURABFRAGE (Kamerasteuerung) ---
        float moveSpeed = 0.15f;
        float rotSpeed  = 0.025f;

        // Blickrichtung ändern mit den PFEILTASTEN
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)    pitch += rotSpeed;
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)  pitch -= rotSpeed;
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)  yaw -= rotSpeed;
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) yaw += rotSpeed;

        // Kamera-Vektoren für relative Bewegung berechnen
        float fx = sin(yaw);
        float fz = -cos(yaw);
        float rx = cos(yaw);
        float rz = sin(yaw);

        // Fortbewegen mit W, A, S, D
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { camX += fx * moveSpeed; camZ += fz * moveSpeed; }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) { camX -= fx * moveSpeed; camZ -= fz * moveSpeed; }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) { camX -= rx * moveSpeed; camZ -= rz * moveSpeed; }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) { camX += rx * moveSpeed; camZ += rz * moveSpeed; }
        
        // Höhensteuerung mit SPACE (Hoch) und LEFT_SHIFT (Runter)
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)      camY += moveSpeed;
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) camY -= moveSpeed;

        // Fenster-Seitenverhältnis dynamisch abfragen (wichtig bei Resizing)
        int width, height;
        glfwGetWindowSize(window, &width, &height);
        float aspect = (height > 0) ? (float)width / (float)height : 1.0f;

        // --- PHYSIK-PASS ---
        // --- PHYSIK-PASS ---
        // glUseProgram: Aktiviert das Programm (hier Compute-Programm). Alle
        // folgenden glUniform/glDispatchCompute-Aufrufe betreffen dieses Programm.
        glUseProgram(computeProgram);
        // glUniform*: Überträgt Uniform-Werte (Zeitdifferenz, Anzahl Partikel)
        // in den Compute-Shader.
        glUniform1f(dtLocation, dt);
        glUniform1i(numParticlesLocation, NUM_PARTICLES);
        // Anzahl Workgroups: ceil(NUM_PARTICLES / local_size_x)
        // In GLSL setzen wir local_size_x = 256, daher teilen wir hier durch 256.
        glDispatchCompute((NUM_PARTICLES + 255) / 256, 1, 1);
        // Memory barrier: Stellt sicher, dass alle Schreibzugriffe des Compute-
        // Shaders zum SSBO abgeschlossen sind, bevor die Render-Pipeline die
        // gleichen Daten liest. Ohne diese Barriere würden Rendering und
        // Compute-Shader möglicherweise gleichzeitig auf dieselben Speicherzellen
        // zugreifen, was zu Race Conditions und sichtbaren Artefakten führt.
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        // --- RENDER-PASS ---
        // --- RENDER-PASS ---
        // glClearColor + glClear: Setzen den Hintergrund; glClear leert die
        // jeweiligen Buffers (hier nur Color Buffer).
        glClearColor(0.01f, 0.01f, 0.03f, 1.0f); // Fast absolut schwarzes All
        glClear(GL_COLOR_BUFFER_BIT);

        // Render-Programm aktivieren
        glUseProgram(renderProgram);
        // Kamera-Uniforms an den Vertex Shader schicken
        glUniform3f(camPosLoc, camX, camY, camZ);
        glUniform2f(camRotLoc, pitch, yaw);
        glUniform1f(aspectLoc, aspect);

        // VAO binden und draw call ausführen
        // glDrawArrays: Zeichnet NUM_PARTICLES Punkte, wobei die Vertex-Attribs
        // aus dem aktuell gebundenen VAO/ARRAY_BUFFER gelesen werden.
        glBindVertexArray(vao);
        glDrawArrays(GL_POINTS, 0, NUM_PARTICLES);
        glBindVertexArray(0);      

        // glfwSwapBuffers: Tauscht Front- und Backbuffer (Präsentation)
        // glfwPollEvents: Verarbeitet Input/Window-Events (nicht-blockierend)
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteProgram(renderProgram);
    glDeleteProgram(computeProgram);
    glDeleteBuffers(1, &ssbo);
    glDeleteVertexArrays(1, &vao);

    glfwTerminate();
    return 0;
}