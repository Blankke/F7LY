

#ifdef LOONGARCH
#include "trap/loongarch/pci.h"
#include "virtio_pci.hh"
#include "fs/buf.hh"
#include "fs/drivers/riscv/virtio2.hh"


pci_device_t pci_device_table[PCI_MAX_DEVICE_NR];/*存储设备信息的结构体数组*/

void pci_read_config(unsigned long base_cfg_addr, unsigned int bus, unsigned int device, unsigned int function, unsigned int reg_id, unsigned int * read_data)
{
    unsigned long pcie_header_base = DMWIN1_MASK|base_cfg_addr| (bus << 16) | (device << 11)| (function<<8);

    *read_data = *(volatile unsigned int *)(pcie_header_base + reg_id) ;

}

void pci_write_config(unsigned long base_cfg_addr, unsigned int bus, unsigned int device, unsigned int function, unsigned int reg_id, unsigned int  write_data)
{
	unsigned long pcie_header_base = DMWIN1_MASK|base_cfg_addr| (bus << 16) | (device << 11)| (function<<8);


	*(volatile unsigned int *)( pcie_header_base + reg_id) = write_data;
	// pr_info("curr *val: 0x%x\n", *val);

}

/*创建一个pci设备信息结构体*/
pci_device_t* pci_alloc_device()
{
    int i;
    for (i = 0; i < PCI_MAX_DEVICE_NR; i++) {
        if (pci_device_table[i].flags == PCI_DEVICE_INVALID) {
            pci_device_table[i].flags = PCI_DEVICE_USING;
            return &pci_device_table[i];
        }
    }
    return NULL;
}

/*初始化pci设备信息*/
static void pci_device_init(
							pci_device_t *device,
							unsigned char bus,
							unsigned char dev,
							unsigned char function,
							unsigned short vendor_id,
							unsigned short device_id,
							unsigned int class_code,
							unsigned char revision_id,
							unsigned char multi_function
						   ) {
	/*设置驱动设备的信息*/
	device->bus = bus;
	device->dev = dev;
	device->function = function;

	device->vendor_id = vendor_id;
	device->device_id = device_id;
	device->multi_function = multi_function;
	device->class_code = class_code;
	device->revision_id = revision_id;
	int i;
	for (i = 0; i < PCI_MAX_BAR; i++) {
		device->bar[i].type = PCI_BAR_TYPE_INVALID;
	}
	device->irq_line = -1;
}

static void pci_device_bar_init(pci_device_bar_t *bar, unsigned int addr_reg_val, unsigned int len_reg_val)
{
	/*if addr is 0xffffffff, we set it to 0*/
	if (addr_reg_val == 0xffffffff) {
		addr_reg_val = 0;
	}
	/*bar寄存器中bit0位用来标记地址类型，如果是1则为io空间，若为0则为mem空间*/
	if (addr_reg_val & 1) {
		/*I/O元基地址寄存器:
		 *Bit1:保留
		 *Bit31-2:RO,基地址单元
		 *Bit63-32:保留
		 */
		bar->type = PCI_BAR_TYPE_IO;
		bar->base_addr = addr_reg_val  & PCI_BASE_ADDR_IO_MASK;
		bar->length    = ~(len_reg_val & PCI_BASE_ADDR_IO_MASK) + 1;
	} else {
		/*MEM 基地址存储器:
		  Bit2-1:RO,MEM 基地址寄存器-译码器宽度单元,00-32 位,10-64 位
Bit3:RO,预提取属性
Bit64-4:基地址单元
*/
		bar->type = PCI_BAR_TYPE_MEM;
		bar->base_addr = addr_reg_val  & PCI_BASE_ADDR_MEM_MASK;
		bar->length    = ~(len_reg_val & PCI_BASE_ADDR_MEM_MASK) + 1;
	}
}



void pci_scan_device(unsigned char bus, unsigned char device, unsigned char function) {
    unsigned int val;
    // pr_info("read config a\n");
    pci_read_config(PCI_CONFIG0_BASE,bus, device, function, PCI_DEVICE_VENDER,(uint *)&val);
    unsigned int vendor_id = val & 0xffff;
    unsigned int device_id = val >> 16;
    if (vendor_id == 0xffff) {
        return;
    }
    pci_device_t *pci_dev = pci_alloc_device();
    if(pci_dev == NULL){
        return;
    }


    pci_read_config(PCI_CONFIG0_BASE,bus, device, function, PCI_BIST_HEADER_TYPE_LATENCY_TIMER_CACHE_LINE,(uint *)&val);
    unsigned char header_type = ((val >> 16));
    pci_read_config(PCI_CONFIG0_BASE,bus, device, function, PCI_STATUS_COMMAND,(uint *)&val);
    pci_dev->command = val & 0xffff;
    pci_dev->status = (val >> 16) & 0xffff;

    pci_read_config(PCI_CONFIG0_BASE,bus, device, function, PCI_CLASS_CODE_REVISION_ID,(uint *)&val);
	unsigned int classcode = val >> 8;
	unsigned char revision_id = val & 0xff;

	/*初始化pci设备*/
	pci_device_init(pci_dev, bus, device, function, vendor_id, device_id, classcode, revision_id, (header_type & 0x80));

	/*初始化设备的bar*/
	int bar, reg;
	for (bar = 0; bar < PCI_MAX_BAR; bar++) {//遍历六个地址寄存器
		reg = PCI_BASS_ADDRESS0 + (bar*4);
		// pr_info("bar %d\n", bar);
		/*获取地址值*/
		// pr_info("reg %d\n", reg);

		// if (bar == 0 && reg == 16 && bus == 0 && device == 13 && function == 1)
		// 	continue;
		// if (bar == 0 && reg == 16 && bus == 0 && device == 14 && function == 1)
		// 	continue;

		pci_read_config(PCI_CONFIG0_BASE,bus, device, function, reg, &val);
		// if (bar == 0 && reg == 16 && bus == 0 && device == 13 && function == 1)
		// 	pr_info("val: 0x%x\n", val);
		/*设置bar寄存器为全1禁用此地址，在禁用后再次读取读出的内容为地址空间的大小*/
		pci_write_config(PCI_CONFIG0_BASE,bus, device, function, reg, 0xffffffff);
		// pr_info("pci_write_config down\n");

		/* bass address[0~5] 获取地址长度*/
		unsigned int len;
		pci_read_config(PCI_CONFIG0_BASE,bus, device, function, reg,&len);
		/*pci_write_config 将io/mem地址返回到confige空间*/
		pci_write_config(PCI_CONFIG0_BASE,bus, device, function, reg, val);
		// pr_info("pci_write_config down\n");
		/*init pci device bar*/
		if (len != 0 && len != 0xffffffff) {
			pci_device_bar_init(&pci_dev->bar[bar], val, len);
		}
	}

	/* 获取 card bus CIS 指针 */
	pci_read_config(PCI_CONFIG0_BASE,bus, device, function, PCI_CARD_BUS_POINTER,&val);
	pci_dev->card_bus_pointer = val;

	/* 获取子系统设备 ID 和供应商 ID */
	pci_read_config(PCI_CONFIG0_BASE,bus, device, function, PCI_SUBSYSTEM_ID,&val);
	pci_dev->subsystem_vendor_id = val & 0xffff;
	pci_dev->subsystem_device_id = (val >> 16) & 0xffff;

	/* 获取扩展ROM基地址 */
	pci_read_config(PCI_CONFIG0_BASE,bus, device, function, PCI_EXPANSION_ROM_BASE_ADDR,&val);
	pci_dev->expansion_rom_base_addr = val;

	/* 获取能力列表 */
	pci_read_config(PCI_CONFIG0_BASE,bus, device, function, PCI_CAPABILITY_LIST,&val);
	pci_dev->capability_list = val;

	/*获取中断相关的信息*/
	pci_read_config(PCI_CONFIG0_BASE,bus, device, function, PCI_MAX_LNT_MIN_GNT_IRQ_PIN_IRQ_LINE,&val);
	if ((val & 0xff) > 0 && (val & 0xff) < 32) {
		unsigned int irq = val & 0xff;
		pci_dev->irq_line = irq;
		pci_dev->irq_pin = (val >> 8)& 0xff;
	}
	if (vendor_id == virtio_blk_vendor && device_id == virtio_blk_device) {
		printf("%d %d\n", pci_dev->irq_line, pci_dev->irq_pin);
	}
	pci_dev->min_gnt = (val >> 16) & 0xff;
	pci_dev->max_lat = (val >> 24) & 0xff;
}

/*扫描*/
void pci_scan_buses()
{
    unsigned int bus;
    unsigned char device, function;
    /*扫描每一条总线上的设备*/
    for (bus = 0; bus < PCI_MAX_BUS; bus++) {//遍历总线
        for (device = 0; device < PCI_MAX_DEV; device++) {//遍历总线上的每一个设备
            for (function = 0; function < PCI_MAX_FUN; function++) {//遍历每个功能号
                // pr_info("bus: %d, device: %d, function: %d\n",bus ,device ,function);
                pci_scan_device(bus, device, function);
            }
        }
    }
    printf("pci_scan_buses done\n");
}

pci_device_t* pci_get_device(unsigned int vendor_id, unsigned int device_id)
{
	int i;
	pci_device_t* device;

	for (i = 0; i < PCI_MAX_DEVICE_NR; i++) {
		device = &pci_device_table[i];

		if (device->flags == PCI_DEVICE_USING &&
			device->vendor_id == vendor_id &&
			device->device_id == device_id) {
			return device;
			}
	}
	return NULL;
}

/*通过主线号，设备号，功能号寻找设备信息*/
pci_device_t* pci_get_device_by_bus(unsigned int bus, unsigned int dev,unsigned int function){
	if (bus>PCI_MAX_BUS|| dev>PCI_MAX_DEV || function>PCI_MAX_FUN)
	{
		return NULL;
	}
	pci_device_t* tmp;
	for (int i = 0; i < PCI_MAX_DEVICE_NR; i++) {
		tmp = &pci_device_table[i];
		if (
			tmp->bus == bus &&
			tmp->dev == dev&&
			tmp->function == function) {
			return tmp;
			}
	}
	return NULL;
}

void pci_init()
{
    printf("pci_init start\n");
    /*初始化pci设备信息结构体*/
    int i;
    for (i = 0; i < PCI_MAX_DEVICE_NR; i++) {
        pci_device_table[i].flags = PCI_DEVICE_INVALID;
    }
    /*扫描所有总线设备*/
    pci_scan_buses();
    printf("scan done\n");
}

//----------------------------

#define PCI_ADDR(addr)  (PCIE0_ECAM + (uint64)(addr))
#define PCI_ADDR_V(addr) (PCIE0_ECAM_V + (uint64)(addr))
#define PCI_REG8(reg)  (*(volatile uint8 *)PCI_ADDR_V(reg))
#define PCI_REG16(reg)  (*(volatile uint16 *)PCI_ADDR_V(reg))
#define PCI_REG32(reg)  (*(volatile uint32 *)PCI_ADDR_V(reg))
#define PCI_REG64(reg)  (*(volatile uint64 *)PCI_ADDR_V(reg))

void pci_config_read(void *buf, uint64 len, uint64 offset)
{
	volatile uint8 *dst = (volatile uint8 *)buf;
	while (len) {
		*dst = PCI_REG8(offset);
		--len;
		++dst;
		++offset;
	}
}


uint8 pci_config_read8(uint64 offset)
{
	return PCI_REG8(offset);
}

uint16 pci_config_read16(uint64 offset)
{
	return PCI_REG16(offset);
}

uint32 pci_config_read32(uint64 offset)
{
	return PCI_REG32(offset);
}

uint64 pci_config_read64(uint64 offset)
{
	return PCI_REG64(offset);
}

void pci_config_write8(uint32 offset, uint8 val)
{
	PCI_REG8(offset) = val;
}

void pci_config_write16(uint32 offset, uint16 val)
{
	PCI_REG16(offset) = val;
}

void pci_config_write32(uint32 offset, uint32 val)
{
	PCI_REG32(offset) = val;
}


#define PAGE_ALIGN(sz)	((((uint64)sz) + 0x0fff) & ~0x0fff)
uint64 pci_alloc_mmio(uint64 sz)
{
	static uint64 s_off = 0;           // static 变量, 共用一个
	// MMIO 区域 + s_off
	// uint64 addr = PHYSTOP + s_off + 2 * PGSIZE;
	uint64 addr = PCIE0_MMIO_V + s_off;
	s_off += PAGE_ALIGN(sz);        // 按页 4k 对齐
	// printf("addr: 0x%016llx, 0x%016llx\n", addr, s_off);
	return addr;
}

uint64 pci_device_probe(uint16 vendor_id, uint16 device_id)
{
    uint32 pci_id = (((uint32)device_id) << 16) | vendor_id;
    uint64 ret = 0;

    for (int bus = 0; bus < 255; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            int func = 0;
            int offset = 0;
            uint64 off = (bus << 16) | (dev << 11) | (func << 8) | (offset);
            volatile uint32 *base = (volatile uint32 *)PCI_ADDR(off);
            uint32 id = base[0];   // device_id + vendor_id

            if (id != pci_id) continue;

            ret = off;

            // command and status register.
            // bit 0 : I/O access enable
            // bit 1 : memory access enable
            // bit 2 : enable mastering
            base[1] = 7;
            __sync_synchronize();
        	if (bus1 == bus && device1 == dev && function1 == func) {
        		continue;
        	} else {
        		if (!bus1 && !device1 && !function1) {
        			bus1 = bus;
        			device1 = dev;
        			function1 = func;
        		} else {
        			bus2 = bus;
        			device2 = dev;
        			function2 = func;
        		}
        	}

        	printf("%d %d %d\n", bus, dev, func);
        	goto out;

        }
    }
	// void* base_addr = PCIE0_MMIO;
	// for (int i = 0;i < 4096;i+=4) {
	// 	printf("%x ", *(volatile uint32 *)((uint64)base_addr + i));
	// }
	// printf("vendor_id: 0x%x\n", vendor_id);
	// printf("device_id: 0x%x\n", device_id);
	// printf("bar_addr : 0x%x\n", ret);          // ECAM 中的 offset
out:
    return ret;
}

uint32 pci_alloc_irq_number(void)
{
	static uint32 n_off = 0;   // Static 从 0x80 开始分配 irq num

	uint32 irq = APLIC_MSIX0_IRQ + n_off;
	n_off += 1;
	return irq;
}


#endif