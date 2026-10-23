# SparkCap FFmpeg 编译指南

- 版本：1.0
- 日期：2026-10-23
- 状态：待评审
- 关联文档：SparkCap-技术选型方案.md §7、SparkCap-命名规范.md §5/§7

---

## 1. 目的与产物

按技术选型 §7 以 **MSVC v14.44** 自行编译静态裁剪版 FFmpeg，仅保留 RTSP/RTP/TCP/UDP 协议、H.264/HEVC 解码与 D3D11VA，供 `backend_ffmpeg.cpp` 内部链接；符号不进 sparkcap.dll 导出表（.def 隔离）。

目标布局（`third_party/ffmpeg/`，成品 .lib 入库供团队直链，无需各自编译）：

    third_party/ffmpeg/
      VERSION            # 锁定记录：tag + commit hash + 编译日期 + 工具集
      include/           # libavcodec/ libavformat/ libavutil/ 头文件
      lib/
        Release/         # avcodec.lib avformat.lib avutil.lib（/MT）
        Debug/           # 同上（/MTd；接受调试期不严格可省略此份）

## 2. 环境准备（一次性）

按 **docs/SparkCap-编译环境安装指南.md** 安装并验证（VS2022 + MSYS2 + make/diffutils/nasm/yasm，以及 `msys2_shell.cmd -use-full-path` 的正确启动方式）。本指南从环境就绪处开始。

- 前提：VS2022（FFmpeg 编译机器锁定 v14.44，记录于 VERSION）；主工程仅要求 ≥ 14.30
- 磁盘：源码 + 编译产物约 2 GB

## 3. 获取源码（锁定 tag）

    cd D:\workspaces\github\SparkCap\third_party\ffmpeg
    git clone https://github.com/FFmpeg/FFmpeg.git src
    cd src
    git checkout n9.0.2        # 锁定稳定分支最新补丁 tag（2026-10 时为 9.0.2 "Lei"）

记录 commit hash（写入 VERSION）：

    git rev-parse HEAD

## 4. 编译步骤

### 4.1 进入带 VS 环境的 MSYS2 shell

`configure --toolchain=msvc` 需要 `cl` 在 PATH 中。标准做法：**在 VS 的 x64 命令行里启动 MSYS2 并继承完整 PATH**：

1. 开始菜单打开 **"x64 Native Tools Command Prompt for VS 2022"**
2. 执行：

       C:\msys64\msys2_shell.cmd -use-full-path

`-use-full-path` 使 MSYS 继承 vcvars64 设置的 PATH/INCLUDE/LIB，`cl.exe` 直接可用。

### 4.2 configure（Release，/MT）

在上述 MSYS shell 中：

    cd /d/workspaces/github/SparkCap/third_party/ffmpeg/src

    ./configure \
      --toolchain=msvc \
      --target-os=win32 \
      --arch=x86_64 \
      --enable-static \
      --disable-shared \
      --disable-programs \
      --disable-doc \
      --disable-avdevice \
      --disable-avfilter \
      --disable-swscale \
      --disable-swresample \
      --disable-everything \
      --enable-network \
      --enable-protocol=rtsp,rtp,tcp,udp \
      --enable-demuxer=rtsp,h264,hevc \
      --enable-parser=h264,hevc \
      --enable-decoder=h264,hevc \
      --enable-hwaccel=h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2 \
      --extra-cflags="-MT" \
      --prefix=/d/workspaces/github/SparkCap/third_party/ffmpeg/install/Release

### 4.3 编译并安装

    make -j8
    make install

产物在 `install/Release/`：`include/`（三个库的头）与 `lib/`（`avcodec.lib`、`avformat.lib`、`avutil.lib`）。

### 4.4 Debug 版（/MTd，可选但推荐）

重开一个干净的 MSYS shell（或在另一份源码目录），仅改变两处：

    ./configure ...（同 4.2，改两处） \
      --extra-cflags="-MTd" \
      --prefix=/d/workspaces/github/SparkCap/third_party/ffmpeg/install/Debug

    make -j8 && make install

> 说明：主工程 Debug（/MTd）链 /MT 编译的 FFmpeg 通常能链接成功（UCRT 符号兼容），但属未定义行为的灰区——编两份最严谨。若接受调试期不严格，可只编 /MT 一份，主工程 Debug 也用它。

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

## 6. 产物入库

    cd third_party/ffmpeg
    cp -r install/Release/include include
    mkdir -p lib/Release lib/Debug
    cp install/Release/lib/*.lib lib/Release/
    cp install/Debug/lib/*.lib   lib/Debug/     # 若编了 Debug 版

写入 `VERSION`：

    FFmpeg n9.0.2 (Lei)
    commit: <git rev-parse HEAD 输出>
    工具集: MSVC v14.44（VS2022 17.14），/MT（Debug: /MTd）
    编译日期: 2026-10-xx
    configure: 见 docs/SparkCap-FFmpeg编译指南.md §4.2

仓库的 `.gitignore` 忽略 `src/` 与 `install/`（源码树与临时安装），**成品 `lib/` 与 `include/` 入库**——团队成员链产物即可，无需本机重编（14.x ABI 兼容）。

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
| `nasm/yasm is missing` | `pacman -S nasm yasm` |
| `--enable-hwaccel=... unknown option` | 新版 FFmpeg 已把 d3d11va 并入 decoder（`--enable-decoder=h264,hevc` 即含）：去掉该行重试 |
| 链接期 unresolved external（`___security_cookie` 等） | MSYS 未继承 VS 的 LIB：同"cl unable"处置 |
| 想改裁剪范围 | 先 `make distclean` 再重新 configure |

## 9. 版本升级流程

1. 在 `src/` 里 `git fetch --tags && git checkout <新 tag>`
2. 重跑 §4–§7 全流程（两份配置）
3. 更新 `VERSION`，提交时在 CHANGELOG 记录升级原因（安全补丁/新特性）
