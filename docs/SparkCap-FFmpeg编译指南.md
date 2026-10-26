# SparkCap FFmpeg 编译指南

- 版本：1.0
- 日期：2026-10-23
- 状态：待评审
- 关联文档：SparkCap-技术选型方案.md §7、SparkCap-命名规范.md §5/§7

---

## 1. 目的与产物

按技术选型 §7 以 **MSVC v14.44** 自行编译静态裁剪版 FFmpeg，仅保留 RTSP/RTP/TCP/UDP 协议、H.264/HEVC 解码与 D3D11VA，供 `backend_ffmpeg.cpp` 内部链接；符号不进 sparkcap.dll 导出表（.def 隔离）。

**仓库不含任何 FFmpeg 构建产物**（纯源码仓库）：克隆后须先按本指南构建一次（双配置约 8 分钟），产物全部在本机：

    third_party/ffmpeg/
      VERSION            # 锁定记录（入库）：tag + 编译日期 + 工具集
      build_ffmpeg.sh    # 一键构建脚本（入库）
      src/               # 源码树（本地产出，不入库）
      install/           # make install 临时目录（不入库）
      include/           # 构建产出的头（不入库，CMake 消费）
      lib/
        Release/         # avcodec/avformat/avutil .lib，/MT（不入库，CMake 消费）
        Debug/           # 同上，/MTd（不入库）

## 2. 环境准备（一次性）

按 **docs/SparkCap-编译环境安装指南.md** 安装并验证（VS2022 + MSYS2 + make/diffutils/nasm/yasm，以及 `msys2_shell.cmd -use-full-path` 的正确启动方式）。本指南从环境就绪处开始。

- 前提：VS2022（FFmpeg 编译机器锁定 v14.44，记录于 VERSION）；主工程仅要求 ≥ 14.30
- 磁盘：源码 + 编译产物约 2 GB

## 3. 获取源码（锁定 tag）

**方式一（推荐）：官方 release tarball**——一次下载约 11 MB，无 git 历史：

    cd D:\workspaces\github\SparkCap\third_party\ffmpeg
    curl -L -o ffmpeg-9.0.2.tar.xz https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
    tar -xJf ffmpeg-9.0.2.tar.xz && ren ffmpeg-9.0.2 src

**方式二：git clone**（GitHub 直连慢时可弃用）：

    git clone --branch n9.0.2 --depth 1 https://github.com/FFmpeg/FFmpeg.git src

tag 以锁定时稳定分支最新补丁为准（2026-10 为 9.0.2 "Lei"）。

## 4. 编译步骤

构建已固化为 `third_party/ffmpeg/build_ffmpeg.sh`（configure 全参数见 §5），两份配置（Release /MT 与 Debug /MTd）各跑一次。

### 4.1 一行命令（无头/CI，推荐）

在任意终端：

    cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul && C:\msys64\msys2_shell.cmd -defterm -no-start -use-full-path /d/workspaces/github/SparkCap/third_party/ffmpeg/build_ffmpeg.sh Release'

三个 msys2_shell 参数缺一不可（实测踩坑）：

| 参数 | 作用 |
|---|---|
| `-defterm` | 绕过 mintty 直接跑 bash——**否则无输出**（mintty 是 GUI 终端，吞掉管道） |
| `-no-start` | 不用 start（同步执行并返回退出码） |
| `-use-full-path` | 继承 vcvars 的完整 PATH（cl 可见；缺它 bash 的 profile 会丢弃 VS 路径） |

> 交互调试可用两参数版：`msys2_shell.cmd -use-full-path`（mintty 窗口）。

### 4.2 产物与耗时

每次约 3–5 分钟。产物在 `install/<Config>/`：`include/` 与 `lib/`（avcodec/avformat/avutil 三个 .lib，合计约 41 MB；链入 DLL 后经 /OPT:REF 裁剪仅增量约 2.6 MB，N6 达标）。

### 4.3 主工程链接（已完成，备忘）

CMake 已接好（`CMakeLists.txt`）：FFmpeg 头按 SYSTEM include + `/external:W0`（第三方警告不归 /W4 /WX 管）；按 `$<CONFIG>` 选 `lib/Release|Debug`。FFmpeg 静态库的 **Windows 系统依赖**须一并链接：

| 系统库 | 用途 |
|---|---|
| ws2_32 | socket（RTSP/RTP 网络） |
| crypt32 / secur32 / ncrypt | schannel TLS（rtsp over TLS 路径被无条件拉入） |
| bcrypt | avutil 随机数 |

## 5. configure 参数说明

| 参数 | 理由 |
|---|---|
| `--toolchain=msvc` | 用 MSVC cl/lib 替代 GCC；产物与主工程同工具集（技术选型 §7） |
| `--target-os=win32` | FFmpeg 对 Windows 目标的惯用值（64 位也是它） |
| `--enable-static --disable-shared` | 静态内嵌，无 avcodec-*.dll 运行时依赖（A7） |
| `--disable-programs --disable-doc` | 不要 ffmpeg.exe 与文档 |
| `--disable-avdevice/avfilter/swscale/swresample` | 只要解复用 + 解码；无转换、无滤镜（F9 不做转换） |
| `--disable-everything` 再逐项 `--enable-*` | 白名单式裁剪，控制体积（N6） |
| `--enable-network` + 四个 protocol | 仅 RTSP 走线：RTSP 信令 + RTP 载荷 + TCP（交错模式）+ UDP |
| demuxer/parser/decoder 仅 h264/hevc | IP Camera 主流编码（Spec F10/R5） |
| hwaccel 四项 | D3D11VA 跨厂商硬解（技术选型 §6；软解回落见同节） |
| `--extra-cflags="-MT"` | 静态 CRT，与主工程一致 |
| `--prefix` | 分 Release/Debug 两目录安装 |

## 6. 产物就位（脚本自动完成）

`build_ffmpeg.sh` 在 `make install` 后自动拷贝：

- `install/<Config>/lib/*.lib` → `lib/<Config>/`（主工程 CMake 按 `$<CONFIG>` 链接）
- `install/<Config>/include/` → `include/`（两配置头相同，只拷一次）

全部为**本地产出、不入库**（.gitignore 覆盖 `src/`、`install/`、`include/`、`lib/`）。FFmpeg 升级或换新机器时重跑 §4 一行命令即可。

维护者升级 FFmpeg 版本时更新 `VERSION`（入库，记录锁定 tag、工具集与编译日期）。

## 7. 验证

1. **静态自洽**：`dumpbin /dependents avcodec.lib`（库无 DLL 依赖声明）
2. **链接主工程后**（CMake 接入后）：
   - `dumpbin /exports sparkcap.dll` 无 `av*` 符号（A7）
   - `dumpbin /dependents sparkcap.dll` 无 avcodec-*.dll（仅系统库）
3. **体积**（N6 个位数 MB）：三 .lib 合计预期 5–10 MB；链入 DLL 后增量 3–5 MB
4. `git status` 确认 `src/`、`install/` 未入库

## 8. 常见问题

| 现象 | 处置 |
|---|---|
| `cl is unable to create an executable file` | 未继承 VS 环境：确认在 x64 Native Tools 命令行里用 `-use-full-path` 启动 MSYS2 |
| msys2_shell 跑脚本**无任何输出** | 加 `-defterm`（mintty 吞掉管道输出，见 §4.1） |
| `nasm/yasm is missing` | `pacman -S nasm yasm` |
| `--enable-hwaccel=... unknown option` | 新版 FFmpeg 已把 d3d11va 并入 decoder（`--enable-decoder=h264,hevc` 即含）：去掉该行重试 |
| GitHub clone 慢/卡死 | 用 §3 方式一 tarball；GitHub release 资产可经 `api.github.com/repos/<owner>/<repo>/releases/assets/<id>`（`Accept: application/octet-stream`）下载 |
| 链接期 unresolved external（`___security_cookie` 等） | MSYS 未继承 VS 的 LIB：同"cl unable"处置 |
| 链接期 `__imp_CertStrToNameA`/`BCryptGenRandom` 等缺失 | FFmpeg 的系统依赖没链全：见 §4.3 系统库表 |
| libav 函数链接报 C++ 修饰名 | libavutil 子头（time.h/opt.h 等）单独 include 无 extern "C" 保护：全部 libav include 包进 `extern "C" {}`（backend_ffmpeg.cpp 已如此） |
| 想改裁剪范围 | 先 `make distclean` 再重新 configure |

## 9. 版本升级流程

1. 在 `src/` 里 `git fetch --tags && git checkout <新 tag>`
2. 重跑 §4–§7 全流程（两份配置）
3. 更新 `VERSION`，提交时在 CHANGELOG 记录升级原因（安全补丁/新特性）
