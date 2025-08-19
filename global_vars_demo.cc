// 演示全局变量在内存中的存储位置
#include <cstdio>

// 全局变量示例

// 1. 初始化的全局变量 - 存储在 .data 段
int initialized_global = 42;
char initialized_array[10] = "hello";

// 2. 未初始化的全局变量 - 存储在 .bss 段  
int uninitialized_global;
char uninitialized_array[100];

// 3. 静态全局变量
static int static_initialized = 100;
static int static_uninitialized;

// 4. 常量全局变量 - 存储在 .rodata 段
const int const_global = 123;
const char const_string[] = "constant string";

// 5. 指针变量
int* global_ptr = nullptr;  // 指针本身在 .data 段（如果初始化）或 .bss 段（如果未初始化）

void print_addresses() {
    printf("=== 全局变量内存地址分析 ===\n\n");
    
    printf("1. .data 段 (初始化的全局变量):\n");
    printf("   initialized_global = %d, 地址: %p\n", initialized_global, &initialized_global);
    printf("   initialized_array = \"%s\", 地址: %p\n", initialized_array, initialized_array);
    printf("   static_initialized = %d, 地址: %p\n", static_initialized, &static_initialized);
    printf("   global_ptr = %p, 地址: %p\n", global_ptr, &global_ptr);
    
    printf("\n2. .bss 段 (未初始化的全局变量):\n");
    printf("   uninitialized_global = %d, 地址: %p\n", uninitialized_global, &uninitialized_global);
    printf("   uninitialized_array[0] = %d, 地址: %p\n", uninitialized_array[0], uninitialized_array);
    printf("   static_uninitialized = %d, 地址: %p\n", static_uninitialized, &static_uninitialized);
    
    printf("\n3. .rodata 段 (只读数据):\n");
    printf("   const_global = %d, 地址: %p\n", const_global, &const_global);
    printf("   const_string = \"%s\", 地址: %p\n", const_string, const_string);
    
    // 局部变量对比 - 存储在栈上
    int local_var = 999;
    printf("\n4. 栈上的局部变量 (对比):\n");
    printf("   local_var = %d, 地址: %p\n", local_var, &local_var);
}

int main() {
    print_addresses();
    return 0;
}
