#pragma once
#include <windows.h>
#include <string>

void InitLog(HMODULE hMod);
void LogLine(const std::string& s);