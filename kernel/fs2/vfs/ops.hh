#pragma once

 char* skipelem(char *path, char *name);
 struct inode* namex(char *path, int nameiparent, char *name);
struct inode* namei(char *path);
struct inode* nameiparent(char *path, char *name);

struct inode* find_inode(char *path, int dirfd, char *name);
void get_absolute_path(const char *path, const char *cwd, char *absolute_path);
