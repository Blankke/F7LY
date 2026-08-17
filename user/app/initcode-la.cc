#include "user.hh"

// 入口只负责按挂载的根文件系统分发测试集，判别与测试集定义在
// user/user_lib/initcode_dispatch.cc，双架构共用同一份逻辑。
extern "C"
{
    __attribute__((section(".text.startup"))) int main()
    {
        if (is_final_disk())
        {
            printf("[initcode] 检测到决赛盘（oscomp-final），执行 final_test\n");
            final_test();
        }
        else if (is_preliminary_disk())
        {
            printf("[initcode] 检测到初赛盘（oscomp-preliminary），执行 pre_test\n");
            pre_test();
        }
        else
        {
            printf("[initcode] 无法识别挂载的根文件系统（既无决赛脚本也无初赛 benchmark），跳过全部测试\n");
        }
        shutdown();
        return 0;
    }
}
