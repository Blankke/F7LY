#!/bin/bash

# 生成文件数组（排除.sh文件）
files_array=()
for file in *; do
    if [[ -f "$file" && ! "$file" =~ \.sh$ ]]; then
        files_array+=("$file")
    fi
done

# 输出数组声明代码
echo "files_array=("
for file in "${files_array[@]}"; do
    echo "    \"$file\""
done
echo