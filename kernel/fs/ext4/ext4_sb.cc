//
// Copied from Li Shuang ( pseudonym ) on 20240-7-22
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

#include "ext4_sb.hh"

#include "ext4_fs.hh"
#include "common.hh"

namespace fs
{
	namespace ext4
	{
		Ext4SB::Ext4SB( Ext4SuperBlock *sb, FileSystem *fs )
		{
			_super_block = *sb;
			_belong_fs	 = (Ext4FS *) fs;
			if ( _super_block.magic != 0xEF53 )
			{
				printfRed( "bad ext4 superblock while magic number (0x%x) is not equal 0xEF53",
						   _super_block.magic );
				return;
			}

			_cache_block_size = _1K << _super_block.log_block_size;
			Info( "ext4 block size %d bytes", _cache_block_size );

			_support_64bit = _super_block.feature_incompat.flags.enable_64bit;

			if ( _support_64bit )
			{
				_cache_block_count =
					_super_block.blocks_count_lo + ( (ulong) _super_block.blocks_count_hi << 32 );
				_cache_free_block_count = _super_block.free_blocks_count_lo +
										  ( (ulong) _super_block.free_blocks_count_hi << 32 );
			}
			else
			{
				_cache_block_count		= _super_block.blocks_count_lo;
				_cache_free_block_count = _super_block.free_blocks_count_lo;
			}

			Info(
				"ext4\n"
				"\tblock count = %d\n"
				"\tbfree block = %d",
				_cache_block_count,
				_cache_free_block_count
			);
		}

		Ext4SB::~Ext4SB()
		{
			// Info( "Ext4SB deconstructor" );
		}

		Inode *fs::ext4::Ext4SB::allocInode( fs::FileAttrs attrs, eastl::string dev_name )
		{
			return nullptr;
		}


	} // namespace ext4

} // namespace fs
