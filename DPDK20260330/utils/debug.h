#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

// 声明全局变量
extern int g_dbg_en;
extern int g_dbg_error_en;

// 调试宏定义 (行尾反斜杠 \ 是必须的，表示宏换行)
#define dbg(fmt, ...)                                                          \
    do {                                                                       \
        if (g_dbg_en) {                                                        \
            printf("[INFO][%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__);   \
        }                                                                      \
    } while (0)

#define dbg_err(fmt, ...)                                                      \
    do {                                                                       \
        if (g_dbg_error_en) {                                                  \
            fprintf(stderr, "[ERROR][%s:%d] " fmt, __FILE__, __LINE__,         \
                    ##__VA_ARGS__);                                            \
        }                                                                      \
    } while (0)

#endif // DEBUG_H