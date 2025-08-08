#!/usr/bin/env python3
"""
LTP测试结果分析脚本
用于分析ltp_rank.txt文件中所有pass的总数
"""

import re
import os
import sys

def analyze_ltp_results(filename="ltp_rank.txt"):
    """
    分析LTP测试结果文件
    
    Args:
        filename: LTP结果文件名
    
    Returns:
        dict: 包含分析结果的字典
    """
    if not os.path.exists(filename):
        print(f"错误: 文件 {filename} 不存在")
        sys.exit(1)
    
    total_pass = 0
    total_tests = 0
    test_count = 0
    failed_tests = []
    perfect_tests = []
    
    # 读取文件并分析
    with open(filename, 'r', encoding='utf-8') as file:
        lines = file.readlines()
    
    # 跳过标题行
    data_lines = lines[2:]  # 跳过标题和分割线
    
    for line in data_lines:
        line = line.strip()
        if not line:  # 跳过空行
            continue
            
        # 使用正则表达式解析每行数据
        # 格式: test_name | pass_count | total_count
        match = re.match(r'^(.+?)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*$', line)
        
        if match:
            test_name = match.group(1).strip()
            pass_count = int(match.group(2))
            test_total = int(match.group(3))
            
            total_pass += pass_count
            total_tests += test_total
            test_count += 1
            
            # 记录失败的测试
            if pass_count < test_total:
                failed_tests.append({
                    'name': test_name,
                    'pass': pass_count,
                    'total': test_total,
                    'fail': test_total - pass_count
                })
            
            # 记录完美通过的测试
            if pass_count == test_total and test_total > 0:
                perfect_tests.append({
                    'name': test_name,
                    'count': test_total
                })
    
    return {
        'total_pass': total_pass,
        'total_tests': total_tests,
        'test_count': test_count,
        'failed_tests': failed_tests,
        'perfect_tests': perfect_tests
    }

def print_summary(results):
    """打印分析结果摘要"""
    print("=" * 60)
    print("LTP 测试结果分析")
    print("=" * 60)
    print(f"总测试用例数量: {results['test_count']}")
    print(f"总测试次数: {results['total_tests']}")
    print(f"总通过次数: {results['total_pass']}")
    print(f"总失败次数: {results['total_tests'] - results['total_pass']}")
    
    if results['total_tests'] > 0:
        pass_rate = (results['total_pass'] / results['total_tests']) * 100
        print(f"总体通过率: {pass_rate:.2f}%")
    
    print(f"完美通过的测试用例数: {len(results['perfect_tests'])}")
    print(f"有失败的测试用例数: {len(results['failed_tests'])}")
    
    print("\n" + "=" * 60)

def print_detailed_analysis(results):
    """打印详细分析结果"""
    print("详细分析:")
    print("-" * 40)
    
    # 显示前10个失败最多的测试
    if results['failed_tests']:
        print("\n失败次数最多的测试用例 (前10个):")
        sorted_failed = sorted(results['failed_tests'], key=lambda x: x['fail'], reverse=True)
        for i, test in enumerate(sorted_failed[:10], 1):
            print(f"{i:2d}. {test['name']:<30} 失败: {test['fail']:3d} (通过: {test['pass']}/{test['total']})")
    
    # 显示前10个通过次数最多的完美测试
    if results['perfect_tests']:
        print("\n通过次数最多的完美测试用例 (前10个):")
        sorted_perfect = sorted(results['perfect_tests'], key=lambda x: x['count'], reverse=True)
        for i, test in enumerate(sorted_perfect[:10], 1):
            print(f"{i:2d}. {test['name']:<30} 通过: {test['count']:3d}")

def main():
    """主函数"""
    filename = "ltp_rank.txt"
    
    # 检查命令行参数
    if len(sys.argv) > 1:
        filename = sys.argv[1]
    
    # 分析结果
    results = analyze_ltp_results(filename)
    
    # 打印摘要
    print_summary(results)
    
    # 询问是否显示详细分析
    choice = input("\n是否显示详细分析? (y/n): ").lower().strip()
    if choice in ['y', 'yes', '是']:
        print_detailed_analysis(results)
    
    # 保存结果到文件
    save_choice = input("\n是否将结果保存到文件? (y/n): ").lower().strip()
    if save_choice in ['y', 'yes', '是']:
        output_file = "ltp_analysis_result.txt"
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write("LTP 测试结果分析\n")
            f.write("=" * 60 + "\n")
            f.write(f"总测试用例数量: {results['test_count']}\n")
            f.write(f"总测试次数: {results['total_tests']}\n")
            f.write(f"总通过次数: {results['total_pass']}\n")
            f.write(f"总失败次数: {results['total_tests'] - results['total_pass']}\n")
            
            if results['total_tests'] > 0:
                pass_rate = (results['total_pass'] / results['total_tests']) * 100
                f.write(f"总体通过率: {pass_rate:.2f}%\n")
            
            f.write(f"完美通过的测试用例数: {len(results['perfect_tests'])}\n")
            f.write(f"有失败的测试用例数: {len(results['failed_tests'])}\n")
        
        print(f"结果已保存到: {output_file}")

if __name__ == "__main__":
    main()
