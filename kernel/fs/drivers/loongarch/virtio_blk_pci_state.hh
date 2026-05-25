#pragma once

#ifdef LOONGARCH

#include "types.hh"

/**
 * @brief LoongArch virtio-blk PCI 发现阶段共享的设备位置信息。
 *
 * PCI 扫描代码与 virtio 块设备适配层都会访问这组状态。
 * 这里单独抽出声明，避免再通过旧头文件的副作用耦合到一起。
 */
extern unsigned char bus1;
extern unsigned char device1;
extern unsigned char function1;
extern unsigned char bus2;
extern unsigned char device2;
extern unsigned char function2;

extern uint64 pci_base1;
extern uint64 pci_base2;

#endif
