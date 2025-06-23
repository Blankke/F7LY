//
// Copy from Li Shuang ( pseudonym ) on 2024-05-19 
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use 
// | Contact Author: lishuang.mk@whu.edu.cn 
// --------------------------------------------------------------
//

#pragma once 

#include "types.hh"

#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <EASTL/unordered_map.h>
// 文件系统命名空间 fs，包含 Path 类用于路径字符串的解析与转换。
// Path 是 VFS 层的路径表示类，负责将用户输入的路径字符串解析为一系列目录组件，
// 并提供路径查找（search）、创建（create）、挂载（mount）、打开（open）等操作。

namespace fs
{
	class File;
	class FileSystem;
	class dentry;
	class file;
	struct FileAttrs;

	class Path
	{
	private:
		dentry *base;  // 路径的起始目录节点（即解析路径的起点，可能是工作目录或根目录）
		eastl::string pathname;  // 原始路径字符串
		eastl::vector<eastl::string> dirname;  // 解析后路径的各级目录名组成的向量

	public:
		Path() = default;
		Path( const Path& path ) = default;
		
		/// @brief 使用 file 指针构造路径（通常用于打开文件的场景）
		Path( const eastl::string& path_, file *base_ );

		// 使用字符串路径 + 起始目录 dentry 构造
		Path( const eastl::string& path_, dentry *base_ )
			: base( base_ )
			, pathname( path_ )
		{
			pathbuild();  // 解析路径字符串
		}

		// 使用字符串路径（默认从根目录或默认起点解析）
		Path( const eastl::string& path_ )
			: base( nullptr )
			, pathname( path_ )
		{
			pathbuild();
		}

		// 使用 C 字符串路径 + file 指针构造
		Path( const char *str_, file *base_ );

		// 使用 C 字符串路径 + 起始目录 dentry 构造
		Path( const char *str_, dentry *base_ )
			: base( base_ )
			, pathname( str_ )
		{
			pathbuild();
		}

		// 使用 C 字符串路径（无起始 dentry）
		Path( const char *str_ )
			: base( nullptr )
			, pathname( str_ )
		{
			pathbuild();
		}

		// 从给定文件对象 file 推导出路径（通常用于打开文件的场景）
		Path( file *base_ );

		// 从给定目录节点 dentry 推导路径
		Path( dentry *base_ )
			: base( base_ )
		{
			pathbuild();
		}

		~Path() = default;

		// 赋值操作
		Path& operator=( const Path& path ) = default;

		// 将 pathname 解析成 dirname（目录名列表）
		void pathbuild();

		// 返回解析后的**绝对路径**字符串
		eastl::string AbsolutePath() const;

		// 从 base 出发解析 pathname 并返回命中路径的 dentry（可以带缓存加速）
		dentry *pathHitTable();

		// 进行路径查找，可选是否查找父目录（parent = true 表示获取倒数第二级）
		dentry *pathSearch( bool parent = false );

		// 创建路径中对应的文件或目录，并返回其 dentry，mode 为权限与类型标志
		dentry *pathCreate( uint32 mode );

		// 将当前路径挂载指定设备路径 dev，fstype 为文件系统类型，flags/data 为挂载参数
		int mount( Path &dev, eastl::string fstype , uint64 flags, uint64 data );

		// 对当前路径执行卸载操作
		int umount( uint64 flags );

		// 打开当前路径对应的文件，并返回 file 描述符（flags 指定打开方式）
		int open( FileAttrs attrs, int flags );

		int rename( const eastl::string &new_name, int flags );

		// 返回原始路径字符串
		eastl::string rPathName() { return pathname; }

		// 返回路径最后一级文件名（例如 /a/b/c.txt 则为 c.txt）
		eastl::string rFileName() { return dirname[ dirname.size() - 1 ]; }
	};

	// 全局挂载表：路径字符串 -> 文件系统对象的映射
	extern eastl::unordered_map<eastl::string, FileSystem *> mnt_table;
} // namespace fs

