#include "Shader.h"
#include <glad/glad.h>

#include <cstring>
#include <fstream>
#include <iostream>

void Shader::compile(const char* vertexCode, const char* fragmentCode)
{
    const unsigned vertex { glCreateShader(GL_VERTEX_SHADER) };
    glShaderSource(vertex, 1, &vertexCode, nullptr);
    glCompileShader(vertex);
    checkCompileErrors(vertex, "VERTEX");

    const unsigned fragment { glCreateShader(GL_FRAGMENT_SHADER) };
    glShaderSource(fragment, 1, &fragmentCode, nullptr);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT");

    ID = glCreateProgram();
    glAttachShader(ID, vertex);
    glAttachShader(ID, fragment);
    glLinkProgram(ID);
    checkCompileErrors(ID, "PROGRAM");

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

void Shader::use() const
{
    glUseProgram(ID);
}

void Shader::setBool(const char* name, bool val) const
{
    glUniform1i(glGetUniformLocation(ID, name), static_cast<int>(val));
}
void Shader::setInt(const char* name, int val) const
{
    glUniform1i(glGetUniformLocation(ID, name), val);
}
void Shader::setFloat(const char* name, float val) const
{
    glUniform1f(glGetUniformLocation(ID, name), val);
}
void Shader::setFloat2(const char* name, float val1, float val2) const
{
    glUniform2f(glGetUniformLocation(ID, name), val1, val2);
}
void Shader::setFloat4(const char* name, const float* vals) const
{
    glUniform4fv(glGetUniformLocation(ID, name), 1, vals);
}

void Shader::checkCompileErrors(unsigned int shader, const char* type)
{
    int success;
    char infoLog[1024];
    if (strcmp(type, "PROGRAM") != 0)
    {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
            std::cout << "ERROR::SHADER_COMPILATION_ERROR of type: " << type << "\n" << infoLog << "\n -- --------------------------------------------------- -- " << std::endl;
        }
    }
    else
    {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success)
        {
            glGetProgramInfoLog(shader, 1024, nullptr, infoLog);
            std::cout << "ERROR::PROGRAM_LINKING_ERROR of type: " << type << "\n" << infoLog << "\n -- --------------------------------------------------- -- " << std::endl;
        }
    }
}
