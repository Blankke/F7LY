//
// Copy from Li Shuang ( pseudonym ) on 2024-07-22
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

#pragma once

#include "ext4.hh"
#include "fs/vfs/inode.hh"

namespace fs
{
	namespace ext4
	{
		class Ext4FS;
		class Ext4Buffer;
		class Ext4IndexNode : public Inode
		{
			friend Ext4FS;
		private:

			Ext4Inode _inode;
			Ext4FS	 *_belong_fs  = nullptr;
			long	  _has_blocks = 0; // 通常是指 4KiB block
			long	  _has_size	  = 0; // bytes
			FileAttrs _attrs;

		public:

			Ext4IndexNode() = default;
			Ext4IndexNode( Ext4Inode &inode, Ext4FS *fs );
			virtual ~Ext4IndexNode() override = default;

			virtual Inode *lookup( eastl::string dirname ) override;
			virtual Inode *mknode( eastl::string name, FileAttrs attrs,
								   eastl::string dev_name ) override;
								   

			virtual size_t nodeRead( u64 dst, size_t off, size_t len ) override;
			virtual size_t nodeWrite( u64 src, size_t off, size_t len ) override { return 0; };
			virtual int	   readlinkat( char *buf, size_t len ) override { return 0; };

			virtual size_t readSubDir( ubuf &dst, size_t off ) override;
			void setAttr(FileAttrs attrs) { _attrs = attrs; }

		public:

			virtual FileAttrs	rMode() const override { return _attrs; };
			virtual dev_t		rDev() const override { return -1; };
			virtual u64			rFileSize() const override { return -1; };
			virtual u64			rIno() const override { return 0; };
			virtual SuperBlock *getSb() const override;
			virtual FileSystem *getFS() const override { return (FileSystem *) _belong_fs; };

		public:

			/// @brief 读取 inode 所含的某一个逻辑块
			/// @param block
			/// 对于当前inode而言的逻辑块号（从0开始的块号，0号块是文件开头）
			/// @param pin pin住buffer，使其不要被释放（unpin可以释放buffer）
			/// @return 数据块的buffer
			Ext4Buffer *read_logical_block( long block, bool pin = false );

		private:

			void _cal_size();
			void _cal_blocks();

			Ext4Buffer *_search_direct_block( long target_block_no, long start_block_no,
											  void *index_block, bool pin = false );
			Ext4Buffer *_search_sindirect_block( long target_block_no, long start_block_no,
												 void *index_block, bool pin = false );
			Ext4Buffer *_search_dindirect_block( long target_block_no, long start_block_no,
												 void *index_block, bool pin = false );
			Ext4Buffer *_search_tindirect_block( long target_block_no, long start_block_no,
												 void *index_block, bool pin = false );
			long		_cal_tind_index( long target_block, long start_block );
			long		_cal_dind_index( long target_block, long start_block );
			long		_cal_sind_index( long target_block, long start_block );

			Inode *_htree_lookup( eastl::string &dir_name );
			Inode *_linear_lookup( eastl::string &dir_name, void *block );

		public: // debug

			void debug_hash( eastl::string dir_name );

		public:
			/// 检查 _attrs 与 _inode.mode 的一致性
			bool checkAttrConsistent() const {
				// 检查权限位
				if (_attrs._value != ((u16)_inode.mode & 0x1FF))
					return false;
				// 检查文件类型
				Ext4InodeMode md = (Ext4InodeMode)((u16)_inode.mode & 0xF000);
				FileTypes ft = FileTypes::FT_NONE;
				switch (md) {
					case ext4_imode_fdir: ft = FileTypes::FT_DIRECT; break;
					case ext4_imode_fchr:
					case ext4_imode_fblk: ft = FileTypes::FT_DEVICE; break;
					case ext4_imode_fifo: ft = FileTypes::FT_PIPE; break;
					case ext4_imode_freg: ft = FileTypes::FT_NORMAL; break;
					default: ft = FileTypes::FT_NONE; break;
				}
				return _attrs.filetype == ft;
			}
		};

	} // namespace ext4

} // namespace fs
