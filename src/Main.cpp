#include <ImGUI/imgui.h>
#include <ImGUI/imgui_impl_glfw.h>
#include <ImGUI/imgui_impl_opengl3.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <nfd.hpp>

#include <array>
#include <map>
#include <string_view>
#include <sstream>
#include <iostream>   
#include <filesystem>
#include <thread>

#include "Shader.h"
#include "resources.h"
#include "ChipInterpretCore.h"
#include "ChipCachedCore.h"
#include "ChipJITCore.h"

constexpr const char* APP_NAME { "MegaJIT-8" };

ChipState s{};

ChipInterpretCore chipInterpretCore { s };
ChipCachedCore chipCachedCore { s };
ChipJITCore chipJITCore { s };
ChipCore* chipCore { &chipJITCore };

enum class CoreType
{
    Interpret,
    Cached,
    JIT
};

CoreType currentCore()
{
    if (chipCore == &chipInterpretCore)
        return CoreType::Interpret;
    if (chipCore == &chipCachedCore)
        return CoreType::Cached;
    if (chipCore == &chipJITCore)
        return CoreType::JIT;

    UNREACHABLE();
}

std::thread coreThread;
bool coreThreadRunning { false };
std::atomic<bool> executeCore { false };
std::atomic<bool> stoppedExecuting { false };

bool setInstructions { false };
uint64_t executedInstructions {};
std::string statStr { "0000.000 MIPS | 00.000 MIPF" };

bool unlimitedMode { true };

int IPF { 10 };
bool paused { false };

bool enableRainbow { false };
Shader pixelShader{};
uint32_t chipTexture;
std::array<uint8_t, ChipState::SCR_HEIGHT* ChipState::SCR_WIDTH> textureBuf;

int viewportWidth, viewportHeight;
int menuBarHeight;
GLFWwindow* window;

const std::filesystem::path defaultPath { std::filesystem::current_path() };
std::filesystem::path currentRomPath{};
bool fileDialogOpen { false };

#ifdef _WIN32
#define STR(s) L##s
#else
#define STR(s) s
#endif

constexpr nfdnfilteritem_t ROMfilterItem[2] { {STR("ROM File"), STR("ch8,bnc")} };
constexpr nfdnfilteritem_t asmFilterItem[1] { {STR("x86-64 Assembly"), STR("txt")} };

std::string getStatStr(uint64_t instrs)
{
    const double mips { instrs / 1e6 };
    const double mipf { mips / 60 };

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << mips << " MIPS | " << mipf << " MIPF";

    return oss.str();
}

void copyChipScreenBuf()
{
    const auto& screenBuf{ chipCore->getScreenBuffer() };

    for (int i = 0; i < ChipState::SCR_WIDTH * ChipState::SCR_HEIGHT; i++)
        textureBuf[i] = (screenBuf[i >> 6] >> (63 - (i & 0x3F))) & 0x1;
}

void setBuffers()
{
    unsigned int VAO, VBO, EBO;

    constexpr unsigned int indices[]
    {
        0, 1, 3,
        1, 2, 3
    };
    constexpr float vertices[]
    {
        1.0f,  1.0f, 0.0f,  1.0f,  0.0f,  // top right     
        1.0f, -1.0f, 0.0f,  1.0f,  1.0f,  // bottom right
       -1.0f, -1.0f, 0.0f,  0.0f,  1.0f,  // bottom left
       -1.0f,  1.0f, 0.0f,  0.0f,  0.0f   // top left 
    };

    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glGenTextures(1, &chipTexture);
    glBindTexture(GL_TEXTURE_2D, chipTexture);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ChipState::SCR_WIDTH, ChipState::SCR_HEIGHT, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    pixelShader = Shader(Resources::VERTEX_SHADER, Resources::FRAGMENT_SHADER);
    pixelShader.use();

    constexpr std::array<float, 4> whiteColor { 1.0f, 1.0f, 1.0f, 1.0f };
    pixelShader.setFloat4("foregroundCol", whiteColor.data());

    constexpr std::array<float, 4> blackColor { 0.0f, 0.0f, 0.0f, 1.0f };
    pixelShader.setFloat4("backgroundCol", blackColor.data());

    pixelShader.setBool("rainbow", false);
}

template <CoreType core>
void coreThreadExecute()
{
    uint64_t localInstructions { 0 };

    while (coreThreadRunning) [[likely]]
    {
        while (executeCore) [[likely]]
        {
            switch (core)
            {
            case CoreType::JIT:
                localInstructions += chipJITCore.execute();
                break;
            case CoreType::Cached:
                localInstructions += chipCachedCore.execute();
                break;
            case CoreType::Interpret:
                chipInterpretCore.execute();
                localInstructions++;
                break;
            }
        }

        if (setInstructions)
        {
            executedInstructions = localInstructions;
            localInstructions = 0;
            setInstructions = false;
        }

        stoppedExecuting = true;
        while (stoppedExecuting) {};
    }
}

inline void startCoreThread()
{
    coreThreadRunning = true;
    executeCore = true;

    switch (currentCore())
    {
    case CoreType::Interpret:
        coreThread = std::thread { coreThreadExecute<CoreType::Interpret> };
        break;
    case CoreType::Cached:
        coreThread = std::thread { coreThreadExecute<CoreType::Cached> };
        break;
    case CoreType::JIT:
        coreThread = std::thread { coreThreadExecute<CoreType::JIT> };
        break;
    }
}
inline void stopCoreThread()
{
    coreThreadRunning = false;
    executeCore = false;

    while (!stoppedExecuting) {};
    stoppedExecuting = false;

    coreThread.join();
}

void coreModeChanged()
{
    if (coreThreadRunning)
    {
        stopCoreThread();
        startCoreThread();
    }
}

template <typename Op>
void threadSafeExec(Op func)
{
    if (coreThreadRunning)
    {
        executeCore = false;
        while (!stoppedExecuting) {};

        func();
        executeCore = true;
        stoppedExecuting = false;
    }
    else
        func();
}

void clearCoreCache()
{
    threadSafeExec([&]
    {
        switch (currentCore())
        {
            case CoreType::Interpret:
                return;
            case CoreType::Cached:
                chipCachedCore.clearCache();
                break;
            case CoreType::JIT:
                chipJITCore.clearJITCache();
                break;
        }
    });
}

void changePauseState()
{
    paused = !paused;

    if (unlimitedMode)
    {
        if (paused)
            stopCoreThread();
        else
            startCoreThread();
    }

    if (paused)
        chipCore->resetKeys();
}

void loadROM(const std::filesystem::path& path)
{
    threadSafeExec([&]
    {
        auto st { std::ifstream { path, std::ios::binary } };

        if (chipCore->loadROM(st))
        {
            currentRomPath = path;
            clearCoreCache();

            if (paused)
                changePauseState();
        }
    });
}

void renderImGUI()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Load ROM"))
            {
                fileDialogOpen = true;
                NFD::UniquePathN outPath;
                const nfdresult_t result { NFD::OpenDialog(outPath, ROMfilterItem, 1, defaultPath.c_str()) };

                if (result == NFD_OKAY)
                    loadROM(outPath.get());

                fileDialogOpen = false;
            }
            else if (ImGui::MenuItem("Reload ROM", "(Esc)"))
                loadROM(currentRomPath.c_str());

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings", "Ctrl+Q"))
        {
            static bool showForegroundPicker { false };
            static bool showBackgroundPicker { false };
            static int volume { 50 };

            ImGui::SeparatorText("Sound");
            ImGui::Checkbox("Enable Sound", &ChipCore::EnableAudio);

            if (ChipCore::EnableAudio)
            {
                ImGui::Spacing();

                if (ImGui::SliderInt("Volume", &volume, 0, 100))
                    ChipCore::setVolume(volume / 100.0);
            }

            ImGui::SeparatorText("UI");

            if (ImGui::Checkbox("Rainbow Screen", &enableRainbow))
            {
                pixelShader.setBool("rainbow", enableRainbow);
                showForegroundPicker = false;
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            static ImVec4 foregroundColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            static ImVec4 backgroundColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

            if (!enableRainbow)
            {
                ImGui::Text("Foreground Color");
                ImGui::SameLine();

                if (ImGui::ArrowButton("foregroundPicker", ImGuiDir_Down))
                {
                    showForegroundPicker = !showForegroundPicker;
                    showBackgroundPicker = false;
                }

                if (showForegroundPicker)
                {
                    if (ImGui::ColorPicker3("Pick a Color", (float*)&foregroundColor))
                        pixelShader.setFloat4("foregroundCol", (float*)&foregroundColor);
                }
            }

            ImGui::Text("Background Color");
            ImGui::SameLine();

            if (ImGui::ArrowButton("backgroundPicker", ImGuiDir_Down))
            {
                showBackgroundPicker = !showBackgroundPicker;
                showForegroundPicker = false;
            }

            if (showBackgroundPicker)
            {
                if (ImGui::ColorPicker3("Pick a Color", (float*)&backgroundColor))
                    pixelShader.setFloat4("backgroundCol", (float*)&backgroundColor);
            }

            ImGui::SeparatorText("Misc.");
            if (ImGui::Button("Reset to Default"))
            {
                foregroundColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                backgroundColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

                pixelShader.setFloat4("foregroundCol", (float*)&foregroundColor);
                pixelShader.setFloat4("backgroundCol", (float*)&backgroundColor);

                showForegroundPicker = false;
                showBackgroundPicker = false;

                enableRainbow = false;
                pixelShader.setBool("rainbow", false);

                ChipCore::EnableAudio = true;

                volume = 50;
                ChipCore::setVolume(0.5);
            }

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("CPU"))
        {
            ImGui::Text("Current Mode: ");
            ImGui::SameLine();

            if (currentCore() == CoreType::JIT)
            {
                if (ImGui::Button("JIT"))
                {
                    chipCore = &chipCachedCore;
                    chipCachedCore.clearCache();
                    coreModeChanged();
                }

                ImGui::SeparatorText("JIT Actions");

                if (ImGui::Button("Export Disassembly"))
                {
                    fileDialogOpen = true;
                    NFD::UniquePathN outPath;
                    const nfdresult_t result { NFD::SaveDialog(outPath, asmFilterItem, 1, defaultPath.c_str(), STR("x64_output.txt")) };

                    if (result == NFD_OKAY)
                        chipJITCore.dumpCode(outPath.get());

                    fileDialogOpen = false;
                }

                if (ImGui::Button("Clear Cache"))
                    clearCoreCache();
            }
            else
            {
                if (currentCore() == CoreType::Interpret)
                {
                    if (ImGui::Button("Interpreter"))
                    {
                        chipCore = &chipJITCore;
                        chipJITCore.clearJITCache();
                        coreModeChanged();
                    }
                }
                else
                {
                    if (ImGui::Button("Cached Interpret"))
                    {
                        chipCore = &chipInterpretCore;
                        coreModeChanged();
                    }

                    ImGui::SeparatorText("Actions");

                    if (ImGui::Button("Clear Cache"))
                        clearCoreCache();
                }
            }

            ImGui::SeparatorText("Performance");

            if (ImGui::Checkbox("Unlimited Mode", &unlimitedMode))
            {
                chipCachedCore.setSlowMode(!unlimitedMode);
                chipJITCore.setSlowMode(!unlimitedMode);

                if (unlimitedMode)
                {
                    if (!paused)
                        startCoreThread();
                }
                else
                {
                    if (!paused)
                        stopCoreThread();

                    glfwSetWindowTitle(window, APP_NAME);
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (unlimitedMode)
                ImGui::Text("Stats: %s", statStr.c_str());
            else
                ImGui::SliderInt("IPF", &IPF, 1, 100);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Quirks"))
        {
            if (ImGui::Checkbox("VFReset", &s.quirks.vfReset)) clearCoreCache();
            if (ImGui::Checkbox("Shifting", &s.quirks.shifting)) clearCoreCache();
            if (ImGui::Checkbox("Jumping", &s.quirks.jumping)) clearCoreCache();
            if (ImGui::Checkbox("Clipping", &s.quirks.clipping)) clearCoreCache();
            if (ImGui::Checkbox("Memory Increment", &s.quirks.memoryIncrement)) clearCoreCache();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Reset to Default"))
            {
                s.quirks = {};
                clearCoreCache();
            }

            ImGui::EndMenu();
        }
        if (paused)
        {
            ImGui::Separator();
            ImGui::Text("Paused");
        }
        ImGui::EndMainMenuBar();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void render()
{
    glClear(GL_COLOR_BUFFER_BIT);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ChipState::SCR_WIDTH, ChipState::SCR_HEIGHT, GL_RED, GL_UNSIGNED_BYTE, textureBuf.data());
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    renderImGUI();
    glfwSwapBuffers(window);
}

const std::map<int, uint8_t> keyConfig
{
    {49, 1},
    {50, 2},
    {51, 3},
    {52, 0xC},
    {81, 4},
    {87, 5},
    {69, 6},
    {82, 0xD},
    {65, 7},
    {83, 8},
    {68, 9},
    {70, 0xE},
    {90, 0xA},
    {88, 0},
    {67, 0xB},
    {86, 0xF}
};

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (action == 1)
    {
        if (key == GLFW_KEY_ESCAPE)
        {
            loadROM(currentRomPath);
            return;
        }
        if (key == GLFW_KEY_TAB)
        {
            changePauseState();
            return;
        }
    }

     if (!paused)
     {
         const auto keyInd { keyConfig.find(key) };

         if (keyInd != keyConfig.end())
         {
             threadSafeExec([&]
             {
                 chipCore->setKey(keyInd->second, action);
             });
         }          
     }
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    viewportWidth = width; viewportHeight = height - menuBarHeight;
    glViewport(0, 0, viewportWidth, viewportHeight);
}

void window_pos_callback(GLFWwindow* window, int x, int y)
{
    glViewport(0, 0, viewportWidth, viewportHeight);  
}

void window_refresh_callback(GLFWwindow* _window)
{
    (void)_window;
    if (!fileDialogOpen) render();
}

#ifdef _WIN32
#include <Windows.h>

inline std::wstring ToUTF16(const std::string& utf8Str)
{
    const auto size = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), utf8Str.length(), nullptr, 0);

    if (size <= 0)
        return L"";

    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), utf8Str.length(), result.data(), size);
    return result;
}

#elif defined(__linux__) || defined(__unix__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <unistd.h>
#include <libproc.h>
#endif

inline std::filesystem::path getExecutablePath()
{
#ifdef _WIN32
    wchar_t pathBuf[MAX_PATH];
    GetModuleFileNameW(NULL, pathBuf, MAX_PATH);
#else
    char pathBuf[4096];
#ifdef __APPLE__
    pid_t pid = getpid();
    proc_pidpath(pid, pathBuf, sizeof(pathBuf));
#elif defined(__linux__) || defined(__unix__)
    ssize_t count = readlink("/proc/self/exe", pathBuf, sizeof(pathBuf));
    if (count != -1) pathBuf[count] = '\0';
#endif
#endif

    const std::filesystem::path path{ pathBuf };
    return path.parent_path();
}

void drop_callback(GLFWwindow* _window, int count, const char** paths)
{
    (void)_window;

    if (count > 0)
    {
#ifdef _WIN32
		loadROM(ToUTF16(paths[0]));
#else
		loadROM(paths[0]);
#endif
    }
}

void setWindowSize()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::BeginMainMenuBar();
    menuBarHeight = static_cast<int>(ImGui::GetWindowSize().y);
    ImGui::EndMainMenuBar();
    ImGui::Render();

    const GLFWvidmode* mode { glfwGetVideoMode(glfwGetPrimaryMonitor()) };
    const int scale { static_cast<int>(mode->width * 0.5f) / ChipState::SCR_WIDTH };
    viewportWidth = scale * ChipState::SCR_WIDTH;
    viewportHeight = scale * ChipState::SCR_HEIGHT;

    glfwSetWindowSize(window, viewportWidth, viewportHeight + menuBarHeight);
    glfwSetWindowAspectRatio(window, viewportWidth, viewportHeight);
    glViewport(0, 0, viewportWidth, viewportHeight);
}

bool setGLFW()
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window = glfwCreateWindow(1, 1, APP_NAME, NULL, NULL);
    if (!window)
    {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetWindowRefreshCallback(window, window_refresh_callback);
    glfwSetWindowPosCallback(window, window_pos_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSetDropCallback(window, drop_callback);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return false;
    }

    return true;
}

void setImGUI()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    const int resolutionX = glfwGetVideoMode(glfwGetPrimaryMonitor())->width;
    const float scaleFactor = (resolutionX / 1920.0f);

    io.Fonts->AddFontFromMemoryCompressedTTF((void*)Resources::ROBOTO_MONO_FONT, sizeof(Resources::ROBOTO_MONO_FONT), scaleFactor * 17);
    ImGui::GetStyle().ScaleAllSizes(scaleFactor);

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
}

int main()
{
    if (!setGLFW())
        return -1;

    setImGUI();
    NFD_Init();
    setWindowSize();
    setBuffers();

    std::thread initThread { ChipCore::initAudio };

    std::stringbuf buf { std::ios::in | std::ios::out };
    buf.sputn(reinterpret_cast<const char*>(Resources::ROM_1DCELL), sizeof(Resources::ROM_1DCELL));
    std::istream st { &buf };
    chipCore->loadROM(st);

    startCoreThread();

    double lastTime { glfwGetTime() }, executeTimer{}, secondsTimer{};
    constexpr double FRAME_RATE { 1.0 / 60 };

    while (!glfwWindowShouldClose(window))
    {
        double currentTime = glfwGetTime();
        double deltaTime = currentTime - lastTime;

        executeTimer += deltaTime;
        secondsTimer += deltaTime;

        glfwPollEvents();

        if (executeTimer >= FRAME_RATE)
        {
            threadSafeExec([&]
            {
                while (executeTimer >= FRAME_RATE)
                {
                    executeTimer -= FRAME_RATE;

                    if (!paused)
                    {
                        chipCore->updateTimers();

                        if (unlimitedMode)
                            continue;

                        for (int i = 0; i < IPF;)
                            i += chipCore->execute();
                    }
                }

                copyChipScreenBuf();
            });
        }

        if (secondsTimer >= 1.0)
        {
            if (unlimitedMode)
            {
                setInstructions = true;
                threadSafeExec([&]() {});

                statStr = getStatStr(executedInstructions / secondsTimer);
                executedInstructions = 0;

                const std::string title { std::string(APP_NAME) + " (" + statStr + ")" };
                glfwSetWindowTitle(window, title.c_str());
            }

            secondsTimer = 0;
        }

        render();
        lastTime = currentTime;
    }

    NFD_Quit();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    glfwTerminate();

    initThread.join();

    if (coreThreadRunning)
        stopCoreThread();

    return 0;
}