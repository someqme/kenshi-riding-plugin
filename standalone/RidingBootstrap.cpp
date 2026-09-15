// RidingBootstrap - in-process loader for RidingPlugin without RE_Kenshi.dll.
// Injected into kenshi_x64.exe by RidingLauncher. Loads the real KenshiLib.dll,
// runs KenshiLib::Init() (fills RVA stubs from RE_Kenshi/RVAs/*.br), then loads
// RidingPlugin.dll and calls startPlugin().
//
// Must NOT do this work inside DllMain (loader lock). A worker thread is spawned
// on PROCESS_ATTACH and does the real sequence.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <fstream>

typedef bool (*KenshiLibInitFn)();
typedef void (*StartPluginFn)();

static std::string ULongToStr(unsigned long v)
{
    char buf[32];
    _snprintf_s(buf, _TRUNCATE, "%lu", v);
    return buf;
}

static void LogLine(const char* msg)
{
    // Mirror into the same file ridelog.py already reads when present;
    // always also leave a local breadcrumb next to the game exe.
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    char gameDir[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, gameDir, MAX_PATH))
    {
        char* slash = strrchr(gameDir, '\\');
        if (slash) *slash = 0;
        std::string path = std::string(gameDir) + "\\RidingBootstrap.log";
        std::ofstream f(path.c_str(), std::ios::app);
        if (f.good())
            f << msg << "\n";
    }
}

static bool ModuleLoaded(const char* name)
{
    return GetModuleHandleA(name) != NULL;
}

static bool HasRvaTables(const std::string& root)
{
    // Steam is the supported target; GOG tables ride along in the package.
    return GetFileAttributesA((root + "\\RE_Kenshi\\RVAs\\Steam_1.0.65.br").c_str())
           != INVALID_FILE_ATTRIBUTES;
}

// Resolve a directory that contains KenshiLib.dll + RE_Kenshi\RVAs\*.br.
// Prefer the game directory (where RE_Kenshi users already have them);
// fall back to the directory this DLL was loaded from (our standalone package).
static std::string FindDataRoot()
{
    char gameDir[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, gameDir, MAX_PATH);
    char* slash = strrchr(gameDir, '\\');
    if (slash) *slash = 0;
    std::string game = gameDir;

    char selfDir[MAX_PATH] = {0};
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&FindDataRoot, &self);
    GetModuleFileNameA(self, selfDir, MAX_PATH);
    slash = strrchr(selfDir, '\\');
    if (slash) *slash = 0;
    // Prefer the bundled KLib 0.4.0/table pair.  A user's existing RE_Kenshi
    // install may carry a newer KLib with a different function-table length.
    if (GetFileAttributesA((std::string(selfDir) + "\\KenshiLib.dll").c_str()) != INVALID_FILE_ATTRIBUTES
        && HasRvaTables(selfDir))
        return selfDir;
    if (GetFileAttributesA((game + "\\KenshiLib.dll").c_str()) != INVALID_FILE_ATTRIBUTES
        && HasRvaTables(game))
        return game;
    return selfDir;
}

static DWORD WINAPI BootstrapThread(LPVOID)
{
    // Give the host a moment to finish its own early loads.
    Sleep(500);

    if (ModuleLoaded("RE_Kenshi.dll"))
    {
        LogLine("RidingBootstrap: RE_Kenshi.dll already present - refusing to load (mutual exclusion).");
        return 1;
    }

    std::string root = FindDataRoot();
    std::string klibPath = root + "\\KenshiLib.dll";
    LogLine(("RidingBootstrap: data root = " + root).c_str());

    HMODULE klib = LoadLibraryA(klibPath.c_str());
    if (!klib)
    {
        LogLine(("RidingBootstrap: LoadLibrary KenshiLib.dll failed, gle="
                 + ULongToStr(GetLastError())).c_str());
        return 2;
    }
    LogLine("RidingBootstrap: KenshiLib.dll loaded.");

    KenshiLibInitFn initFn =
        (KenshiLibInitFn)GetProcAddress(klib, "?Init@KenshiLib@@YA_NXZ");
    if (!initFn)
    {
        LogLine("RidingBootstrap: KenshiLib::Init export not found.");
        return 3;
    }

    // Init resolves RVAs via the relative path RE_Kenshi/RVAs/*.br.
    // Our injected CWD is the game folder, but the tables may live in the
    // standalone package next to this DLL - point CWD at the data root first.
    char prevCwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(MAX_PATH, prevCwd);
    SetCurrentDirectoryA(root.c_str());

    bool initOk = initFn();

    SetCurrentDirectoryA(prevCwd);

    if (!initOk)
    {
        LogLine("RidingBootstrap: KenshiLib::Init() returned false - RVA resolve failed.");
        LogLine("RidingBootstrap: check RE_Kenshi/RVAs/Steam_1.0.65.br next to KenshiLib.dll.");
        return 4;
    }
    LogLine("RidingBootstrap: KenshiLib::Init() OK.");

    // RidingPlugin.dll sits next to us in the standalone package.
    char selfDir[MAX_PATH] = {0};
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&BootstrapThread, &self);
    GetModuleFileNameA(self, selfDir, MAX_PATH);
    char* slash = strrchr(selfDir, '\\');
    if (slash) *slash = 0;
    std::string pluginPath = std::string(selfDir) + "\\RidingPlugin.dll";

    HMODULE plugin = LoadLibraryA(pluginPath.c_str());
    if (!plugin)
    {
        LogLine(("RidingBootstrap: LoadLibrary RidingPlugin.dll failed, gle="
                 + ULongToStr(GetLastError())).c_str());
        return 5;
    }
    LogLine("RidingBootstrap: RidingPlugin.dll loaded.");

    StartPluginFn start =
        (StartPluginFn)GetProcAddress(plugin, "?startPlugin@@YAXXZ");
    if (!start)
    {
        LogLine("RidingBootstrap: startPlugin export not found.");
        return 6;
    }

    start();
    LogLine("RidingBootstrap: startPlugin() returned - riding is live.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE th = CreateThread(NULL, 0, BootstrapThread, NULL, 0, NULL);
        if (th)
            CloseHandle(th);
        else
            LogLine("RidingBootstrap: CreateThread failed.");
    }
    return TRUE;
}
