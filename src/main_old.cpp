#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <cstdlib> // Für unsetenv

// Sicherheitsfallback für PI
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

struct Particle {
    float x, y, z, mass; // Position und Masse im 'w'-Kanal
    float vx, vy, vz, pad;    // Geschwindigkeit und Padding für 16-Byte-Ausrichtung
};




void glfw_error_callback(int error, const char* description) {
    std::cerr << "GLFW Fehler (" << error << "): " << description << std::endl;
}

int main() {
    // Zwingt GLFW unter Linux/WSL, X11 statt Wayland zu nutzen
    unsetenv("WAYLAND_DISPLAY");    
    glfwSetErrorCallback(glfw_error_callback);
    
    // 1. GLFW initialisieren
    if (!glfwInit()) {
        std::cerr << "Fehler beim Initialisieren von GLFW!" << std::endl;
        return -1;
    }

    // 2. OpenGL-Version auf 4.3 Core festlegen (Wichtig für Compute Shader!)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // 3. Fenster erstellen
    GLFWwindow* window = glfwCreateWindow(800, 600, "Galaxie Simulation - Test", NULL, NULL);
    if (!window) {
        std::cerr << "Fenster konnte nicht erstellt werden!" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    // 4. GLAD laden (lädt alle OpenGL-Funktionszeiger)
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "GLAD konnte nicht initialisiert werden!" << std::endl;
        return -1;
    }

    // Ausgabe der GPU-Informationen, um zu sehen, ob deine 3060 Ti aktiv ist!
    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;
    std::cout << "GPU: " << glGetString(GL_RENDERER) << std::endl;

    // Particle-Daten vorbereiten (Beispiel mit 1000 Partikeln)
    const int NUM_PARTICLES = 10000; // Fang ruhig mit 10k an, deine 3060 Ti langweilt sich da noch!
    std::vector<Particle> particles(NUM_PARTICLES);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> distRadius(1.0f, 5.0f);
    std::uniform_real_distribution<float> distAngle(0.0f, 2.0f * M_PI);
    std::uniform_real_distribution<float> distHeight(-0.1f, 0.1f);

    for (int i = 0; i < NUM_PARTICLES; ++i) {
        float r = distRadius(gen);
        float theta = distAngle(gen);
        
        // Positionen in einer Scheibe
        particles[i].x = r * cos(theta);
        particles[i].y = distHeight(gen);
        particles[i].z = r * sin(theta);
        particles[i].mass = 1.0f; // Einheitsmasse für jeden Stern

        // Initiale Orbit-Geschwindigkeit (senkrecht zum Radius-Vektor für Kreisbahnen)
        float speed = sqrt(10.0f / r); // Simple Kepler-Näherung
        particles[i].vx = -speed * sin(theta);
        particles[i].vy = 0.0f;
        particles[i].vz = speed * cos(theta);
    }

    GLuint ssbo;
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    // Daten ins VRAM laden
    glBufferData(GL_SHADER_STORAGE_BUFFER, particles.size() * sizeof(Particle), particles.data(), GL_DYNAMIC_DRAW);
    // Den Puffer an Bindungspunkt 0 binden (wichtig für den Shader später!)
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);


    // 5. Hauptschleife (Main Loop)
    while (!glfwWindowShouldClose(window)) {
        // Hintergrundfarbe setzen (Dunkles Weltraum-Blau)
        glClearColor(0.05f, 0.05f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        glEnableClientState(GL_VERTEX_ARRAY);
        // Wir sagen OpenGL, dass die ersten 3 Floats (x,y,z) die Position sind
        glVertexPointer(3, GL_FLOAT, sizeof(Particle), (void*)0);

        // Alle Sterne als Punkte zeichnen
        glDrawArrays(GL_POINTS, 0, NUM_PARTICLES);

        glDisableClientState(GL_VERTEX_ARRAY);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);      

        // Buffer tauschen und Events verarbeiten
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwTerminate();
    return 0;
}