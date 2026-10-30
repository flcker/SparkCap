// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// SparkCap 内部：字符串工具（src/ 私有头）。

#pragma once

#include <windows.h>

#include <stddef.h>

namespace sparkcap {

// 宽字符 → UTF-8，写入 dst（cap 字节）；转换失败或超长时截断，保证 NUL 结尾。
inline void toUtf8(const wchar_t* src, char* dst, size_t cap)
{
    if (cap == 0)
        return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, static_cast<int>(cap),
                                      nullptr, nullptr);
    if (n <= 0)
        dst[0] = '\0';                         // 转换失败置空，不视为枚举失败
    else if (static_cast<size_t>(n) == cap)
        dst[cap - 1] = '\0';                   // 恰好写满：强制结尾
}

// UTF-8 → 宽字符，写入 dst（cap 个 wchar）；转换失败或超长时截断，保证 NUL 结尾。
inline void toWide(const char* src, wchar_t* dst, size_t cap)
{
    if (cap == 0)
        return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, static_cast<int>(cap));
    if (n <= 0)
        dst[0] = L'\0';
    else if (static_cast<size_t>(n) == cap)
        dst[cap - 1] = L'\0';
}

} // namespace sparkcap
