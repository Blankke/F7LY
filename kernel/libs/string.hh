#include "types.hh" 
#pragma once
class string
{
public:
    // 构造函数
    string();
    string(const char *str);
    string(const string &other);

    // 析构函数
    ~string();

    // 赋值操作符
    string &operator=(const string &other);
    string &operator=(const char *str);

    // 访问元素
    char &operator[](size_t index);
    const char &operator[](size_t index) const;

    // 字符串操作
    size_t length() const;
    const char *c_str() const;
    bool empty() const;

    // 字符串连接
    string operator+(const string &other) const;
    string &operator+=(const string &other);

    // 比较操作
    bool operator==(const string &other) const;
    bool operator!=(const string &other) const;
    bool operator<(const string &other) const;
    bool operator>(const string &other) const;

    // 其他实用函数
    void clear();
    void resize(size_t new_size);
    string substr(size_t pos, size_t len) const;

private:
    char *data_;
    size_t length_;
    size_t capacity_;

    void allocate(size_t size);
    void reallocate(size_t new_capacity);
};