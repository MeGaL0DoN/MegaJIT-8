#include <ImGUI/imgui.h>
#include <ImGUI/imgui_impl_glfw.h>
#include <ImGUI/imgui_impl_opengl3.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <nfd.hpp>

#include <array>
#include <map>
#include <sstream>
#include <iostream>   
#include <filesystem>
#include <thread>

#include "Shader.h"
#include "resources.h"
#include "ChipInterpretCore.h"
#include "ChipJITCore.h"

constexpr const char* APP_NAME = "MegaJIT-8";

extern ChipState s;

ChipInterpretCore chipInterpretCore{};
ChipJITCore chipJITCore{};
ChipCore* chipCore{ &chipJITCore };

std::thread cpuThread;
std::atomic<bool> CPUThreadRunning{ false };
uint64_t executedInstructions{ 0 };

bool JITMode{ true };
bool unlimitedMode{ false };

int IPF { 9 };
bool paused{ false };

bool enableRainbow { false };
Shader pixelShader;
uint32_t chipTexture;

int viewport_width, viewport_height;
int menuBarHeight;
GLFWwindow* window;

bool fileDialogOpen{ false };

#ifdef _WIN32
#define STR(s) L##s
#else
#define STR(s) s
#endif

const std::filesystem::path defaultPath{ std::filesystem::current_path() };
constexpr nfdnfilteritem_t ROMfilterItem[2]{ {STR("ROM File"), STR("ch8,bnc")} };
constexpr nfdnfilteritem_t asmFilterItem[1]{ {STR("x64 Assembly"), STR("txt")} };

std::string instrPerSecondStr{"Instructions per second: 0.000 MIPS"};

std::string toMIPSstring(uint64_t instr)
{
    double mips = instr / 1e6;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << mips << " MIPS";

    return oss.str();
}

std::array<uint8_t, ChipState::SCRHeight * ChipState::SCRWidth> textureBuf;

void draw()
{
    const auto& screenBuf = chipCore->getScreenBuffer();

    for (int i = 0; i < ChipState::SCRWidth * ChipState::SCRHeight; i++)
        textureBuf[i] = (screenBuf[i >> 6] >> (63 - (i & 0x3F))) & 0x1;

    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ChipState::SCRWidth, ChipState::SCRHeight, GL_RED, GL_UNSIGNED_BYTE, textureBuf.data());
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

void setBuffers()
{
    unsigned int VAO, VBO, EBO;

    constexpr unsigned int indices[] =
    {
        0, 1, 3,
        1, 2, 3
    };
    constexpr float vertices[] =
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

    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ChipState::SCRWidth, ChipState::SCRHeight, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    pixelShader = Shader(resources::vertexShader, resources::fragmentShader);
    pixelShader.use();

    constexpr std::array<float, 4> whiteColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    pixelShader.setFloat4("foregroundCol", whiteColor.data());

    constexpr std::array<float, 4> blackColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    pixelShader.setFloat4("backgroundCol", blackColor.data());

    pixelShader.setBool("rainbow", false);
}

template <bool JIT>
void cpuThreadExecute()
{
    uint64_t threadInstructions{ 0 };

    while (CPUThreadRunning) [[likely]]
    {
        if constexpr (JIT)
            threadInstructions += chipJITCore.execute();
        else
        {
            chipInterpretCore.execute();
            threadInstructions++;
        }
    }

    executedInstructions = threadInstructions;
}

inline void startCPUThread()
{
    CPUThreadRunning = true;
    cpuThread = std::thread{ JITMode ? cpuThreadExecute<true> : cpuThreadExecute<false> };
}
inline void stopCPUThread()
{
    CPUThreadRunning = false;
    cpuThread.join();
}

std::filesystem::path currentROMPAth{};
void loadROM(const std::filesystem::path& path)
{
    if (CPUThreadRunning)
        stopCPUThread();

    if (chipCore->loadROM(path))
    {
        paused = false;
        currentROMPAth = path;
    }

    if (unlimitedMode)
        startCPUThread();
}

inline void changePauseState()
{
    paused = !paused;

    if (paused)
        chipCore->resetKeys();

    if (unlimitedMode)
    {
        if (paused)
            stopCPUThread();
        else
            startCPUThread();
    }
}

inline void coreModeChanged()
{
    if (CPUThreadRunning)
    {
        stopCPUThread();
        startCPUThread();
    }
}

inline void clearJITcache()
{
    bool threadRunning = CPUThreadRunning;
    if (threadRunning) stopCPUThread();

    chipJITCore.clearJITCache();
    if (threadRunning) startCPUThread();
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
                nfdresult_t result = NFD::OpenDialog(outPath, ROMfilterItem, 1, defaultPath.c_str());

                if (result == NFD_OKAY)
                    loadROM(outPath.get());

                fileDialogOpen = false;
            }
            else if (ImGui::MenuItem("Reload ROM", "(Esc)"))
                loadROM(currentROMPAth.c_str());

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings", "Ctrl+Q"))
        {
            static bool showForegroundPicker{ false };
            static bool showBackgroundPicker{ false };
            static int volume{ 50 };

            ImGui::SeparatorText("Sound");
            ImGui::Checkbox("Enable Sound", &ChipCore::enableAudio);

            if (ChipCore::enableAudio)
            {
                ImGui::Separator();

                if (ImGui::SliderInt("Volume", &volume, 0, 100))
                    ChipCore::setVolume(volume / 100.0);
            }

            ImGui::SeparatorText("UI");

            if (ImGui::Checkbox("Rainbow Screen", &enableRainbow))
            {
                pixelShader.setBool("rainbow", enableRainbow);
                showForegroundPicker = false;
            }

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

                ImGui::Separator();
                ImGui::Spacing();
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

                ChipCore::enableAudio = true;

                volume = 50;
                ChipCore::setVolume(0.5);
            }

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("CPU"))
        {
            ImGui::Text("Current Mode: ");
            ImGui::SameLine();

            if (JITMode)
            {
                if (ImGui::Button("JIT"))
                {
                    JITMode = false;
                    chipCore = &chipInterpretCore;
                    coreModeChanged();
                }

                ImGui::SeparatorText("JIT actions");

                if (ImGui::Button("Export Disassembly"))
                {
                    fileDialogOpen = true;
                    NFD::UniquePathN outPath;
                    nfdresult_t result = NFD::SaveDialog(outPath, asmFilterItem, 1, defaultPath.c_str(), STR("x64_output.txt"));

                    if (result == NFD_OKAY)
                        chipJITCore.dumpCode(outPath.get());

                    fileDialogOpen = false;
                }

                if (ImGui::Button("Clear Cache"))
                    clearJITcache();
            }
            else
            {
                if (ImGui::Button("Interpreter"))
                {
                    JITMode = true;
                    chipCore = &chipJITCore;
                    chipJITCore.clearJITCache();
                    coreModeChanged();
                }
            }

            ImGui::SeparatorText("Performance");

            if (!chipCore->isRomLoaded())
                ImGui::BeginDisabled();

            if (ImGui::Checkbox("Unlimited Mode", &unlimitedMode))
            {
                bool startThread{ false };

                if (!paused)
                {
                    if (unlimitedMode)
                        startThread = true;
                    else
                    {
                        if (CPUThreadRunning) stopCPUThread();
                        glfwSetWindowTitle(window, APP_NAME);
                    }
                }

                chipJITCore.setSlowMode(!unlimitedMode);
                if (startThread) startCPUThread();
            }

            if (!chipCore->isRomLoaded())
                ImGui::EndDisabled();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (unlimitedMode)
                ImGui::Text("%s", instrPerSecondStr.c_str());
            else
                ImGui::SliderInt("IPF", &IPF, 1, 100);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Quirks"))
        {
            if (ImGui::Checkbox("VFReset", &Quirks::VFReset) && JITMode) clearJITcache();
            if (ImGui::Checkbox("Shifting", &Quirks::Shifting) && JITMode) clearJITcache();
            if (ImGui::Checkbox("Jumping", &Quirks::Jumping) && JITMode) clearJITcache();
            if (ImGui::Checkbox("Clipping", &Quirks::Clipping) && JITMode) clearJITcache();
            if (ImGui::Checkbox("Memory Increment", &Quirks::MemoryIncrement) && JITMode) clearJITcache();

            ImGui::Spacing();
            ImGui::Separator();

            if (ImGui::Button("Reset to Default"))
            {
                Quirks::Reset();
                if (JITMode) clearJITcache();
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
    draw();
    renderImGUI();
    glfwSwapBuffers(window);
}

const std::map<int, uint8_t> keyConfig =
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
            loadROM(currentROMPAth);
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
         auto keyInd = keyConfig.find(key);

         if (keyInd != keyConfig.end())
             chipCore->setKey(keyInd->second, action);
     }
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    viewport_width = width; viewport_height = height - menuBarHeight;
    glViewport(0, 0, viewport_width, viewport_height);
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
#endif

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

    const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    viewport_width = { static_cast<int>(mode->width * 0.55f) };
    viewport_height = viewport_width / 2;

    glfwSetWindowSize(window, viewport_width, viewport_height + menuBarHeight);
    glfwSetWindowAspectRatio(window, viewport_width, viewport_height);
    glViewport(0, 0, viewport_width, viewport_height);
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
    io.IniFilename = "imgui.ini";

    const int resolutionX = glfwGetVideoMode(glfwGetPrimaryMonitor())->width;
    const float scaleFactor = (resolutionX / 1920.0f);

    io.Fonts->AddFontFromMemoryTTF((void*)resources::robotoMonoFont, sizeof(resources::robotoMonoFont), scaleFactor * 17);
    ImGui::GetStyle().ScaleAllSizes(scaleFactor);

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
}

int main()
{
    if (!setGLFW()) return -1;
    setImGUI();
    NFD_Init();
    setWindowSize();
    setBuffers();

    std::thread initThread{ ChipCore::initAudio };
    loadROM("ROMs/chipLogo.ch8");

    double lastTime = glfwGetTime();
    double executeTimer{};
    double secondsTimer{};

    while (!glfwWindowShouldClose(window))
    {
        double currentTime = glfwGetTime();
        double deltaTime = currentTime - lastTime;

        executeTimer += deltaTime;
        secondsTimer += deltaTime;

        glfwPollEvents();

        while (executeTimer >= 1.0 / 60)
        {
            executeTimer -= (1.0 / 60);

            if (!paused && chipCore->isRomLoaded())
            {
                chipCore->updateTimers();

                if (!unlimitedMode)
                {
                    for (int i = 0; i < IPF; i++)
                    {
                        if (JITMode)
							chipJITCore.execute();
						else
							chipInterpretCore.execute();			
                    }
                }
            }
        }

        if (secondsTimer >= 1.0)
        {
            if (unlimitedMode)
            {
                bool threadRunning = CPUThreadRunning;

                if (threadRunning)
                    stopCPUThread();

                std::string mipsStr = toMIPSstring(executedInstructions / secondsTimer);
                instrPerSecondStr = "Instructions per second: " + mipsStr;

                std::string title = std::string(APP_NAME) + " (" + mipsStr + ")";
                glfwSetWindowTitle(window, title.c_str());

                executedInstructions = 0;

                if (threadRunning)
                    startCPUThread();
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

    if (CPUThreadRunning)
        stopCPUThread();

    return 0;
}