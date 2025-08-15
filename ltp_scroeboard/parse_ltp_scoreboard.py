#!/usr/bin/env python3
"""
解析 ltp_scoreboard 文件，生成三列格式的输出：
第一列：测例名字
第二列：是否通过（pass/fail）
第三列：备注
"""

import re
import sys

def parse_ltp_scoreboard(filename):
    """解析 ltp_scoreboard 文件"""
    results = []
    
    with open(filename, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # 匹配所有行，包括注释和非注释的
    lines = content.split('\n')
    
    for line in lines:
        line = line.strip()
        if not line:
            continue
            
        # 检查是否是注释行
        is_commented = line.strip().startswith('//')
        
        # 提取测例名字和注释
        if is_commented:
            # 注释行：// "testcase", 或 // "testcase",  // 注释内容
            match = re.search(r'//\s*"([^"]+)"', line)
            if match:
                testcase = match.group(1)
                # 提取注释内容（如果有的话）
                comment_match = re.search(r'//\s*"[^"]+"\s*,?\s*//\s*(.+)', line)
                comment = comment_match.group(1).strip() if comment_match else ""
                results.append((testcase, "", comment))
        else:
            # 非注释行："testcase", 或 "testcase", // 注释内容
            match = re.search(r'"([^"]+)"', line)
            if match:
                testcase = match.group(1)
                # 提取注释内容（如果有的话）
                comment_match = re.search(r'//\s*(.+)', line)
                comment = comment_match.group(1).strip() if comment_match else ""
                results.append((testcase, "pass", comment))
    
    return results

def main():
    if len(sys.argv) != 2:
        print("Usage: python3 parse_ltp_scoreboard.py <ltp_scoreboard_file>")
        sys.exit(1)
    
    filename = sys.argv[1]
    
    try:
        results = parse_ltp_scoreboard(filename)
        
        # 输出表头
        print(f"{'测例名字':<30} | {'状态':<10} | 备注")
        print("-" * 80)
        
        # 输出结果
        for testcase, status, comment in results:
            print(f"{testcase:<30} | {status:<10} | {comment}")
            
        print(f"\n总计: {len(results)} 个测例")
        pass_count = sum(1 for _, status, _ in results if status == "pass")
        fail_count = len(results) - pass_count
        print(f"通过: {pass_count} 个")
        print(f"失败: {fail_count} 个")
        
    except FileNotFoundError:
        print(f"错误: 文件 '{filename}' 不存在")
        sys.exit(1)
    except Exception as e:
        print(f"错误: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
