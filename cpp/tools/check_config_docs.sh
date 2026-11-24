#!/usr/bin/env bash
# 检查 Config.cpp 中解析的每个配置项都在 streaming.conf.example 中有说明（防止文档与代码脱节）。
set -euo pipefail
cd "$(dirname "$0")/.."
missing=0
for k in $(grep -o 'key == "[A-Za-z_]*"' src/config/Config.cpp | sed 's/key == "//; s/"//' | sort -u); do
    if ! grep -qE "^#? ?$k=" streaming.conf.example; then
        echo "配置项 $k 未在 streaming.conf.example 中说明"
        missing=$((missing + 1))
    fi
done
if [ "$missing" -gt 0 ]; then
    echo "共 $missing 个配置项缺少说明"
    exit 1
fi
echo "配置文档与代码一致（$(grep -o 'key == "[A-Za-z_]*"' src/config/Config.cpp | sort -u | wc -l) 个配置项）"
