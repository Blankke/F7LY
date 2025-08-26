# 分工文档

## 第一次分工
- **需要参考的代码分为三部分**：
    - 原版xv6的c代码实现部分，参考华科adddOS的文件，
    - xv6的模块化以及逐步实现，参考华东师大的内核编写指导
    - xv6的c++lize，参考学长代码的模块设计
- **要做的事情**：
    - 从内核编写指导的第一步开始，把启动的部分用c++面向对象化。
    - 阅读内核编写指导的后续步骤每一步涉及的文件，并参考学长代码记录笔记，在步骤进行到相应位置时能够有思路书写。
    - 将defs.h中的函数变成相应类的成员函数。
- 两步同时进行，也就是一边写一边看，在5.5之前进行阶段性小结1，完成无用户态的内核编写。
- czc：步骤1
- zzy：步骤3
- gkq：步骤2



	# 以下是LoongArch可选参数（已注释）：
	# -drive file=rootfs.img,if=none,format=raw,id=x0
	# -device virtio-blk-pci,drive=x0,bus=virtio-mmio-bus.0
	# -no-reboot
	# -device virtio-net-pci,netdev=net0
	# -netdev user,id=net0,hostfwd=tcp::5555-:5555,hostfwd=udp::5555-:5555
	# -rtc base=utc
	# -drive file=disk-la.img,if=none,format=raw,id=x1
	# -device virtio-blk-pci,drive=x1,bus=virtio-mmio-bus.1