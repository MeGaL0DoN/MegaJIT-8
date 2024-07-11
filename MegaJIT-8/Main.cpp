#include "ImGui/imgui.h"
#include "ImGui/imgui_impl_glfw.h"
#include "ImGui/imgui_impl_opengl3.h"

#include "glad/glad.h"
#include "GLFW/glfw3.h"
#include "nfd/nfd.hpp"

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
uint32_t executedInstructions{ 0 };

bool JITMode{ true };
bool unlimitedMode{ false };

int CPUfrequency{ 500 };
bool pause{ false };

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
constexpr nfdnfilteritem_t ROMfilterItem[2]{ {STR("ROM File"), STR("ch8,benchmark")} };
constexpr nfdnfilteritem_t asmFilterItem[1]{ {STR("x64 Assembly"), STR("txt")} };

std::string instrPerSecondStr{"Instructions per second: 0.000 MIPS"};

std::string toMIPSstring(uint32_t instr)
{
    double mips = instr / 1e6;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << mips << " MIPS";

    return oss.str();
}

void draw()
{
    const auto& screenBuf = chipCore->getScreenBuffer();
    std::array<uint8_t, ChipState::SCRHeight* ChipState::SCRWidth> textureBuf{};

    while (s.drawLock.exchange(true)) {}

    for (int i = 0; i < ChipState::SCRWidth * ChipState::SCRHeight; i++)
        textureBuf[i] = (screenBuf[i >> 6] >> (63 - (i & 0x3F))) & 0x1;

    s.drawLock.store(false);

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
}

template <bool JIT>
void cpuThreadExecute()
{
    uint32_t threadInstructions{ 0 };

    while (CPUThreadRunning) [[likely]]
    {
        if constexpr (JIT)
            threadInstructions += chipJITCore.execute();
        else
            threadInstructions += chipInterpretCore.execute();
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
        pause = false;
        currentROMPAth = path;
    }

    if (unlimitedMode)
        startCPUThread();
}

inline void changePauseState()
{
    pause = !pause;

    if (pause)
        chipCore->resetKeys();

    if (unlimitedMode)
    {
        if (pause)
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
                ImGui::Spacing();

                if (ImGui::SliderInt("Volume", &volume, 0, 100))
                    ChipCore::setVolume(volume / 100.0);
            }

            ImGui::SeparatorText("UI");

            static ImVec4 foregroundColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            static ImVec4 backgroundColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

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
                    nfdresult_t result = NFD::SaveDialog(outPath, asmFilterItem, 1, defaultPath.c_str(), L"x64_output.txt");

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

            if (ImGui::Checkbox("Unlimited Mode", &unlimitedMode))
            {
                bool startThread{ false };

                if (!pause)
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
            if (unlimitedMode && JITMode)
			{
                ImGui::Spacing();

                if (ImGui::Checkbox("Draw Sync", &s.enableDrawLocking))
                    clearJITcache();
			}

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (unlimitedMode)
                ImGui::Text(instrPerSecondStr.c_str());
            else
                ImGui::SliderInt("CPU Frequency", &CPUfrequency, 60, 10000);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Quirks"))
        {
            ImGui::Checkbox("VFReset", &Quirks::VFReset);
            ImGui::Checkbox("Shifting", &Quirks::Shifting);
            ImGui::Checkbox("Jumping", &Quirks::Jumping);
            ImGui::Checkbox("Clipping", &Quirks::Clipping);
            ImGui::Checkbox("Memory Increment", &Quirks::MemoryIncrement);

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Button("Reset to Default")) Quirks::Reset();
            ImGui::EndMenu();
        }
        if (pause)
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

std::map<int, uint8_t> keyConfig{};
void loadKeyConfig()
{
    std::ifstream configFile("data/keyConfig.ini");
    std::string line;

    while (std::getline(configFile, line))
    {
        std::stringstream ss(line);
        char chipKey;
        int bindValue;

        ss >> chipKey;
        ss.ignore(1);
        ss >> bindValue;

        keyConfig[bindValue] = std::stoi(std::string{ chipKey }, 0, 16);
    }
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (action == 1)
    {
        if (key == GLFW_KEY_ESCAPE)
        {
            loadROM(currentROMPAth.c_str());
            return;
        }
        if (key == GLFW_KEY_TAB)
        {
            changePauseState();
            return;
        }
    }

    if (!pause)
    {
        auto keyInd = keyConfig.find(scancode);

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

void drop_callback(GLFWwindow* _window, int count, const char** paths)
{
    (void)_window;

    if (count > 0)
        loadROM(paths[0]);
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
    viewport_width = { static_cast<int>(mode->width * 0.62f) };
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
    io.IniFilename = "data/imgui.ini";

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
    setWindowSize();
    setBuffers();

    loadKeyConfig();

    std::thread initThread{ ChipCore::initAudio };
    loadROM("ROMs/chipLogo.ch8");

    double lastTime = glfwGetTime();
    double executeTimer{};
    double secondsTimer{};

    double remainderCycles{};

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

            if (!pause)
            {
                chipCore->updateTimers();

                if (!unlimitedMode)
                {
                    double cycles = (CPUfrequency / 60.0) + remainderCycles;
                    int wholeCycles { static_cast<int>(cycles) };
                    remainderCycles = cycles - wholeCycles;

                    for (int i = 0; i < wholeCycles; i++)
                        chipCore->execute();
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

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    glfwTerminate();

    initThread.join();

    if (CPUThreadRunning)
        stopCPUThread();

    return 0;
}
