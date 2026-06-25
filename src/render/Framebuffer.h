#pragma once
#include <GL/glew.h>

class Framebuffer {
public:
    GLuint FBO;
    GLuint textureColorbuffer;
    GLuint textureDepthbuffer; // Буфер глубины как текстура для чтения в пост-процессе
    int width, height;

    Framebuffer(int w, int h);
    ~Framebuffer();

    void Bind();
    void Unbind();
    void Resize(int w, int h);
private:
    void Generate();
};