#!/bin/bash
# 构建环境就绪检查（docs/SparkCap-编译环境安装指南.md §6）
# 用法：x64 Native Tools 环境下 msys2_shell.cmd -no-start -use-full-path <本脚本>
set -e
echo "== MSYS: $(uname -s)"
echo "== cl:   $(which cl)"
cl 2>&1 | head -2
echo "== make: $(make --version | head -1)"
echo "== nasm: $(nasm -v)"
echo "== diff: $(diff --version | head -1)"
echo "ENV OK"
