#pragma once
#ifdef LOONGARCH
#include "types.hh"
#include "trap/loongarch/pci.h"
#include "virtio_ring.hh"


/* Common configuration */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
/* Notifications */
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
/* ISR Status */
#define VIRTIO_PCI_CAP_ISR_CFG 3
/* Device specific configuration */
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
/* PCI configuration access */
#define VIRTIO_PCI_CAP_PCI_CFG 5
/* Shared memory region */
#define VIRTIO_PCI_CAP_SHARED_MEMORY_CFG 8
/* Vendor-specific data */
#define VIRTIO_PCI_CAP_VENDOR_CFG 9

#define le64 uint64
#define le32 uint32
#define le16 uint16
#define off_t uint32

struct virtio_pci_cap {
    uint8 cap_vndr; /* Generic PCI field: PCI_CAP_ID_VNDR */
    uint8 cap_next; /* Generic PCI field: next ptr. */
    uint8 cap_len; /* Generic PCI field: capability length */
    uint8 cfg_type; /* Identifies the structure. */
    uint8 bar; /* Where to find it. */
    uint8 id; /* Multiple capabilities of the same type */
    uint8 padding[2]; /* Pad to full dword. */
    le32 offset; /* Offset within bar. */
    le32 length; /* Length of the structure, in bytes. */
};

// 仅寻址超过 4GB 时, 使用
struct virtio_pci_cap64{
    struct virtio_pci_cap cap;
    uint32 offset_hi;
    uint32 length_hi;
};

struct virtio_pci_notify_cap {
    struct virtio_pci_cap cap;
    le32 notify_off_multiplier; /* Multiplier for queue_notify_off. */
};

struct virtio_pci_common_cfg {
    /* About the whole device. */
    le32 device_feature_select; /* read-write */
    le32 device_feature; /* read-only for driver */
    le32 driver_feature_select; /* read-write */
    le32 driver_feature; /* read-write */
    le16 config_msix_vector; /* read-write */
    le16 num_queues; /* read-only for driver */
    uint8 device_status; /* read-write */
    uint8 config_generation; /* read-only for driver */

    /* About a specific virtqueue. */
    le16 queue_select; /* read-write */
    le16 queue_size; /* read-write */
    le16 queue_msix_vector; /* read-write */
    le16 queue_enable; /* read-write */
    le16 queue_notify_off; /* read-only for driver */
    le64 queue_desc; /* read-write */
    le64 queue_driver; /* read-write avail ring */
    le64 queue_device; /* read-write used ring */
    le16 queue_notify_data; /* read-only for driver */
    le16 queue_reset;       /* read-write */
};

#undef le64
#undef le32
#undef le16
#undef off_t

typedef struct virtio_pci_hw {
    uint8      bar;
    uint8	    use_msix;
	//uint8      modern;
    uint32     notify_off_multiplier;
    void    *common_cfg;
    void    *isr_cfg;
    void    *device_cfg;
    void    *notify_cfg;
    struct pci_msix msix;
} virtio_pci_hw_t;

enum virtio_msix_status {
	VIRTIO_MSIX_NONE = 0,
	VIRTIO_MSIX_DISABLED = 1,
	VIRTIO_MSIX_ENABLED = 2
};



int virtio_pci_read_caps(virtio_pci_hw_t *hw, uint64 pci_base, trap_handler_fn *msix_isr);
uint64 virtio_pci_get_device_features(virtio_pci_hw_t *hw);
uint64 virtio_pci_get_driver_features(virtio_pci_hw_t *hw);
void virtio_pci_set_driver_features(virtio_pci_hw_t *hw, uint64 features);
uint16 virtio_pci_get_queue_size(virtio_pci_hw_t *hw, int qid);
void virtio_pci_set_queue_size(virtio_pci_hw_t *hw, int qid, int qsize);
void virtio_pci_set_queue_addr(virtio_pci_hw_t *hw, int qid, struct vring *vr);
void virtio_pci_set_queue_addr2(virtio_pci_hw_t *hw, int qid, void *desc, void *avail, void *used);
uint32 virtio_pci_get_queue_notify_off(virtio_pci_hw_t *hw, int qid);
void *virtio_pci_get_queue_notify_addr(virtio_pci_hw_t *hw, int qid);
void virtio_pci_set_queue_notify(virtio_pci_hw_t *hw, int qid);
void virtio_pci_set_queue_enable(virtio_pci_hw_t *hw, int qid);
uint16 virtio_pci_get_queue_enable(virtio_pci_hw_t *hw, int qid);
void virtio_pci_disable_queue_msix(virtio_pci_hw_t *hw, int qid);
void virtio_pci_set_queue_msix(virtio_pci_hw_t *hw, int qid, uint16 msix_vector);
void virtio_pci_disable_config_msix(virtio_pci_hw_t *hw);
void virtio_pci_set_config_msix(virtio_pci_hw_t *hw, uint16 msix_vector);
uint32 virtio_pci_clear_isr(virtio_pci_hw_t *hw);
uint8 virtio_pci_get_status(virtio_pci_hw_t *hw);
void virtio_pci_set_status(virtio_pci_hw_t *hw, uint8 status);
int virtio_pci_negotiate_driver_features(virtio_pci_hw_t *hw, uint64 features);
uint32 virtio_pci_get_config(virtio_pci_hw_t *hw, int offset, int size);
int virtio_pci_setup_queue(virtio_pci_hw_t *hw, struct vring *vr);
void virtio_pci_print_common_cfg(virtio_pci_hw_t *hw);

#endif