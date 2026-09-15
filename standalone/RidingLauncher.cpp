// RidingLauncher - starts Kenshi and injects RidingBootstrap.dll.
// Standalone player path: no RE_Kenshi.dll required.
//
// Usage:
//   RidingLauncher.exe [kenshi_dir]
// If kenshi_dir is omitted, tries Steam library paths and %KenshiDir%.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <stdio.h>

static void Fail(const char* msg)
{
    MessageBoxA(NULL, msg, "Riding Launcher", MB_OK | MB_ICONERROR);
    fprintf(stderr, "%s\n", msg);
    ExitProcess(1);
}

static bool FileExists(const std::string& p)
{
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool NeedsSteam1068Downgrade(const std::string& kenshi)
{
    std::ifstream f((kenshi + "\\currentVersion.txt").c_str());
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.find("1.0.68") != std::string::npos) return true;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA((kenshi + "\\kenshi_x64.exe").c_str(), GetFileExInfoStandard, &fa)) return false;
    ULARGE_INTEGER n;
    n.HighPart = fa.nFileSizeHigh; n.LowPart = fa.nFileSizeLow;
    return n.QuadPart == 36718592ULL; // Steam 1.0.68 x64 (MD5 8A03C256...)
}

// RE_Kenshi itself handles Steam 1.0.68 by applying this official Courgette
// patch to a private 1.0.65 executable.  Keep the same compatibility path in
// standalone mode: KLib 0.4.0 and its 7409-entry table remain ABI-compatible,
// while the user's installed Kenshi.exe is never overwritten.
static bool EnsureSteam1068CompatExe(const std::string& kenshi, const std::string& self,
                                      std::string& exeName)
{
    exeName = "kenshi_x64.exe";
    if (!NeedsSteam1068Downgrade(kenshi)) return true;

    std::string tool = self + "\\tools\\courgette64.exe";
    std::string patch = self + "\\tools\\kenshi_x64.exe.patch";
    std::string out = kenshi + "\\kenshi_x64_riding_1.0.65.exe";
    if (!FileExists(tool) || !FileExists(patch))
        return false;

    // The official output is 36,713,984 bytes. Reuse it when present so a
    // second launch does not rewrite a file that belongs to this package.
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(out.c_str(), GetFileExInfoStandard, &fa)) {
        ULARGE_INTEGER n;
        n.HighPart = fa.nFileSizeHigh; n.LowPart = fa.nFileSizeLow;
        if (n.QuadPart == 36713984ULL) { exeName = "kenshi_x64_riding_1.0.65.exe"; return true; }
    }
    DeleteFileA(out.c_str());

    std::string cmd = "\"" + tool + "\" -apply \"" + kenshi + "\\kenshi_x64.exe\" \"" + patch + "\" \"" + out + "\"";
    std::vector<char> line(cmd.begin(), cmd.end()); line.push_back(0);
    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, &line[0], NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, self.c_str(), &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (code != 0 || !FileExists(out)) return false;
    exeName = "kenshi_x64_riding_1.0.65.exe";
    return true;
}

static std::string DirName(const std::string& path)
{
    size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return path;
    return path.substr(0, slash);
}

static std::string SelfDir()
{
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    return DirName(buf);
}

// Common Steam library roots. Enough for a first cut; override with argv[1]
// or the KenshiDir environment variable.
static std::vector<std::string> CandidateKenshiDirs()
{
    std::vector<std::string> out;

    char env[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("KenshiDir", env, MAX_PATH) > 0)
        out.push_back(env);

    const char* guesses[] = {
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Kenshi",
        "C:\\Program Files\\Steam\\steamapps\\common\\Kenshi",
        "D:\\steam\\steamapps\\common\\Kenshi",
        "D:\\Steam\\steamapps\\common\\Kenshi",
        "E:\\Steam\\steamapps\\common\\Kenshi",
        "E:\\steam\\steamapps\\common\\Kenshi",
    };
    for (size_t i = 0; i < sizeof(guesses) / sizeof(guesses[0]); ++i)
        out.push_back(guesses[i]);

    return out;
}

static std::string FindKenshiDir(int argc, char** argv)
{
    if (argc >= 2 && FileExists(std::string(argv[1]) + "\\kenshi_x64.exe"))
        return argv[1];

    std::vector<std::string> cands = CandidateKenshiDirs();
    for (size_t i = 0; i < cands.size(); ++i)
    {
        if (FileExists(cands[i] + "\\kenshi_x64.exe"))
            return cands[i];
    }
    return "";
}

// Inject a DLL via classic CreateRemoteThread(LoadLibraryW).
static bool InjectDll(HANDLE process, const std::wstring& dllPath)
{
    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;

    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, NULL))
    {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
    if (!loadLib)
    {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE th = CreateRemoteThread(process, NULL, 0,
                                   (LPTHREAD_START_ROUTINE)loadLib,
                                   remote, 0, NULL);
    if (!th)
    {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(th, 15000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return code != 0;
}

int main(int argc, char** argv)
{
    std::string self = SelfDir();
    std::string bootstrap = self + "\\RidingBootstrap.dll";
    std::string plugin = self + "\\RidingPlugin.dll";

    if (!FileExists(bootstrap))
        Fail("RidingBootstrap.dll not found next to this launcher.");
    if (!FileExists(plugin))
        Fail("RidingPlugin.dll not found next to this launcher.");

    std::string kenshi = FindKenshiDir(argc, argv);
    if (kenshi.empty())
    {
        Fail("Could not find kenshi_x64.exe.\n"
             "Pass the Kenshi install folder as the first argument,\n"
             "or set the KenshiDir environment variable.");
        return 1;
    }

    if (FileExists(kenshi + "\\RE_Kenshi.dll"))
    {
        int r = MessageBoxA(NULL,
            "RE_Kenshi.dll is present in the Kenshi folder.\n"
            "The standalone launcher and RE_Kenshi cannot both load RidingPlugin.\n\n"
            "Continue anyway? (Bootstrap will refuse if RE_Kenshi actually loads.)",
            "Riding Launcher", MB_YESNO | MB_ICONWARNING);
        if (r != IDYES)
            return 0;
    }

    // The standalone package must carry its own KLib/table ABI.  Do not
    // silently select a newer RE_Kenshi KLib from the game directory.
    std::string klibGame = kenshi + "\\KenshiLib.dll";
    std::string klibSelf = self + "\\KenshiLib.dll";
    if (!FileExists(klibSelf))
    {
        Fail("KenshiLib.dll not found.\n"
             "Place the standalone package's KLib next to this launcher.\n"
             "Do not substitute a different RE_Kenshi/KenshiLib build.");
        return 1;
    }

    std::string exeName;
    if (!EnsureSteam1068CompatExe(kenshi, self, exeName))
    {
        Fail("Steam 1.0.68 detected, but the bundled compatibility patch could not be applied.\n"
             "Keep tools\\courgette64.exe and tools\\kenshi_x64.exe.patch next to the launcher.\n"
             "The original Kenshi_x64.exe was not modified.");
        return 1;
    }
    std::string exe = kenshi + "\\" + exeName;
    std::wstring wexe(exe.begin(), exe.end());
    std::wstring wboot(bootstrap.begin(), bootstrap.end());
    std::wstring wkenshi(kenshi.begin(), kenshi.end());

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // Create suspended so injection happens before the game finishes booting.
    if (!CreateProcessW(wexe.c_str(), NULL, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, wkenshi.c_str(), &si, &pi))
    {
        char buf[256];
        _snprintf_s(buf, _TRUNCATE, "CreateProcess failed (gle=%lu).", GetLastError());
        Fail(buf);
        return 1;
    }

    bool ok = InjectDll(pi.hProcess, wboot);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (!ok)
    {
        Fail("Failed to inject RidingBootstrap.dll into kenshi_x64.exe.\n"
             "The game was still started; riding will NOT be available this session.");
        return 1;
    }

    printf("Kenshi started with RidingPlugin bootstrap.\n");
    printf("Game dir : %s\n", kenshi.c_str());
    printf("Bootstrap: %s\n", bootstrap.c_str());
    printf("If riding does not work, check %s\\RidingBootstrap.log\n", kenshi.c_str());
    return 0;
}
