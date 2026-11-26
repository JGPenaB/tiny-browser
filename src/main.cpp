#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "common.h"

namespace tb {
int run_browser(HINSTANCE hi, const std::string& start);
int run_screenshot(const std::string& url, const std::wstring& out, int w, int h, bool dev, float scroll, int devTab,
                   const std::wstring& script, const std::wstring& vaultPath);
int run_selftest(const std::wstring& outPath);
namespace js {
int runJsFile(const std::wstring& path, const std::wstring& outPath);
int runJsBatch(const std::wstring& listPath, const std::wstring& outPath);
}
}  // namespace tb

// Usage:
//   tinybrowser.exe [url]
//   tinybrowser.exe --selftest [results.txt]
//   tinybrowser.exe --js <script.js> [output.txt]
//   tinybrowser.exe --screenshot <url> <out.png> [--size WxH] [--devtools [elements|network|perf]] [--scroll N]
//                   [--do "click:x,y;type:text;key:enter"] [--vault test-vault.dat]
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    LocalFree(argv);

    if (args.size() >= 2 && args[0] == L"--js") return tb::js::runJsFile(args[1], args.size() > 2 ? args[2] : L"");
    if (args.size() >= 3 && args[0] == L"--js-batch") return tb::js::runJsBatch(args[1], args[2]);
    if (!args.empty() && args[0] == L"--selftest") return tb::run_selftest(args.size() > 1 ? args[1] : L"selftest.txt");
    if (args.size() >= 3 && args[0] == L"--screenshot") {
        int w = 1280, h = 860, devTab = 0;
        bool dev = false;
        float scroll = 0;
        std::wstring script, vault;
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == L"--size" && i + 1 < args.size()) swscanf_s(args[++i].c_str(), L"%dx%d", &w, &h);
            else if (args[i] == L"--scroll" && i + 1 < args.size()) scroll = (float)_wtof(args[++i].c_str());
            else if (args[i] == L"--do" && i + 1 < args.size()) script = args[++i];
            else if (args[i] == L"--vault" && i + 1 < args.size()) vault = args[++i];
            else if (args[i] == L"--devtools") {
                dev = true;
                if (i + 1 < args.size() && args[i + 1].rfind(L"--", 0) != 0) {
                    std::wstring t = args[++i];
                    devTab = t == L"network" ? 1 : t == L"perf" ? 2 : t == L"console" ? 3 : 0;
                }
            }
        }
        return tb::run_screenshot(tb::narrow(args[1]), args[2], w, h, dev, scroll, devTab, script, vault);
    }
    return tb::run_browser(hi, args.empty() ? "about:home" : tb::narrow(args[0]));
}
