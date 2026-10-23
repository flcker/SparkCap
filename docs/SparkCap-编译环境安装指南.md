# SparkCap 编译环境安装指南

- 版本：1.0
- 日期：2026-10-23
- 状态：待评审
- 关联文档：SparkCap-FFmpeg编译指南.md（环境搭好后按它编译 FFmpeg）

---

## 1. 适用范围

| 角色 | 需要的环境 | 本文档 |
|---|---|---|
| 普通开发者（编译主工程） | 仅 VS2022 + CMake（§3、§4） | 用到一半 |
| **FFmpeg 编译者 / 维护者** | VS2022 + MSYS2 全套（§3–§6） | 全文 |

主工程只要 VS2022（任意 14.3x+ 工具集）；只有编译 FFmpeg 静态库的维护者需要 MSYS2。

## 2. 环境组成总览

| 组件 | 用途 | 版本要求 | 验证命令 |
|---|---|---|---|
| Visual Studio 2022 | MSVC 编译器（cl/lib）与 CMake | v143 工具集 ≥ 14.30 | `cl`（§3.3） |
| CMake | 主工程构建 | ≥ 3.24 | `cmake --version` |
| MSYS2 | 提供 shell 环境（FFmpeg 的 configure 是脚本） | 最新 | `uname -s`（§5.2） |
| make / diffutils | FFmpeg 构建调度与 configure 依赖 | 最新 | `make --version` |
| nasm / yasm | FFmpeg x86 汇编部分 | 最新 | `nasm -v` |

## 3. Visual Studio 2022

### 3.1 已安装——验证工具集

    dir "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC"

列出的目录名即工具集版本（如 `14.44.35207`）。**≥ 14.30 即可编主工程**；FFmpeg 编译锁定维护者机器上的版本（记录于 `third_party/ffmpeg/VERSION`）。

### 3.2 未安装——安装步骤

1. 下载 [Visual Studio 2022 Community](https://visualstudio.microsoft.com/zh-hans/downloads/) 安装器
2. 安装器中勾选 workload：**"使用 C++ 的桌面开发"**（右侧默认组件已含 MSVC v143 与 Windows SDK，无需增删）
3. 安装完成后运行 VS Installer → 单个组件确认：**"MSVC v143 - VS 2022 C++ x64/x86 生成工具"** 已勾选

> CMake：VS 自带一份（`C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`）。若命令行 `cmake --version` 无输出，安装[独立版](https://cmake.org/download/)并勾选"加入 PATH"。

### 3.3 验证

打开 **"x64 Native Tools Command Prompt for VS 2022"**（开始菜单），执行：

    cl

预期输出含 `用于 x64 的 Microsoft (R) C/C++ 优化编译器` 与版本号（如 `19.44.35229`，即工具集 14.44）。

## 4. （可选）CMake 独立安装

VS 自带 CMake 时可跳过。需要独立版：https://cmake.org/download/ 下载 MSI，安装时勾选 **"Add CMake to the system PATH"**。

验证（任意终端）：

    cmake --version

预期 ≥ 3.24。

## 5. MSYS2 安装（仅 FFmpeg 编译者）

### 5.1 安装

1. 下载：https://www.msys2.org → `msys2-x86_64-latest.exe`
2. 运行安装器，全部默认即可：
   - 安装路径保持 **`C:\msys64`**（后续文档都按此路径写）
   - 其余选项默认（开始菜单组等）
3. 安装完成后**先不要**开始用——先做首次更新

### 5.2 首次更新（pacman）

MSYS2 用 `pacman` 管理软件包（与 Arch Linux 同源）。打开开始菜单的 **"MSYS2 MSYS"** 终端（注意是 **MSYS**，不是 MINGW64/UCRT64）执行：

    pacman -Syu

- 可能提示 "terminate these processes?"（终止进程确认）→ 输 `y` 回车
- 可能提示需要关闭终端以完成更新 → **关掉窗口，重新打开 MSYS2 MSYS，再执行一次** `pacman -Syu`，直到输出 `there is nothing to do` 为止

> 国内网络慢/超时：编辑 `C:\msys64\etc\pacman.d\mirrorlist.mingw64` 等文件，在文件顶部加一行清华镜像：
> `Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/x86_64/`
> （`mirrorlist.msys` 对应路径为 `/msys2/msys/$arch/`）

### 5.3 pacman 命令速查

pacman 是 MSYS2 的包管理器（与 Arch Linux 同源）。参数拆开记：`-S`（**S**ynchronize，安装/升级）、`-y`（刷新包数据库）、`-u`（升级所有过期包）、`--needed`（已装且最新则跳过）。

| 命令 | 作用 |
|---|---|
| `pacman -Syu` | 刷新数据库并升级全部已装包（首次安装后必做，见 §5.2） |
| `pacman -S 包名` | 安装包 |
| `pacman -S --needed 包1 包2` | 安装多个包（已装最新的自动跳过） |
| `pacman -Ss 关键词` | 搜索仓库 |
| `pacman -Qs 关键词` | 查询本机已装的包 |
| `pacman -Q` | 列出全部已装包 |
| `pacman -R 包名` | 卸载 |
| `pacman -Rns 包名` | 卸载并清理配置与无人依赖 |

**注意**：不要单独使用 `pacman -Sy`（只刷数据库不升级）——会造成"部分升级"状态，可能导致库版本错配；要刷数据库就带着 `-u` 一起。

### 5.4 安装构建工具

同一个 MSYS 终端执行：

    pacman -S --needed make diffutils nasm yasm

各包用途：

| 包 | 用途 |
|---|---|
| `make` | FFmpeg 的构建调度 |
| `diffutils` | configure 脚本依赖的 diff/cmp |
| `nasm` | x86 汇编器（FFmpeg 优化汇编） |
| `yasm` | 备用汇编器（老配置路径） |

> **不要**安装 `mingw-w64-x86_64-gcc` 系列——本流程全程用 MSVC 的 `cl`，装 GCC 容易误链错工具链。

### 5.5 验证

MSYS 终端执行：

    make --version | head -1
    diff --version | head -1
    nasm -v

预期分别输出 GNU Make、GNU diffutils、NASM 的版本行。

## 6. 环境整体验证（FFmpeg 编译就绪检查）

按顺序执行，全过即环境就绪：

**第 1 步**：开始菜单打开 "x64 Native Tools Command Prompt for VS 2022"，执行：

    C:\msys64\msys2_shell.cmd -use-full-path

（`-use-full-path` 让 MSYS 继承 VS 的 PATH/INCLUDE/LIB，`cl` 才可用）

**第 2 步**：进入的 MSYS 终端里执行：

    which cl && cl 2>&1 | head -2
    which make && make --version | head -1
    which nasm && nasm -v

**预期**：`cl` 路径指向 `.../VC/Tools/MSVC/14.xx.../bin/Hostx64/x64/cl`，三工具版本行全部输出。

**第 3 步**：版本记录。把 `cl` 版本号（14.xx）记下来——编译 FFmpeg 时写入 `third_party/ffmpeg/VERSION`。

环境就绪，接下来按 **docs/SparkCap-FFmpeg编译指南.md §3** 开始编译。

## 7. 常见问题

| 现象 | 处置 |
|---|---|
| `pacman -Syu` 卡住/超时 | 按 §5.2 换清华镜像 |
| MSYS 里 `cl: command not found` | 没用 `-use-full-path` 启动，或启动它的父终端不是 x64 Native Tools |
| `cl` 找到但报 `ucrt.lib` 之类找不到 | VS 的 Windows SDK 组件没装全：VS Installer → 修改 → 确认 "Windows 11 SDK" 勾选 |
| 杀毒软件拦截 make/nasm | 加入白名单（MSYS2 是正规开源软件，误报常见） |
| 装错进了 MINGW64/UCRT64 终端 | 关掉，用 "MSYS2 MSYS"（开始菜单里第一个紫色图标组） |
| 想卸载重装 MSYS2 | 删除 `C:\msys64` 目录 + 开始菜单组即可（无注册表残留） |
