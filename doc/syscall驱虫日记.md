# chdir
- 官老师第一次发现是别人的代码为什么挂载后会创建函数，我们这里直接爆了。
- 镜头跟着官老师，我们一路来到了lookup函数
```c
	Inode *Ext4IndexNode::lookup( eastl::string dirname )
		{
			// printfBlue( "current inode mode=%x\n", _inode.mode );
			//  if(!checkAttrConsistent())
			//   printfRed( "ext4-inode: inode mode isnot consistent with attrs\n" );
			// printfGreen( "ext4-inode: inode mode is consistent with attrs\n" );
			if ( !( _inode.mode & ext4_imode_fdir ) ) // 当前inode不是目录
			{
				panic( "try to lookup an inode that's not dir (dirname='%s')",
						   dirname.c_str() );
				return nullptr;
			}
```
- 抛开注释不说，我们找到了原因是_inode的mode是0，那么这里为什么是0呢？官老师感到非常的奇怪，明明一开始的打印是有mode的啊？
```c
 └── / [mode: 16895, type: 3]
     ├── mnt [mode: 16895, type: 3]
     ├── etc [mode: 16895, type: 3]
     │   └── busybox.conf [mode: 16895, type: 3]
     ├── bin [mode: 16895, type: 3]
     │   └── ls [mode: 33279, type: 4]
     ├── tmp [mode: 16895, type: 3]
     ├── sys [mode: 16895, type: 3]
     ├── dev [mode: 16895, type: 3]
     │   ├── rtc [mode: 8630, type: 2]
     │   ├── null [mode: 8630, type: 2]
     │   ├── stdout [mode: 8630, type: 2]
     │   ├── zero [mode: 8630, type: 2]
     │   ├── hda [mode: 8630, type: 2]
     │   ├── Disk driver [mode: 8630, type: 2]
     │   ├── stderr [mode: 8630, type: 2]
     │   └── stdin [mode: 8630, type: 2]
     └── proc [mode: 16895, type: 3]
         ├── self [mode: 16676, type: 3]
         │   └── exe [mode: 0, type: 0]
         ├── meminfo [mode: 0, type: 0]
         └── mounts [mode: 33060, type: 4]
```
- 于是一路尾随，想知道啥时候把mnt的mode给干掉了，最后追到mnt挂载的最初点，发现此mode非彼mode，这个ext4的inode是有两个属性的啊，在copilot老师的帮助下，它告诉我们_attrs是手动赋值的，`_inode`是读磁盘的时候就赋值了的。
```c
		class Ext4IndexNode : public Inode
		{
			friend Ext4FS;
		private:

			Ext4Inode _inode;
			Ext4FS	 *_belong_fs  = nullptr;
			long	  _has_blocks = 0; // 通常是指 4KiB block
			long	  _has_size	  = 0; // bytes
			FileAttrs _attrs;
```
- 那么是什么时候读的磁盘呢。应该是read_node,我们追到了这个地方，在初始化ext4fs的位置，也就是挂载的时候调用 的ramfs
```c
            fs::Path mnt("/mnt");
            fs::Path dev("/dev/hda");
            mnt.mount(dev, "ext4", 0, 0);
```
Ramfs->mount()初始化会调用init，我们在inode结点从磁盘读取的地方设下了天罗地网，设法找到_mode这里到底有没有。
```c
			Ext4Inode node;
			if ( read_inode( root_ino, node ) < 0 )
			{
				panic( "ext4-fs : read inode failure while init root" );
				return;
			}
			printfYellow( "ext4-fs : read root inode %d, mode: %d\n", root_ino, node.mode );
			_root_inode = new Ext4IndexNode( node, this );
			printfGreen( "ext4-fs : init root inode mode:%d\n", _root_inode->_inode.mode );
			_root_inode->setAttr(FileAttrs(fs::FileTypes::FT_DIRECT, 0777));
```
那还真没用，我们读的时候已经找不到了，所以肯定在之前读磁盘的时候就已经出问题了。那么，现在问题的上半场就解决了，我们把问题重定向到了磁盘没读进来的问题。

- 当我们的记者去上个厕所的时间，官老师已经迅速锁定了新的嫌疑人，同样在init函数之中，但是在更早期就接触了我们的当事人磁盘，并试图从中获取到信息，失败的地方就在这里
```c
			Buffer buf;
			//这个读了lba的头
			buf = k_bufm.read_sync( dev, 2 );
			new ( &_sb ) Ext4SB( (Ext4SuperBlock*) buf.get_data_ptr(), this );
```
- 走进read_sync，我们看到官老师在现场设下的埋伏，很明显，官老师已经读到了这里面的内容，而且觉得不对，在本台的反复询问下，官老师不厌其烦地给刚来到现场的记者讲述刚刚发生的事情：
```c
	Buffer BufferManager::read_sync(int dev, uint64 lba)
	{
		Buffer buf = _get_buffer_sync(dev, lba);
		uint offset = _offset_from_lba(lba);
		buf._buffer_base = (void *)((uint64)buf._buffer_base +
									default_sector_size * offset);
		
		/// debug
		printfCyan("BufferManager : read_sync lba %u, offset %u\n", lba, offset);
		printfCyan( "Physical disk offset: 0x%x\n", lba * default_sector_size ); // 打印物理偏移
		uint8_t *data = static_cast<uint8_t *>(buf._buffer_base);
		printf("Buffer content at LBA %u:\n", lba);
		for (int i = 0; i < 64; ++i)
		{
			printf("%02x ", data[i]);
			if ((i + 1) % 16 == 0)
				printf("\n");
		}

		return buf;
	}

```
- 让我们看看输出，输出很奇怪地，打出来的东西和oskernel2025的正品进行比较后发现，lba2的位置只有前面是对的，后面一半对不上，再后面的lba8等更是全为0，这让本台记者非常不解，似乎这是读了一半后失败了？
- 官老师大觉不妙，首先怀疑的是我们的磁盘是不是不对，于是复制来了oskernel2025里面的fs.img，这下好了，lba2全部都对上了。后面依然全是0，有些页面还重新读取了一遍lba2，这让官老师大为恼火。
    - 不过我们的官老师很快就想出了更好的破案方法，hex editor可以直接阅读磁盘内容，这下官老师设置打印语句，把磁盘开始的偏移量打了出来，和磁盘文件直接对比，那么这么一来，我们就能看到正品的地址是多少了，lba 2是0x400处，没问题，内容也对上了，官老师自己奇思巧算，lba8就是`7*400`,那么就是0x1600的位置，一看果然不对
    - 但是lba8是0x1000处，这点官老师也很快意识到了，并预感这就是事情的本源，很不幸，我们打印的偏移量也是0x1000，输出依然是0，没用破获这个案件。
- 官老师盯着函数看了半天，觉得另有门道，offset、buf.base这种变量根本就和硬盘无关，我们读到的内容应该是与操作系统无关的，所以封装到底层的函数肯定更重要。他大笔一挥，让警犬向函数深处追去，并咬牙切齿地断言（assert）就是` _get_buffer_sync`里面那个`_get_buffer( dev, lba, false )`有问题，严查！必须严查！
- 不料`_get_buffer`错综复杂，本台记者跟了一会就跟不上官老师的步伐，在迷宫里昏昏欲睡，半梦半醒间惊呼这里还有一层里面的封装，这才是出口！
```c
			else
			{
				_lock.release();
				bd->read_blocks_sync( lba, default_buffer_size / bd->get_block_size(), &buf_des,
									  1 );
				_lock.acquire();
			}
```
- 官老师却嗤之以鼻，
    - “这扇门后面还有一个入口，这个 `virtio_disk_rw`的协议什么的绝对有问题，于是热血沸腾，但是其实已经到了穷途末路，时间已晚，记者已经快要放弃采访，再找不到凶手都要回去睡觉了。
```c
    int VirtioDriver::read_blocks_sync(long start_block, long block_count, dev::BufferDescriptor *buf_list,
                                       int buf_count)
    {
      virtio_disk_rw(start_block, block_count, buf_list, buf_count, 0);
      return 0;
    }
```
- 官老师在反复的查找后也显得非常疲惫，在又提了几个线索后表示必须让头脑清醒一点，回寝室冲澡泸盥，否则他想不出来为什么错了。
    - 他嘴里反复念叨，“这个设备virtio的协议一定有问题”
    - 当记者提问什么是协议时，官老师动着疲惫的身躯说：“哎呀就是设备请求头那些”
```c

      // 设置请求头描述符
      disk.desc[idx[0]].addr = PTE2PA(mem::k_pagetable.kwalkaddr((uint64)&buf0).get_data());
      disk.desc[idx[0]].len = sizeof(buf0);
      disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
      disk.desc[idx[0]].next = idx[1];

      // 设置数据描述符
      disk.desc[idx[1]].addr = buf_list[0].buf_addr;
      disk.desc[idx[1]].len = _block_size * block_count;
      disk.desc[idx[1]].flags = write ? 0 : VRING_DESC_F_WRITE;
      disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
      disk.desc[idx[1]].next = idx[2];

      // 设置状态描述符
      disk.info[idx[0]].status = 0; // 设置为未完成状态
      disk.desc[idx[2]].addr = (uint64)&disk.info[idx[0]].status;
      disk.desc[idx[2]].len = 1;
      disk.desc[idx[2]].flags = VRING_DESC_F_WRITE;
      disk.desc[idx[2]].next = 0;

      // 记录请求信息
      disk.info[idx[0]].b = buf_list;
      disk.info[idx[0]].wait = true;

      // 通知设备
      disk.avail[2 + (disk.avail[1] % NUM)] = idx[0];
```
- 记者于是象征性地盯着这里看，官老师在这时宣布“不就是读个磁盘吗？这都读不出来，今天做不出来跳楼了。”
    - 记者两眼放光，这才是大新闻啊，要是我换到官老师寝室去不就保研了吗？但是我不能换，保研名额就会被官老师的室友抢占几个，这样是不划算的。盘算着，记者马上想到昨日跟随官老师报导之时查获的一个嫌犯`walk_addr`，这里好像有他的同伙`kwalkaddr`。笨手笨脚的记者于是点开了这个函数尝试检查，发现此举的官老师大手一挥，“哎呀，你添一个别人那版的上去就好了。”
    - 转身关门离去，留下一地鸡毛。

- 于是我们留在现场的记者遵从官老师的指示，昨天的嫌犯`walk_addr`是因为页内偏移被对齐了，所以所有本页的访问都只有页头的基址导致错误。`kwalkaddr`和它犯下了同样的罪过，在当局的处理下已经捉拿归案，换上了没有玩忽职守的`kwalk_addr`。
- 记者小心翼翼地将所谓的"协议"改了一下，然后试探性地make run
```c
      disk.desc[idx[0]].addr = mem::k_pagetable.kwalk_addr((uint64) &buf0);
```
- 就在这个时候，在官老师离去的20后，磁盘修好了，打出来的输出可以对上了：
```sh
Buffer content at LBA 8:
01 02 00 00 11 02 00 00 21 02 00 00 d8 00 74 13 
1f 00 04 00 00 00 00 00 11 d2 8e 9a 74 13 e3 b7 
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
00 00 00 00 00 00 00 00 f9 73 90 fe 00 00 00 00 
```
- 我们打电话让官老师见证这一刻，
>[!bug] 官老师说
>**“我敲！一个walk_addr坑了我两次！”**
>           ***watchout  n.  留神观察；留心守候；监视***

# fork
```cpp

    int VirtualMemoryManager::vm_copy(PageTable &old_pt, PageTable &new_pt, uint64 start, uint64 size)
```
抄过来的代码和我们代码中函数原型不一致
注意点
- 在使用时父进程需要等待子进程执行完, 不能直接关机

# execve
- 学长的proc_pagetable函数是弃用的, 我们的是好的, 直接用这个函数就可以构建基础页表
- 在从elf提取flag时, 需要进行一下映射, elf的flag和sv39的标志位不一样
- 注意根页表, 我们pte的根页表是直接开出来空间的, 在释放的时候不用直接释放, 留着就行