#pragma once
#include <GL/glew.h>
#include <string>

class ShaderLoader {
public:
    static GLuint CreateProgram(const char* vertexPath, const char* fragmentPath);
    static GLuint CreateComputeProgram(const char* computePath);   // GL 4.3+ compute
    static GLuint CreateProgramTess(const char* vertexPath, const char* tessCtrlPath,
                                    const char* tessEvalPath, const char* fragmentPath); // GL 4.0+ tessellation
private:
    static std::string ReadFile(const char* filePath);
    static std::string ReadFileWithIncludes(const std::string& filePath, int depth = 0);
    static GLuint CompileShader(GLenum type, const char* source);
};