# #!/bin/bash

# # 查找所有 C/C++ 文件（.c, .cpp, .h, .hpp）
# files=$(find . -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) )

# # 存储处理过的文件名
# processed_files=()

# # 遍历每个文件
# while IFS= read -r file; do
#     # 检查是否包含 stdio.h、stdlib.h 或 string.h
#     if grep -q -E '#include\s*<(stdio|stdlib|string)\.h(pp)?>' "$file"; then
#         echo "处理文件: $file"
#         processed_files+=("$file")  # 记录文件名

#         # 在第一个 #include 前添加 "libs/klib.hh"
#         sed -i '1i #include "libs/klib.hh"' "$file"

#         # 删除 stdio.h、stdlib.h 和 string.h
#         sed '/#include\s*<\(stdio\|stdlib\|string\)\.h>/d' "$file"
#     fi
# done <<< "$files"

# # 输出处理过的文件列表
# echo -e "\n已处理的文件："
# printf '%s\n' "${processed_files[@]}"
# echo -e "\n总计处理文件数: ${#processed_files[@]}"
# echo "完成！"


#!/bin/bash

echo "开始查找 C/C++ 文件..."

# 查找所有 C/C++ 文件（.c, .cpp, .h, .hpp）
# 使用 -print0 选项，它会用 null 字符（ASCII 0）来分隔文件名，而不是换行符。
# 这样可以正确处理文件名中包含空格或换行符的情况。
find . -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -print0 | \
while IFS= read -r -d '' file; do
    # 检查是否包含 stdio.h、stdlib.h 或 string.h (包括 .h 和 .hpp)
    if grep -q -E '#include\s*<(stdio|stdlib|string)\.h(pp)?>' "$file"; then
        echo "处理文件: $file"
        processed_files+=("$file")  # 记录文件名

        # 删除 stdio.h、stdlib.h 和 string.h (包括 .h 和 .hpp)
        # 确保使用 -i 选项进行文件修改
        sed -i -E '/#include\s*<(stdio|stdlib|string)\.h(pp)?>/d' "$file"

        # 在第一个 #include 前添加 "libs/klib.hh"
        # 寻找第一个 #include 行，并在其前插入
        # 如果文件中没有 #include，则默认插入到文件首行
        first_include_line=$(grep -n '#include' "$file" | head -n 1 | cut -d: -f1)
        if [ -n "$first_include_line" ]; then
            # 找到 #include，在其之前插入
            sed -i "${first_include_line}i #include \"libs/klib.hh\"" "$file"
        else
            # 没有找到 #include，在文件开头插入
            sed -i '1i #include "libs/klib.hh"' "$file"
        fi
    fi
done

# 输出处理过的文件列表
echo -e "\n--- 处理结果 ---"
if [ ${#processed_files[@]} -eq 0 ]; then
    echo "没有文件需要处理。"
else
    echo "已处理的文件列表："
    printf '%s\n' "${processed_files[@]}"
fi
echo -e "\n总计处理文件数: ${#processed_files[@]}"
echo "脚本执行完成！"