#pragma once

class Shader
{
public:
    unsigned int ID{};
    constexpr bool compiled() const { return ID != 0; }

    Shader() = default;
    Shader(const char* vertexCode, const char* fragmentCode) { compile(vertexCode, fragmentCode); };

    void compile(const char* vertexCode, const char* fragmentCode);
    void use() const;

    void setBool(const char* name, bool val) const;
    void setInt(const char* name, int val) const;
    void setFloat(const char* name, float val) const;
    void setFloat2(const char*, float val1, float val2) const;
    void setFloat4(const char* name, const float* vals) const;

private:
    static void checkCompileErrors(unsigned int shader, const char* type);
};