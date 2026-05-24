#!/bin/bash

# 从文件读取并分析LTP测试输出的评测脚本
# 使用方法: ./analyze_output.sh <输出文件路径>

# 检查参数
if [ $# -ne 1 ]; then
    echo "Usage: $0 <output_file_path>"
    echo "Example: $0 /home/kidszz/F7LY/output"
    exit 1
fi

OUTPUT_FILE="$1"

# 检查文件是否存在
if [ ! -f "$OUTPUT_FILE" ]; then
    echo "Error: File '$OUTPUT_FILE' not found!"
    exit 1
fi

echo "=== LTP Output Analysis Tool ==="
echo "Analyzing file: $OUTPUT_FILE"
echo ""

# 创建临时文件存储结果
temp_results=$(mktemp)
temp_analysis=$(mktemp)

# 创建分析结果文件
analysis_file="analysis_results.txt"
rank_file="analysis_rank.txt"
> "$analysis_file"  # 清空文件

echo "Processing test output..."

# 分析输出文件
{
    echo "=== Analysis of LTP Test Output ==="
    echo "Source file: $OUTPUT_FILE"
    echo "Analysis time: $(date)"
    echo ""
    
    # 查找测试用例开始标记
    echo "=== Test Cases Found ==="
    test_cases=$(grep -E "^RUN LTP CASE" "$OUTPUT_FILE" | wc -l)
    echo "Total test cases found: $test_cases"
    echo ""
    
    if [ $test_cases -eq 0 ]; then
        echo "Warning: No LTP test cases found in the output file!"
        exit 1
    fi
    
    # 使用Python脚本解析完整输出
    echo "Parsing test results using judge_ltp_musl.py..."
    cat "$OUTPUT_FILE" | python3 judge_ltp_musl.py 2>/dev/null > "$temp_analysis"
    
    # 检查解析结果
    if [ ! -s "$temp_analysis" ] || [ "$(cat "$temp_analysis")" = "[]" ]; then
        echo "Warning: Failed to parse test results with judge_ltp_musl.py"
        echo "Performing manual analysis..."
        
        # 手动分析
        manual_analysis_count=0
        manual_pass_total=0
        manual_fail_total=0
        
        # 直接分析 Summary 部分
        while IFS= read -r line; do
            if echo "$line" | grep -q "^Summary:"; then
                manual_analysis_count=$((manual_analysis_count + 1))
                # 读取下一行的passed数量
                read -r next_line
                if echo "$next_line" | grep -q "^passed"; then
                    passed_count=$(echo "$next_line" | awk '{print $2}')
                    manual_pass_total=$((manual_pass_total + passed_count))
                fi
                # 读取failed行
                read -r fail_line
                if echo "$fail_line" | grep -q "^failed"; then
                    failed_count=$(echo "$fail_line" | awk '{print $2}')
                    manual_fail_total=$((manual_fail_total + failed_count))
                fi
            fi
        done < "$OUTPUT_FILE"
        
        echo "Manual analysis results:"
        echo "- Test cases with summary: $manual_analysis_count"
        echo "- Total passed: $manual_pass_total"
        echo "- Total failed: $manual_fail_total"
        
        # 创建简单的结果格式
        echo "manual_analysis $manual_pass_total $((manual_pass_total + manual_fail_total))" >> "$temp_results"
    else
        echo "Successfully parsed results!"
        
        # 解析JSON结果并转换为简单格式
        python3 -c "
import sys
import json
try:
    with open('$temp_analysis', 'r') as f:
        data = json.loads(f.read())
    
    total_pass = 0
    total_all = 0
    
    with open('$temp_results', 'w') as out_file:
        for item in data:
            name = item.get('name', 'unknown')
            pass_count = item.get('pass', 0)
            all_count = item.get('all', 0)
            total_pass += pass_count
            total_all += all_count
            out_file.write(f'{name} {pass_count} {all_count}\n')
        
        print(f'Total cases: {len(data)}')
        print(f'Total passed: {total_pass}')
        print(f'Total tests: {total_all}')
        print(f'Pass rate: {(total_pass/total_all*100):.2f}%' if total_all > 0 else 'Pass rate: 0%')

except Exception as e:
    print(f'Error processing JSON: {e}')
    sys.exit(1)
"
    fi
    
    echo ""
    echo "=== Individual Test Case Results ==="
    if [ -s "$temp_results" ]; then
        while read -r name pass_count all_count; do
            if [ $all_count -gt 0 ]; then
                pass_rate=$(echo "scale=2; $pass_count * 100 / $all_count" | bc -l 2>/dev/null || echo "0")
                printf "%-30s | Pass: %-4s | Total: %-4s | Rate: %s%%\n" "$name" "$pass_count" "$all_count" "$pass_rate"
            else
                printf "%-30s | Pass: %-4s | Total: %-4s | Rate: N/A\n" "$name" "$pass_count" "$all_count"
            fi
        done < "$temp_results"
    else
        echo "No detailed results available."
    fi
    
    echo ""
    echo "=== Overall Statistics ==="
    
    # 计算总体统计
    total_cases=$(wc -l < "$temp_results" 2>/dev/null || echo "0")
    total_pass_sum=$(awk '{sum += $2} END {print sum}' "$temp_results" 2>/dev/null || echo "0")
    total_all_sum=$(awk '{sum += $3} END {print sum}' "$temp_results" 2>/dev/null || echo "0")
    
    echo "Total test cases analyzed: $total_cases"
    echo "Total passed tests: $total_pass_sum"
    echo "Total tests executed: $total_all_sum"
    
    if [ $total_all_sum -gt 0 ]; then
        overall_rate=$(echo "scale=2; $total_pass_sum * 100 / $total_all_sum" | bc -l 2>/dev/null || echo "0")
        echo "Overall pass rate: ${overall_rate}%"
    else
        echo "Overall pass rate: N/A"
    fi
    
    # 查找失败的测试
    echo ""
    echo "=== Failed Tests Analysis ==="
    failed_count=$(grep -c "TFAIL" "$OUTPUT_FILE" 2>/dev/null || echo "0")
    passed_count=$(grep -c "TPASS" "$OUTPUT_FILE" 2>/dev/null || echo "0")
    
    echo "TPASS count: $passed_count"
    echo "TFAIL count: $failed_count"
    
    if [ $failed_count -gt 0 ]; then
        echo ""
        echo "Sample failed tests:"
        grep "TFAIL" "$OUTPUT_FILE" | head -10 | while IFS= read -r line; do
            echo "  $line"
        done
        
        if [ $failed_count -gt 10 ]; then
            echo "  ... and $((failed_count - 10)) more failed tests"
        fi
    fi
    
    echo ""
    echo "=== System Information ==="
    # 提取系统信息
    if grep -q "Platform Name" "$OUTPUT_FILE"; then
        echo "QEMU Platform Information:"
        grep -E "(Platform Name|Platform Features|Firmware|Domain0)" "$OUTPUT_FILE" | head -10
    fi
    
    if grep -q "Boot HART" "$OUTPUT_FILE"; then
        echo ""
        echo "Boot Information:"
        grep -E "Boot HART" "$OUTPUT_FILE" | head -5
    fi
    
} > "$analysis_file"

# 创建排序后的结果文件
if [ -s "$temp_results" ]; then
    echo "Creating ranked results..."
    
    # 按pass数量排序（从高到低）
    sort -k2,2nr -k3,3nr "$temp_results" > temp_sorted.txt
    
    # 添加表头并保存到排名文件
    {
        echo "=== LTP Test Cases Ranking ==="
        echo "Analysis time: $(date)"
        echo "Source: $OUTPUT_FILE"
        echo ""
        printf "%-30s | %-10s | %-11s | %-10s\n" "Test Case Name" "Pass Count" "Total Count" "Pass Rate"
        echo "============================================================================"
        while read -r name pass_count all_count; do
            if [ $all_count -gt 0 ]; then
                pass_rate=$(echo "scale=1; $pass_count * 100 / $all_count" | bc -l 2>/dev/null || echo "0")
                printf "%-30s | %-10s | %-11s | %-10s%%\n" "$name" "$pass_count" "$all_count" "$pass_rate"
            else
                printf "%-30s | %-10s | %-11s | %-10s\n" "$name" "$pass_count" "$all_count" "N/A"
            fi
        done < temp_sorted.txt
    } > "$rank_file"
    
    echo ""
    echo "=== Top 10 Test Cases ==="
    head -15 "$rank_file" | tail -10
fi

echo ""
echo "=== Analysis Complete ==="
echo "Detailed analysis saved to: $analysis_file"
if [ -f "$rank_file" ]; then
    echo "Ranked results saved to: $rank_file"
fi

echo ""
echo "Quick summary:"
cat "$analysis_file" | grep -A 10 "=== Overall Statistics ==="

# 清理临时文件
rm -f "$temp_results" "$temp_analysis" temp_sorted.txt

echo ""
echo "Analysis finished successfully!"
