#include "log.h"
#include <fstream>
#include <mutex>

static HMODULE g_logMod = nullptr;
static std::mutex g_logMtx;
static std::ofstream g_log;

void InitLog(HMODULE hMod) {
    g_logMod = hMod;
}

void LogLine(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (!g_log.is_open()) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(g_logMod, path, MAX_PATH);
        std::string fullPath(path);
        auto pos = fullPath.find_last_of("\\/");
        if (pos != std::string::npos) {
            fullPath.resize(pos + 1);
        }
        fullPath += "g3_rtgi_dump.log";
        g_log.open(fullPath, std::ios::out | std::ios::trunc);
    }
    g_log << s << "\n";
    g_log.flush();
}