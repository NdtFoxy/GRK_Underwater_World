#include "ShaderLoader.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>

std::string ShaderLoader::ReadFile(const char* filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Could not open file: " << filePath << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static std::string dirOf(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return (s == std::string::npos) ? std::string() : path.substr(0, s + 1);
}

// Resolve `#include "relative/path"` directives relative to the including
// file's directory. GLSL 330 has no native include, so we splice the text in
// before compilation. Only lines whose first non-blank token is `#include "`
// are transformed; every other line is passed through byte-for-byte, so no
// other shader changes behavior. Single-argument `#line` directives keep
// compiler error line numbers meaningful.
std::string ShaderLoader::ReadFileWithIncludes(const std::string& filePath, int depth) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Could not open file: " << filePath << std::endl;
        return "";
    }
    if (depth > 16) {
        std::cerr << "Shader include depth exceeded at: " << filePath << std::endl;
        return "";
    }
    const std::string dir = dirOf(filePath);
    std::stringstream out;
    std::string line;
    int lineNo = 0;
    while (std::getline(file, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t p = line.find_first_not_of(" \t");
        if (p != std::string::npos && line.compare(p, 8, "#include") == 0) {
            size_t q1 = line.find('"', p + 8);
            size_t q2 = (q1 == std::string::npos) ? std::string::npos
                                                  : line.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                std::string inc = line.substr(q1 + 1, q2 - q1 - 1);
                out << "#line 1\n"
                    << ReadFileWithIncludes(dir + inc, depth + 1) << "\n"
                    << "#line " << (lineNo + 1) << "\n";
                continue;
            }
        }
        out << line << "\n";
    }
    return out.str();
}

GLuint ShaderLoader::CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        std::cerr << "ERROR::SHADER::COMPILATION_FAILED\n" << infoLog << std::endl;
    }
    return shader;
}

GLuint ShaderLoader::CreateComputeProgram(const char* computePath) {
    std::string cCode = ReadFileWithIncludes(computePath);
    GLuint cShader = CompileShader(GL_COMPUTE_SHADER, cCode.c_str());

    GLuint program = glCreateProgram();
    glAttachShader(program, cShader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, NULL, infoLog);
        std::cerr << "ERROR::SHADER::COMPUTE::LINKING_FAILED (" << computePath << ")\n"
                  << infoLog << std::endl;
    }
    glDeleteShader(cShader);
    return program;
}

GLuint ShaderLoader::CreateProgramTess(const char* vertexPath, const char* tessCtrlPath,
                                       const char* tessEvalPath, const char* fragmentPath) {
    std::string vCode  = ReadFileWithIncludes(vertexPath);
    std::string tcCode = ReadFileWithIncludes(tessCtrlPath);
    std::string teCode = ReadFileWithIncludes(tessEvalPath);
    std::string fCode  = ReadFileWithIncludes(fragmentPath);

    GLuint vShader  = CompileShader(GL_VERTEX_SHADER, vCode.c_str());
    GLuint tcShader = CompileShader(GL_TESS_CONTROL_SHADER, tcCode.c_str());
    GLuint teShader = CompileShader(GL_TESS_EVALUATION_SHADER, teCode.c_str());
    GLuint fShader  = CompileShader(GL_FRAGMENT_SHADER, fCode.c_str());

    GLuint program = glCreateProgram();
    glAttachShader(program, vShader);
    glAttachShader(program, tcShader);
    glAttachShader(program, teShader);
    glAttachShader(program, fShader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, NULL, infoLog);
        std::cerr << "ERROR::SHADER::TESS::PROGRAM::LINKING_FAILED\n" << infoLog << std::endl;
    }
    glDeleteShader(vShader);
    glDeleteShader(tcShader);
    glDeleteShader(teShader);
    glDeleteShader(fShader);
    return program;
}

GLuint ShaderLoader::CreateProgram(const char* vertexPath, const char* fragmentPath) {
    std::string vCode = ReadFileWithIncludes(vertexPath);
    std::string fCode = ReadFileWithIncludes(fragmentPath);

    GLuint vShader = CompileShader(GL_VERTEX_SHADER, vCode.c_str());
    GLuint fShader = CompileShader(GL_FRAGMENT_SHADER, fCode.c_str());

    GLuint program = glCreateProgram();
    glAttachShader(program, vShader);
    glAttachShader(program, fShader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, NULL, infoLog);
        std::cerr << "ERROR::SHADER::PROGRAM::LINKING_FAILED\n" << infoLog << std::endl;
    }

    glDeleteShader(vShader);
    glDeleteShader(fShader);

    return program;
}