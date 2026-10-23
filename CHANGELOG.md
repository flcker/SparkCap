# Changelog

本项目的所有显著变更记录于此。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### Added

- 工程骨架：CMake 构建（`cmake --preset default` → `--preset release|debug`，VS2022 x64、静态 CRT /MT、/W4 /WX、/utf-8）
- 公共头 `include/sparkcap.h` + `include/sparkcap_types.h`：12 个 C 门面 API、13 个错误码枚举、像素格式/后端枚举、帧与设备信息结构、帧回调类型（Spec §4 的代码化）
- `src/facade.cpp` stub 实现（接口全部返回 `SPARKCAP_ERR_UNSUPPORTED`，`sparkcap_version` 除外）
- `src/sparkcap.def` 导出白名单；dumpbin 验证通过：导出表恰好 12 符号、依赖仅 KERNEL32.dll
- `AGENTS.md` 项目约定（红线规则、构建与提交前验证、许可警示）、`CLAUDE.md`、`README.md`、`CHANGELOG.md`
- `third_party/ffmpeg/VERSION` 版本记录占位（M3 前锁定 tag + commit hash）

### Changed

- 三份设计文档 v1.0 → v1.1（评审修订）：
  - 接口表补 `sparkcap_set_frame_callback`（F5 回调模式此前无 API 支撑）与 `sparkcap_version`；
  - 错误码由裸数字改为 `SPARKCAP_ERR_*` 符号名并从 9 个扩至 13 个（新增设备占用、句柄无效、内存不足、回调内禁止）；
  - 补回调-stop 竞态语义（Spec §4.4）与调用顺序状态机（§4.5）；
  - 修正命名规范 `/NOIMPLIB` 与 .lib 隐式链接的矛盾，明确 `SPARKCAP_API` 宏与 `.def` 分工；
  - 修正 PixelFormat 归类（MJPG 为压缩格式、H264 移出输出侧、RGB32 限设备原生输出）；
  - 补 A8 多设备并发验收、A7 dumpbin 命令具体化、待确认事项补热插拔与日志回调
- 工具链要求拆为两层：主工程放宽为 VS2022 任意小版本（v143 ≥ 14.30，CMake 配置期检查 `MSVC_VERSION >= 1930`）；FFmpeg 构建锁定 v14.44（由维护者执行，14.x 二进制兼容保证旧版 VS2022 可链接）
