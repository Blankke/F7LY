define va2pa
  set $va = $arg0
  set $vpn2 = ($va >> 30) & 0x1FF
  set $vpn1 = ($va >> 21) & 0x1FF
  set $vpn0 = ($va >> 12) & 0x1FF
  set $off  = $va & 0xFFF

  printf "VA  = 0x%016lx\n", $va
  printf "VPN = {%d, %d, %d}, offset = 0x%X\n", $vpn2, $vpn1, $vpn0, $off

  # 读取 satp，提取根页表 PPN
  set $satp = $satp
  set $ppn0 = $satp & ((1UL << 44) - 1)
  set $pt0 = $ppn0 << 12
  printf "Level 2 root page table @ 0x%016lx\n", $pt0

  # 读取第一级（vpn2）
  set $pte2_addr = $pt0 + $vpn2 * 8
  set $pte2 = *(long*)$pte2_addr
  printf "L2 PTE @ 0x%016lx = 0x%016lx\n", $pte2_addr, $pte2
  if ($pte2 & 1) == 0
    printf "L2 PTE not valid.\n"
    return
  end
  set $ppn1 = ($pte2 >> 10) & ((1UL << 44) - 1)
  set $pt1 = $ppn1 << 12

  # 读取第二级（vpn1）
  set $pte1_addr = $pt1 + $vpn1 * 8
  set $pte1 = *(long*)$pte1_addr
  printf "L1 PTE @ 0x%016lx = 0x%016lx\n", $pte1_addr, $pte1
  if ($pte1 & 1) == 0
    printf "L1 PTE not valid.\n"
    return
  end
  set $ppn2 = ($pte1 >> 10) & ((1UL << 44) - 1)
  set $pt2 = $ppn2 << 12

  # 读取第三级（vpn0）
  set $pte0_addr = $pt2 + $vpn0 * 8
  set $pte0 = *(long*)$pte0_addr
  printf "L0 PTE @ 0x%016lx = 0x%016lx\n", $pte0_addr, $pte0
  if ($pte0 & 1) == 0
    printf "L0 PTE not valid.\n"
    return
  end
  set $ppn3 = ($pte0 >> 10) & ((1UL << 44) - 1)
  set $pa = ($ppn3 << 12) + $off
  printf "PA  = 0x%016lx\n", $pa
end
