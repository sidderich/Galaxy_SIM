cmake_minimum_required(VERSION 3.10)
project(GalaxieSimulation)

set(CMAKE_CXX_STANDARD 17)

# Header-Verzeichnisse angeben
include_directories(include)

# Such nach installierten Bibliotheken (GLFW und OpenGL)
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)

# Die ausfuehrbare Datei erstellen
add_executable(GalaxieSim src/main.cpp src/glad.c)

# Bibliotheken linken
target_link_libraries(GalaxieSim glfw OpenGL::GL)