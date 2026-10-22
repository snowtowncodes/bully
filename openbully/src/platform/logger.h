#pragma once

#include <string>
#include <cstdio>
#include <utility>

namespace OpenBully {

class Logger {
public:
    static void Init();
    
    template<typename... Args>
    static void Info(const char* fmt, Args&&... args) {
        printf("[INFO] ");
        printf(fmt, std::forward<Args>(args)...);
        printf("\n");
    }
    
    template<typename... Args>
    static void Error(const char* fmt, Args&&... args) {
        fprintf(stderr, "[ERROR] ");
        fprintf(stderr, fmt, std::forward<Args>(args)...);
        fprintf(stderr, "\n");
    }
    
    template<typename... Args>
    static void Warning(const char* fmt, Args&&... args) {
        printf("[WARNING] ");
        printf(fmt, std::forward<Args>(args)...);
        printf("\n");
    }
};

} // namespace OpenBully
