//
// Copy from Li Shuang ( pseudonym ) on 2024-05-19
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//
#include "proc_manager.hh"
#include "fs/ext4/ext4_fs.hh" 

#include "fs/vfs/path.hh"
#include "fs/vfs/file/file.hh"
#include "fs/vfs/file/normal_file.hh"
#include "fs/vfs/dentry.hh"
#include "fs/vfs/fs_defs.hh"
#include "fs/fat/fat32fs.hh"
#include "printer.hh"
#include <EASTL/unordered_map.h>

namespace fs
{
	eastl::unordered_map<eastl::string, FileSystem *> mnt_table;

	/**
	 * @brief 构造一个路径对象，并基于提供的文件指针初始化路径结构。
	 *
	 * @param path_ 路径字符串，例如 "/home/user/file.txt"
	 * @param base_ 基础文件指针，要求为 normal_file 类型（含 dentry）
	 *
	 * @note 若 base_ 不是 normal_file 类型，则会触发 panic。
	 */
	/// 这里的base_文件需要有dentry，目前只有normalfile有dentry
	Path::Path( const eastl::string& path_, file *base_ )
		: pathname( path_ )
	{
		if(base_ == nullptr)
		{
			base = nullptr;
		} else {
			fs::normal_file *file = static_cast<fs::normal_file *>( base_ );
			if( file == nullptr ) 
				panic("Path: base file is not a normalfile");
			
			base = file->getDentry();
		}
		pathbuild();
	}

	Path::Path(const char *str_, file *base_)
		: pathname(str_)
	{
		fs::normal_file *file = static_cast<fs::normal_file *>(base_);
		if (file == nullptr)
			panic("Path: base file is not a normalfile");

		base = file->getDentry();
		pathbuild();
	}

	Path::Path(file *base_)
	{
		fs::normal_file *file = static_cast<fs::normal_file *>(base_);
		if (file == nullptr)
			panic("Path: base file is not a normalfile");

		base = file->getDentry();
		pathbuild();
	}

	/**
	 * @brief 将 pathname 字符串解析为路径段（dirname 向量），并确定路径基准（base）。
	 *
	 * @note 会自动处理绝对路径（以 '/' 开头）和相对路径。若 base 为 nullptr，则默认使用进程当前工作目录。
	 *       结果保存在成员变量 base 与 dirname 中。
	 */
	void Path::pathbuild()
	{
		//[[maybe_unused]]Dentry *root = mnt_table[ "/" ]->getRoot();
		printfBlue("pathbuild: pathname = %s\n", pathname.c_str());
		if (pathname.size() < 1)
		{
			base = proc::k_pm.get_cur_pcb()->get_cwd();
			return;
		}
		else if (pathname[0] == '/')
		{
			printfBlue("pathbuild: absolute path\n");
			if(mnt_table.find("/") == mnt_table.end())
			{

				panic("Path: mnt_table does not contain root");
			}
			base = mnt_table["/"]->getRoot();
		}
		else if (base == nullptr)
		{
			base = proc::k_pm.get_cur_pcb()->get_cwd();
		}

		Info("base name is %s ", base->rName());
		size_t len = pathname.size();
		if (len > 0)
		{ // 保证数组长度不为0
			auto ind = new size_t[len][2]{{0, 0}};
			bool rep = true;
			int dirnum = 0;
			for (size_t i = 0; i < len; ++i)
			{ // 识别以'/'结尾的目录
				if (pathname[i] == '/')
				{
					if (!rep)
					{
						rep = true;
						++dirnum;
					}
				}
				else
				{
					++(ind[dirnum][1]);
					if (rep)
					{
						rep = false;
						ind[dirnum][0] = i;
					}
				}
			}
			if (!rep)
			{
				++dirnum;
			} // 补齐末尾'/'
			dirname = eastl::vector<eastl::string>(dirnum);
			for (size_t i = 0; i < (size_t)dirnum; ++i)
			{
				dirname[i] = pathname.substr(ind[i][0], ind[i][1]);
			}

			delete[] ind;
		}
		return;
	}
	/**
	 * @brief 获取当前路径对象对应的绝对路径字符串。
	 *
	 * @return eastl::string 表示该路径的绝对路径，例如 "/home/user/file.txt"
	 *
	 * @note 会自动处理 "." 和 ".." 等路径段。
	 */
	eastl::string Path::AbsolutePath() const
	{
		eastl::vector<eastl::string> absname = dirname;
		for (dentry *entry = base;
			 !(entry->isRoot() && entry->getNode()->getSb()->getFileSystem()->isRootFS());
			 entry = entry->getParent())
		{
			absname.emplace(absname.begin(), entry->rName());
		}
		eastl::string path_abs = "";
		for (auto name = absname.begin(); name != absname.end();)
		{
			if (*name == "." || *name == "..")
			{
				absname.erase(name);
			}
			else if (next(name) != absname.end() && *(next(name)) == "..")
			{
				name = absname.erase(name);
			}
			else
			{
				path_abs += "/" + *name;
				name++;
			}
		}
		if (path_abs == "")
		{
			path_abs = "/";
		}

		return path_abs;
	}

	dentry *Path::pathHitTable()
	{
		eastl::string path_abs = AbsolutePath();
		size_t len = path_abs.size();
		size_t longest = 0;
		eastl::string longest_prefix = "";
		for (auto tbl : mnt_table)
		{
			eastl::string prefix = tbl.first;
			size_t len_prefix = prefix.size();
			if (len >= len_prefix && len_prefix > longest && path_abs.compare(0, len_prefix, prefix) == 0)
			{
				longest = len_prefix;
				longest_prefix = prefix;
			}
		}

		if (longest > 0)
		{
			dentry *root = mnt_table[longest_prefix]->getSuperBlock()->getRoot();
			base = root;
			pathname = path_abs.substr(longest);
			if (pathname.empty())
				dirname.clear();
			else
				pathbuild();
			return root;
		}
		else
			return nullptr;
	}

	dentry *Path::pathSearch(bool parent)
	{
		/// @todo 1: 通过路径名查找文件

		if (pathname == "/")
			return mnt_table["/"]->getSuperBlock()->getRoot();
		dentry *entry, *next;
		if (base == nullptr)
		{
			return nullptr;
		} // 无效路径
		// else if( ( entry = pathHitTable()) != nullptr) { }// 查询挂载表，找到对应根目录
		// else { entry = base; }	// 未找到对应根目录，使用当前目录

		entry = base;
		int dirsize = dirname.size();
		// printfBlue( "path search %d level", dirsize );
		for (int i = 0; i < dirsize; i++)
		{
			while (entry->isMntPoint())
				panic("pathSearch: entry is a mount point");
			/// @todo 这里随后检查 是否是目录，文件的结构不完善
			// if( !( entry->getNode()->rMode() & (fs::File_dir << fs::FileAttrs::File_dir_s ) ) )
			// 	return nullptr; // 不是目录
			if (parent && i == dirsize - 1)
				return entry; // 返回父目录
			if (dirname[i] == ".")
				next = entry;
			else if (dirname[i] == "..")
				next = entry->getParent();
			else
			{
				// printfBlue("dentry %s search sub-dir\n", entry->rName().c_str());
				if (auto it = entry->EntrySearch(dirname[i]); it != nullptr)
				{
					next = it;
				}
				else
					return nullptr;
			}
			entry = next;
		}
		return entry;
	}

	int Path::mount(Path &dev, eastl::string fstype, uint64 flags, uint64 data)
	{
		fs::dentry *mntEnt = pathSearch();
		fs::dentry *devEnt = dev.pathSearch();
		if (mntEnt == nullptr || devEnt == nullptr)//TODO: 无效路径，无法挂载，返回0？，抄自学长
		{
			return 0;
		}
		if (mntEnt->getNode()->getFS()->mount(devEnt, mntEnt, fstype) == 0)
		{
			return 0;
		}
		return -1;
	}

	int Path::umount(uint64 flags)
	{
		fs::dentry *mnt = pathSearch();
		if (mnt->getParent()->getNode()->getFS()->umount(mnt) == 0)
		{
			mnt_table.erase(pathname);
			return 0;
		}
		// return -1; @ TODO：2025/06/03
		return 0;
	}

	int Path::open(FileAttrs attrs_, int flags)
	{
		printf("flags is %d\n", flags);
		dentry *den = pathSearch();

		if (!den && flags & O_CREAT) // @todo 创建文件
			return -1;
		FileAttrs attrs = den->getNode()->rMode();

		if (attrs.filetype != FileTypes::FT_DIRECT && attrs_.u_write && attrs_.o_write && attrs_.g_write)
			printfRed(" try to open a directory with write mode");

		if (attrs_.filetype == FileTypes::FT_DIRECT && attrs.filetype != FileTypes::FT_DIRECT)
			printfRed(" try to open a not directory file as directory ");
		if (attrs_.o_write > attrs.o_write &&
			attrs_.o_read > attrs.o_read &&
			attrs_.o_exec > attrs.o_exec)
			return -1; // 权限校验失败

		fs::normal_file *f = new fs::normal_file(attrs_, den);
		if (flags & O_APPEND)
			f->setAppend();
		proc::Pcb *cur_proc = proc::k_pm.get_cur_pcb();
		int fd = proc::k_pm.alloc_fd(cur_proc, f);

		if (fd < 0)
			return -1;
		return fd;
	}

	int Path::rename(const eastl::string &new_path, int /*flags*/)
	{
		// 获取当前 Path 对象的绝对路径
		eastl::string old_path_str = this->AbsolutePath();
		const char* old_path = old_path_str.c_str();
		const char* new_path_cstr = new_path.c_str();

		// 调用底层 ext4_do_rename 进行重命名
		int ret = ext4_do_rename(old_path, new_path_cstr);
		if (ret != 0) {
			// 返回底层错误码，便于上层处理
			return ret;
		}
		return 0;
	}
} // namespace fs
