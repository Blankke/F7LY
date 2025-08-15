#!/usr/bin/env python3
"""
将解析后的测例列表转换为 C++ 数组格式
输入格式：测例名字 状态 备注
输出格式：{"testcase", true/false, true/false},
"""

import sys
import re

def parse_input_line(line):
    """解析输入行，提取测例名字和状态"""
    line = line.strip()
    if not line or line.startswith('-') or line.startswith('测例名字'):
        return None
    
    # 使用正则表达式匹配格式：测例名字 状态 备注
    # 测例名字可能包含特殊字符，状态是 pass 或空白
    match = re.match(r'^(\S+)\s+(pass|\s*)\s*(.*)', line)
    if match:
        testcase = match.group(1)
        status = match.group(2).strip()
        # 如果状态为空，则视为失败
        status = "pass" if status == "pass" else "fail"
        comment = match.group(3).strip()
        return testcase, status, comment
    return None

def status_to_bool(status):
    """将状态转换为布尔值"""
    return "true" if status == "pass" else "false"

def generate_cpp_array(results):
    """生成 C++ 数组格式"""
    print("// LTP 测例数组")
    print("// 格式: {测例名字, 是否通过, 是否启用}")
    print("const std::vector<std::tuple<std::string, bool, bool>> ltp_tests = {")
    
    for i, (testcase, status, comment) in enumerate(results):
        is_pass = status_to_bool(status)
        # 第二个布尔值表示是否启用，这里默认设置为 true
        # 你可以根据需要修改这个逻辑
        is_enabled = "true"
        
        # 添加逗号，最后一行不加
        comma = "," if i < len(results) - 1 else ""
        
        # 如果有注释，添加为行内注释
        comment_str = f" // {comment}" if comment else ""
        
        print(f'    {{"{testcase}", {is_pass}, {is_enabled}}}{comma}{comment_str}')
    
    print("};")

def main():
    """主函数"""
    # 如果提供了文件参数，从文件读取；否则从stdin读取
    if len(sys.argv) > 1:
        filename = sys.argv[1]
        try:
            with open(filename, 'r', encoding='utf-8') as f:
                lines = f.readlines()
        except FileNotFoundError:
            print(f"错误: 文件 '{filename}' 不存在", file=sys.stderr)
            sys.exit(1)
    else:
        print("请输入测例数据 (格式: 测例名字 状态 备注)，按 Ctrl+D 结束:", file=sys.stderr)
        lines = sys.stdin.readlines()
    
    results = []
    for line in lines:
        parsed = parse_input_line(line)
        if parsed:
            results.append(parsed)
    
    if not results:
        print("错误: 没有找到有效的测例数据", file=sys.stderr)
        sys.exit(1)
    
    generate_cpp_array(results)
    
    # 输出统计信息到 stderr
    print(f"\n// 统计信息:", file=sys.stderr)
    print(f"// 总计: {len(results)} 个测例", file=sys.stderr)
    pass_count = sum(1 for _, status, _ in results if status == "pass")
    print(f"// 通过: {pass_count} 个", file=sys.stderr)
    print(f"// 失败: {len(results) - pass_count} 个", file=sys.stderr)

if __name__ == "__main__":
    main()
