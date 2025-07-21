#include <EASTL/string.h>
#include <EASTL/unique_ptr.h>
#include "printer.hh"
#include <EASTL/vector.h>
#include "fs/vfs/file.hh"
#include "fs/vfs/file/virtual_file.hh"
#include "fs/vfs/vfs_utils.hh"
#define MAX_CHILDREN_NUM 128

namespace fs
{
    struct vfile_msg
    {
        bool is_virtual;
        int file_type;  // FileTypes 枚举值
        eastl::unique_ptr<VirtualContentProvider> provider;
        
        vfile_msg() : is_virtual(false), file_type(0), provider(nullptr) {}
    };

    struct vfile_tree_node
    {
        eastl::string name;
        int file_type;  // FileTypes 枚举值
        eastl::unique_ptr<VirtualContentProvider> provider;
        
        // 树形结构相关
        vfile_tree_node* parent;
        vfile_tree_node* children[MAX_CHILDREN_NUM];
        int children_count;
        
        // 构造函数
        vfile_tree_node(const eastl::string &name, int file_type = 0, 
                       eastl::unique_ptr<VirtualContentProvider> provider = nullptr)
            : name(name), file_type(file_type), provider(std::move(provider)), 
              parent(nullptr), children_count(0) 
        {
            for (int i = 0; i < MAX_CHILDREN_NUM; i++) {
                children[i] = nullptr;
            }
        }
        
        // 析构函数
        ~vfile_tree_node() 
        {
            for (int i = 0; i < children_count; i++) {
                delete children[i];
            }
        }
        
        // 添加子节点
        bool add_child(vfile_tree_node* child) 
        {
            if (children_count >= MAX_CHILDREN_NUM) {
                return false;
            }
            children[children_count] = child;
            child->parent = this;
            children_count++;
            return true;
        }
        
        // 查找子节点
        vfile_tree_node* find_child(const eastl::string& name) const 
        {
            for (int i = 0; i < children_count; i++) {
                if (children[i] && children[i]->name == name) {
                    return children[i];
                }
            }
            return nullptr;
        }
        
        // 删除子节点
        bool remove_child(const eastl::string& name) 
        {
            for (int i = 0; i < children_count; i++) {
                if (children[i] && children[i]->name == name) {
                    delete children[i];
                    // 将后面的元素前移
                    for (int j = i; j < children_count - 1; j++) {
                        children[j] = children[j + 1];
                    }
                    children[children_count - 1] = nullptr;
                    children_count--;
                    return true;
                }
            }
            return false;
        }
        
        // 检查是否为叶子节点
        bool is_leaf() const 
        {
            return children_count == 0;
        }
        
        // 获取完整路径
        eastl::string get_full_path() const 
        {
            eastl::vector<eastl::string> path_parts;
            const vfile_tree_node* current = this;
            
            while (current && current->parent) {
                path_parts.push_back(current->name);
                current = current->parent;
            }
            
            eastl::string full_path = "/";
            for (int i = path_parts.size() - 1; i >= 0; i--) {
                full_path += path_parts[i];
                if (i > 0) full_path += "/";
            }
            
            return full_path;
        }
    };
    
    class VirtualFileSystem
    {
    private:
        vfile_tree_node* root;  // 根节点
        
        vfile_tree_node* find_node_by_path(const eastl::string& path) const;
        vfile_tree_node* create_path_nodes(const eastl::string& path);
        void destroy_tree(vfile_tree_node* node);
        
    public:
        VirtualFileSystem();
        ~VirtualFileSystem();
        
        // 禁用拷贝构造和赋值
        VirtualFileSystem(const VirtualFileSystem&) = delete;
        VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;
        
        eastl::vector<eastl::string> virtual_file_path_list;  // 保留用于兼容性
        bool is_filepath_virtual(const eastl::string &path) const;
        vfile_msg get_vfile_msg(const eastl::string &absolute_path) const;
        void dir_init();
        int openat(eastl::string absolute_path, fs::file *&file, uint flags);
        int vfile_openat(eastl::string absolute_path, fs::file *&file, uint flags);
        eastl::vector<eastl::string> path_split(const eastl::string &path) const;
        
        bool add_virtual_file(const eastl::string& path, int file_type, 
                             eastl::unique_ptr<VirtualContentProvider> provider);
        bool remove_virtual_file(const eastl::string& path);
        bool is_virtual_path(const eastl::string& path) const;
        vfile_tree_node* get_virtual_node(const eastl::string& path) const;
        
        void list_virtual_files(const eastl::string& dir_path, 
                               eastl::vector<eastl::string>& file_list) const;
        void print_tree(vfile_tree_node *node, int depth, const eastl::string &prefix) const;
        bool is_file_exist(const eastl::string &path) const
        {
            vfile_tree_node *node = find_node_by_path(path);

            return (node != nullptr && node->file_type != 0) || vfs_is_file_exist(path.c_str()); // 0表示不存在或不是文件
        }
        int path2filetype(eastl::string &absolute_path) const
        {
            vfile_tree_node *node = find_node_by_path(absolute_path);
            if (node)
            {
                return node->file_type;
            }
            else
            {
                return vfs_path2filetype(absolute_path);
            }
        }
    };

    extern VirtualFileSystem k_vfs;
}