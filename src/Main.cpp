#include <ImGUI/imgui.h>
#include <ImGUI/imgui_impl_glfw.h>
#include <ImGUI/imgui_impl_opengl3.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <nfd.hpp>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#endif

#include <array>
#include <map>
#include <sstream>
#include <iostream>   
#include <filesystem>
#include <thread>
#include <chrono>

#include "Shader.h"
#include "resources.h"
#include "ChipInterpretCore.h"
#include "ChipAsmInterpretCore.h"
#include "ChipCachedCore.h"
#include "ChipJITCore.h"

constexpr auto APP_NAME { "MegaJIT-8" };

enum class CoreType
{
    Interpret,
    AsmInterpret,
    Cached,
    JIT
};

std::thread coreThread;
std::atomic coreThreadRunning { false };
std::atomic executeCore { false };
std::atomic stoppedExecuting { false };

ChipState s{};

ChipInterpretCore chipInterpretCore { s, executeCore };
ChipAsmInterpretCore chipAsmInterpretCore { s, executeCore };
ChipCachedCore chipCachedCore { s, executeCore };
ChipJITCore chipJITCore { s, executeCore };
ChipCore* chipCore { &chipJITCore };

CoreType currentCore()
{
    if (chipCore == &chipJITCore)
        return CoreType::JIT;
    if (chipCore == &chipInterpretCore)
        return CoreType::Interpret;
    if (chipCore == &chipAsmInterpretCore)
        return CoreType::AsmInterpret;

    return CoreType::Cached;
}

bool setStats { false };
uint64_t executedInstructions{}, accumulatedInstructions{};
double cpuFrequency{}, secondsTimer, totalTime{};
std::string statsStr { "0000.000 MIPS | 00.000 MIPF" }, avgPerfStr { "Avg. over 0 seconds: 0000.000 MIPS" };

bool unlimitedMode { true };

int IPF { 10 };
bool paused { false };

bool vsync { false };
bool lockVsyncSetting { false };
bool rainbow { false };
Shader pixelShader{};
uint32_t chipTexture;
std::array<uint8_t, ChipState::SCR_HEIGHT * ChipState::SCR_WIDTH> textureBuf;

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

constexpr nfdnfilteritem_t ROMfilterItem[2] { {STR("ROM File"), STR("ch8,bin")} };
constexpr nfdnfilteritem_t asmFilterItem[1] { {STR("x86-64 Assembly"), STR("txt")} };

Xbyak::CodeGenerator code{};
std::string cpuBrandStr(48, '\0');

void emitCPUNameGetter()
{
    code.push(code.rbx);
    code.mov(code.r8, reinterpret_cast<uintptr_t>(cpuBrandStr.data()));

    code.xor_(code.ecx, code.ecx);
    code.mov(code.eax, 0x80000002);
    code.cpuid();
    code.mov(code.dword[code.r8 + 0], code.eax);
    code.mov(code.dword[code.r8 + 4], code.ebx);
    code.mov(code.dword[code.r8 + 8], code.ecx);
    code.mov(code.dword[code.r8 + 12], code.edx);

    code.xor_(code.ecx, code.ecx);
    code.mov(code.eax, 0x80000003);
    code.cpuid();
    code.mov(code.dword[code.r8 + 16], code.eax);
    code.mov(code.dword[code.r8 + 20], code.ebx);
    code.mov(code.dword[code.r8 + 24], code.ecx);
    code.mov(code.dword[code.r8 + 28], code.edx);

    code.xor_(code.ecx, code.ecx);
    code.mov(code.eax, 0x80000004);
    code.cpuid();
    code.mov(code.dword[code.r8 + 32], code.eax);
    code.mov(code.dword[code.r8 + 36], code.ebx);
    code.mov(code.dword[code.r8 + 40], code.ecx);
    code.mov(code.dword[code.r8 + 44], code.edx);

    code.pop(code.rbx);
    code.ret();
}

// Is guaranteed to take ~100 cycles per iteration on any cpu since 'add eax, eax' sequence is dependent on previous results.
void emitBurn100xCyclesFunc()
{
    Xbyak::Label loop;
    code.L(loop);

    for (int i = 0; i < 100; i++)
        code.add(code.eax, code.eax);

#ifdef _WIN32
    code.dec(code.ecx);
#else
    code.dec(code.edi);
#endif
    code.jnz(loop);

    code.ret();
}

void emitCode()
{
    emitCPUNameGetter();
    code.getCode<void(*)()>()();

    const auto nul { std::ranges::find(cpuBrandStr, '\0') };
    cpuBrandStr.erase(nul, cpuBrandStr.end());
    const auto pos { cpuBrandStr.find_last_not_of(' ') };

    if (pos == std::string::npos)
        cpuBrandStr.clear();
    else
        cpuBrandStr.erase(pos + 1);

    code.resetSize();
    emitBurn100xCyclesFunc();
}

void setOpenGL()
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

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glGenTextures(1, &chipTexture);
    glBindTexture(GL_TEXTURE_2D, chipTexture);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ChipState::SCR_WIDTH, ChipState::SCR_HEIGHT, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    pixelShader = Shader(Resources::VERTEX_SHADER, Resources::FRAGMENT_SHADER);
    pixelShader.use();
    pixelShader.setBool("rainbow", false);
}

template <CoreType core>
void coreThreadExecute()
{
    uint64_t localInstructions { 0 };

    while (coreThreadRunning) [[likely]]
    {
        switch (core)
        {
        case CoreType::JIT:
            localInstructions += chipJITCore.execute();
            break;
        case CoreType::Interpret:
            localInstructions += chipInterpretCore.execute();
            break;
        case CoreType::AsmInterpret:
            localInstructions += chipAsmInterpretCore.execute();
            break;
        case CoreType::Cached:
            localInstructions += chipCachedCore.execute();
            break;
        }

        if (setStats)
        {
            constexpr int EXECUTE_CYCLES { 100000 };

            const auto start { std::chrono::high_resolution_clock::now() };
            code.getCode<void(*)(int)>()(EXECUTE_CYCLES / 100);
            const auto elapsed { std::chrono::high_resolution_clock::now() - start };

            cpuFrequency = EXECUTE_CYCLES / static_cast<double>(elapsed.count());
            executedInstructions = localInstructions;
            localInstructions = 0;
            setStats = false;
        }

        stoppedExecuting = true;
        while (stoppedExecuting) {};
    }
}

void startCoreThread()
{
    coreThreadRunning = true;
    executeCore = true;

    switch (currentCore())
    {
    case CoreType::JIT:
        coreThread = std::thread{ coreThreadExecute<CoreType::JIT> };
        break;
    case CoreType::Interpret:
        coreThread = std::thread { coreThreadExecute<CoreType::Interpret> };
        break;
    case CoreType::AsmInterpret:
        coreThread = std::thread{ coreThreadExecute<CoreType::AsmInterpret> };
        break;
    case CoreType::Cached:
        coreThread = std::thread { coreThreadExecute<CoreType::Cached> };
        break;
    }
}
void stopCoreThread()
{
    coreThreadRunning = false;
    executeCore = false;

    while (!stoppedExecuting) {};
    stoppedExecuting = false;

    coreThread.join();
}

template <typename Op>
void threadSafeExec(Op func)
{
    if (executeCore)
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

void clearStats()
{
    setStats = true;
    secondsTimer = 0.0;
    totalTime = 0.0;
    accumulatedInstructions = 0;
}

void clearCoreCache()
{
    threadSafeExec([&]
    {
        switch (currentCore())
        {
        case CoreType::JIT:
            chipJITCore.clearJITCache();
            break;
        case CoreType::Cached:
            chipCachedCore.clearCache();
            break;
        default:
            break;
        }
    });
}

void coreModeChanged()
{
    if (!coreThreadRunning)
        return;

    stopCoreThread();
    clearCoreCache();
    clearStats();
    startCoreThread();
}

void changePauseState()
{
    paused = !paused;

    if (unlimitedMode)
    {
        if (paused)
            stopCoreThread();
        else
        {
            startCoreThread();
            clearStats();
        }
    }

    if (paused)
        chipCore->resetKeys();
}

void loadROM(std::istream& st, const std::filesystem::path& path)
{
    threadSafeExec([&]
    {
        if (chipCore->loadROM(st))
        {
            clearCoreCache();
            clearStats();
            currentRomPath = path;

            if (paused)
                changePauseState();
        }
    });
}

void loadROM(const std::filesystem::path& path = "")
{
    if (path.empty())
    {
        std::stringbuf buf{ std::ios::in | std::ios::out };
        buf.sputn(reinterpret_cast<const char*>(&Resources::ROM_1DCELL[0]), sizeof(Resources::ROM_1DCELL));
        std::istream st{ &buf };
        loadROM(st, "");
        return;
    }

    auto st { std::ifstream { path, std::ios::binary } };
    loadROM(st, path);
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

                if (const nfdresult_t result { NFD::OpenDialog(outPath, ROMfilterItem, 1, defaultPath.c_str()) }; result == NFD_OKAY)
                    loadROM(outPath.get());

                fileDialogOpen = false;
            }
            else if (ImGui::MenuItem("Reload ROM", "(Esc)"))
                loadROM(currentRomPath.c_str());

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings", "Ctrl+Q"))
        {
            static int volume { 50 };

            ImGui::SeparatorText("Audio");
            ImGui::Checkbox("Enable Audio", &ChipCore::EnableAudio);

            if (ChipCore::EnableAudio)
            {
                ImGui::Spacing();

                if (ImGui::SliderInt("Volume", &volume, 0, 100))
                    ChipCore::setVolume(volume / 100.0);
            }

            ImGui::SeparatorText("Graphics");

            if (lockVsyncSetting) 
                ImGui::BeginDisabled();

            if (ImGui::Checkbox("VSync", &vsync))
            {
                glfwSwapInterval(vsync ? 1 : 0);
#ifdef _WIN32
                if (vsync)
                    timeEndPeriod(1);
                else
                    timeBeginPeriod(1);
#endif
            }
            if (lockVsyncSetting)
            {
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Forced in GPU driver settings!");

                ImGui::EndDisabled();
            }

            if (ImGui::Checkbox("Rainbow Screen", &rainbow))
                pixelShader.setBool("rainbow", rainbow);

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("CPU"))
        {
            ImGui::Text("Current Mode: ");
            ImGui::SameLine();

            switch (currentCore())
            {
            case CoreType::JIT:
                if (ImGui::Button("JIT Compiler (x64)"))
                {
                    chipCore = &chipAsmInterpretCore;
                    coreModeChanged();
                }

                ImGui::SeparatorText("JIT Actions");

                if (ImGui::Button("Export Disassembly"))
                {
                    fileDialogOpen = true;
                    NFD::UniquePathN outPath;

                    if (const nfdresult_t result{ NFD::SaveDialog(outPath, asmFilterItem, 1, defaultPath.c_str(), STR("x64_output.txt")) }; result == NFD_OKAY)
                    {
                        threadSafeExec([&]
                        {
                            chipJITCore.dumpCode(outPath.get());
                        });
                    }

                    fileDialogOpen = false;
                }

                if (ImGui::Button("Clear Cache"))
                {
                    threadSafeExec([&]
                    {
                        chipJITCore.clearJITCache();
                    });
                }
                break;
            case CoreType::AsmInterpret:
                if (ImGui::Button("Interpreter (ASM)"))
                {
                    chipCore = &chipInterpretCore;
                    coreModeChanged();
                }
                break;
            case CoreType::Interpret:
                if (ImGui::Button("Interpreter (C++)"))
                {
                    chipCore = &chipCachedCore;
                    coreModeChanged();
                }
                break;
            default:
                if (ImGui::Button("Cached Interp. (C++)"))
                {
                    chipCore = &chipJITCore;
                    coreModeChanged();
                }

                ImGui::SeparatorText("Actions");

                if (ImGui::Button("Clear Cache"))
                {
                    threadSafeExec([&]
                    {
                        chipCachedCore.clearCache();
                    });
                }
                break;
            }

            ImGui::SeparatorText("Performance");

            if (ImGui::Checkbox("Unlimited Mode", &unlimitedMode))
            {
                if (unlimitedMode)
                {
                    chipCachedCore.setSlowMode(false);
                    chipJITCore.setSlowMode(false);

                    if (!paused)
                        startCoreThread();

                    clearStats();
                }
                else
                {
                    if (!paused)
                        stopCoreThread();

                    chipCachedCore.setSlowMode(true);
                    chipJITCore.setSlowMode(true);

                    glfwSetWindowTitle(window, APP_NAME);
                }
            }

            if (unlimitedMode && !currentRomPath.empty())
            {
                ImGui::SameLine();

                if (ImGui::Button("Bench"))
                {
                    loadROM();
                    currentRomPath.clear();
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (unlimitedMode)
            {
                ImGui::Text("Stats: %s", statsStr.c_str());
                ImGui::Text("%s", avgPerfStr.c_str());
            }
            else
                ImGui::SliderInt("IPF", &IPF, 1, 100);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Quirks"))
        {
            if (ImGui::Checkbox("VF Reset", &s.quirks.vfReset)) clearCoreCache();
            if (ImGui::Checkbox("Memory", &s.quirks.memoryIncrement)) clearCoreCache();
            if (ImGui::Checkbox("Clipping", &s.quirks.clipping)) clearCoreCache();
            if (ImGui::Checkbox("Shifting", &s.quirks.shifting)) clearCoreCache();
            if (ImGui::Checkbox("Jumping", &s.quirks.jumping)) clearCoreCache();

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

void copyChipScreenBuf()
{
    const auto& screenBuf { chipCore->getScreenBuffer() };

    for (int i = 0; i < ChipState::SCR_WIDTH * ChipState::SCR_HEIGHT; i++)
        textureBuf[i] = (screenBuf[i >> 6] >> (63 - (i & 0x3F))) & 0x1;
}

void render()
{
    glClear(GL_COLOR_BUFFER_BIT);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ChipState::SCR_WIDTH, ChipState::SCR_HEIGHT, GL_RED, GL_UNSIGNED_BYTE, textureBuf.data());
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
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
         if (const auto keyInd { keyConfig.find(key) }; keyInd != keyConfig.end())
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
std::wstring ToUTF16(const std::string& utf8Str)
{
    const auto size { MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), static_cast<int>(utf8Str.length()), nullptr, 0) };

    if (size <= 0)
        return L"";

    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), static_cast<int>(utf8Str.length()), result.data(), size);
    return result;
}
#endif

std::filesystem::path getExecutablePath()
{
#ifdef _WIN32
    wchar_t pathBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, pathBuf, MAX_PATH);
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

    const std::filesystem::path path { pathBuf };
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
    const int scale { static_cast<int>(static_cast<float>(mode->width) * 0.5f) / ChipState::SCR_WIDTH };
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

    window = glfwCreateWindow(1, 1, APP_NAME, nullptr, nullptr);
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

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
    {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return false;
    }

    return true;
}

void checkVSyncStatus()
{
#if defined (__APPLE__)
    glfwSwapInterval(0);
    lockVsyncSetting = true;
#elif defined(_WIN32) 
    const auto vsyncCheckFunc { reinterpret_cast<int(*)()>(glfwGetProcAddress("wglGetSwapIntervalEXT")) };

    glfwSwapInterval(1);

    if (!vsyncCheckFunc())
        lockVsyncSetting = true;
    else
    {
        glfwSwapInterval(0);

        if (vsyncCheckFunc())
        {
            vsync = true;
            lockVsyncSetting = true;
        }
        else
            timeBeginPeriod(1);
    }
#else
    glfwSwapInterval(0);
#endif
}

void setImGUI()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io { ImGui::GetIO() };
    io.IniFilename = nullptr;

    const int resolutionX { glfwGetVideoMode(glfwGetPrimaryMonitor())->width };
    const float scaleFactor { static_cast<float>(resolutionX) / 1920.0f };

    io.Fonts->AddFontFromMemoryCompressedTTF(Resources::ROBOTO_MONO_FONT, sizeof(Resources::ROBOTO_MONO_FONT), scaleFactor * 17);
    ImGui::GetStyle().ScaleAllSizes(scaleFactor);

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
}

int main()
{
    if (!setGLFW())
        return -1;

    setOpenGL();
    setImGUI();
    setWindowSize();
    checkVSyncStatus();
    NFD_Init();
    emitCode();

    srand(time(nullptr));

    std::thread initThread { ChipCore::initAudio };
    initThread.detach();
    loadROM();
    startCoreThread();

    double lastTime { glfwGetTime() }, execTimer{};
    constexpr double FRAME_RATE { 1.0 / 60 };

    while (!glfwWindowShouldClose(window))
    {
        const double currentTime { glfwGetTime() };
        const double deltaTime { currentTime - lastTime };

        lastTime = currentTime;
        execTimer += deltaTime;
        secondsTimer += deltaTime;

        const bool frameElapsed { vsync || execTimer >= FRAME_RATE };

        if (frameElapsed)
            glfwPollEvents();
        else if (!vsync)
        {
            const double remainder { FRAME_RATE - execTimer };

            constexpr double SLEEP_THRESHOLD =
#ifdef _WIN32
                0.002;
#else
                0.001;
#endif
            if (remainder >= SLEEP_THRESHOLD)
            {
                // Sleep on windows is less precise than linux/macOS, even with timeBeginPeriod(1). So need to sleep less time.
                const std::chrono::duration<double> sleepTime {
#ifdef _WIN32
                    remainder <= 0.004 ? 0.001 : remainder <= 0.006 ? 0.002 : remainder / 1.6
#else
                    remainder / 1.5
#endif
                };
                std::this_thread::sleep_for(sleepTime);
            }
        }

        if (execTimer >= FRAME_RATE)
        {
            if (!paused)
            {
                threadSafeExec([&]
                {
                    chipCore->updateTimers();

                    if (!unlimitedMode)
                    {
                        for (int i = 0; i < IPF;)
                            i += static_cast<int>(chipCore->execute());
                    }

                    copyChipScreenBuf();
                });
            }

            execTimer -= FRAME_RATE;
        }

        if (secondsTimer >= 1.0)
        {
            if (unlimitedMode)
            {
                std::ostringstream oss;

                if (!paused)
                {
                    setStats = true;
                    threadSafeExec([&]() {});
                    executedInstructions = static_cast<uint64_t>(static_cast<double>(executedInstructions) / secondsTimer);
                    accumulatedInstructions += executedInstructions;
                    totalTime += secondsTimer;

                    oss << "Avg. over " << static_cast<int>(totalTime) << " seconds: " << ((accumulatedInstructions / static_cast<int>(totalTime)) / 1e6) << " MIPS";
                    avgPerfStr = oss.str();
                    oss.str("");
                }
                else
                    executedInstructions = 0;

                const double mips { static_cast<double>(executedInstructions) / 1e6 };
                const double mipf { mips / 60 };
                oss << std::fixed << std::setprecision(3) << APP_NAME << " (" << mips << " MIPS) | " << cpuBrandStr;

                if (!paused)
                    oss << " | " << cpuFrequency << " GHz";;

                glfwSetWindowTitle(window, oss.str().c_str());
                
                oss.str("");
                oss << mips << " MIPS | " << mipf << " MIPF";
                statsStr = oss.str();
            }

            secondsTimer = 0;
        }

        if (frameElapsed)
            render();
    }

    NFD_Quit();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    glfwTerminate();

    if (coreThreadRunning)
        stopCoreThread();

    return 0;
}