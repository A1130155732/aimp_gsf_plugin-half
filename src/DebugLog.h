#pragma once
#include <windows.h>
#include <string>
#include <cstdio>

// 定义 ENABLE_DEBUG_LOG 宏以启用文件日志，否则禁用
//#define ENABLE_DEBUG_LOG

static void LogDebug(const wchar_t* format, ...)
{
#ifdef ENABLE_DEBUG_LOG
    static const wchar_t* logPath = L"C:\\Temp\\gsf_plugin_debug.log";
    
    wchar_t message[1024];
    va_list args;
    va_start(args, format);
    vswprintf_s(message, format, args);
    va_end(args);

    HANDLE hFile = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        SetFilePointer(hFile, 0, nullptr, FILE_END);
        
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t timeBuf[64];
        swprintf_s(timeBuf, L"[%04d-%02d-%02d] ",
                   st.wYear, st.wMonth, st.wDay);
        
        DWORD written;
        WriteFile(hFile, timeBuf, (DWORD)wcslen(timeBuf) * sizeof(wchar_t), &written, nullptr);
        WriteFile(hFile, message, (DWORD)wcslen(message) * sizeof(wchar_t), &written, nullptr);
        
        const wchar_t* newline = L"\r\n";
        WriteFile(hFile, newline, (DWORD)wcslen(newline) * sizeof(wchar_t), &written, nullptr);
        
        CloseHandle(hFile);
    }
#else
    // 当 ENABLE_DEBUG_LOG 未定义时，LogDebug 函数为空操作，避免性能开销
    (void)format; // 避免未使用参数警告
#endif
}
