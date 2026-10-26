SparkCap
Copyright (C) 2026 SparkCap Contributors

本库以 GNU Lesser General Public License v3.0 授权（见 LICENSE）。

第三方组件
==========

FFmpeg (libavcodec / libavformat / libavutil)
  版本    : n9.0.2 "Lei"（官方 release tarball，https://ffmpeg.org/releases/）
  许可    : GNU LGPL-2.1-or-later（全文见 third_party/ffmpeg/LICENSE）
  用途    : RTSP 流媒体接入与 H.264/HEVC 解码（D3D11VA 硬解/软解）
  构建方式: 静态裁剪编译（MSVC v14.44），仅启用 RTSP/RTP/TCP/UDP 协议、
            H.264/HEVC 解码与 D3D11VA，--enable-gpl 与 --enable-nonfree
            均未开启（纯 LGPL 路径）。构建脚本与配置见
            third_party/ffmpeg/build_ffmpeg.sh 与 docs/building-ffmpeg.md
  替换/重链: LGPL 义务履行方式——SparkCap 全部源码在本仓库公开，
            FFmpeg 上游源码为官方公开 tarball，配合上述构建脚本与
            CMake 工程可完整重建含任一 FFmpeg 版本的 sparkcap.dll

系统依赖（随 Windows 分发，非本库分发物）
  Media Foundation / DirectShow / D3D11 / Winsock / schannel —— Windows SDK
  MSVC CRT（/MT 静态链接）—— 按微软 Visual Studio 再分发条款

测试基建（不随产品二进制分发，无许可传递）
  MediaMTX、ffmpeg 命令行 —— 仅测试环境使用，见 docs/testing-guide.md §6
