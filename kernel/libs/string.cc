#include "string.hh"

string::string() : data_(nullptr), length_(0), capacity_(0)
{
    allocate(1);
    data_[0] = '\0';
}

string::string(const char *str) : data_(nullptr), length_(0), capacity_(0)
{
    if (str)
    {
        length_ = 0;
        while (str[length_] != '\0')
        {
            length_++;
        }

        allocate(length_ + 1);
        for (size_t i = 0; i < length_; ++i)
        {
            data_[i] = str[i];
        }
        data_[length_] = '\0';
    }
    else
    {
        allocate(1);
        data_[0] = '\0';
    }
}

string::string(const string &other) : data_(nullptr), length_(0), capacity_(0)
{
    allocate(other.length_ + 1);
    length_ = other.length_;
    for (size_t i = 0; i < length_; ++i)
    {
        data_[i] = other.data_[i];
    }
    data_[length_] = '\0';
}

string::~string()
{
    if (data_)
    {
        delete[] data_;
    }
}

void string::allocate(size_t size)
{
    capacity_ = size;
    data_ = new char[capacity_];
}

void string::reallocate(size_t new_capacity)
{
    if (new_capacity <= capacity_)
        return;

    char *new_data = new char[new_capacity];
    for (size_t i = 0; i < length_; ++i)
    {
        new_data[i] = data_[i];
    }
    new_data[length_] = '\0';

    delete[] data_;
    data_ = new_data;
    capacity_ = new_capacity;
}

string &string::operator=(const string &other)
{
    if (this != &other)
    {
        if (capacity_ < other.length_ + 1)
        {
            delete[] data_;
            allocate(other.length_ + 1);
        }

        length_ = other.length_;
        for (size_t i = 0; i < length_; ++i)
        {
            data_[i] = other.data_[i];
        }
        data_[length_] = '\0';
    }
    return *this;
}

string &string::operator=(const char *str)
{
    if (str)
    {
        size_t new_length = 0;
        while (str[new_length] != '\0')
        {
            new_length++;
        }

        if (capacity_ < new_length + 1)
        {
            delete[] data_;
            allocate(new_length + 1);
        }

        length_ = new_length;
        for (size_t i = 0; i < length_; ++i)
        {
            data_[i] = str[i];
        }
        data_[length_] = '\0';
    }
    else
    {
        clear();
    }
    return *this;
}

char &string::operator[](size_t index)
{
    return data_[index];
}

const char &string::operator[](size_t index) const
{
    return data_[index];
}

size_t string::length() const
{
    return length_;
}

const char *string::c_str() const
{
    return data_;
}

bool string::empty() const
{
    return length_ == 0;
}

string string::operator+(const string &other) const
{
    string result;
    result.resize(length_ + other.length_);

    for (size_t i = 0; i < length_; ++i)
    {
        result.data_[i] = data_[i];
    }

    for (size_t i = 0; i < other.length_; ++i)
    {
        result.data_[length_ + i] = other.data_[i];
    }

    result.length_ = length_ + other.length_;
    result.data_[result.length_] = '\0';

    return result;
}

string &string::operator+=(const string &other)
{
    if (capacity_ < length_ + other.length_ + 1)
    {
        reallocate(length_ + other.length_ + 1);
    }

    for (size_t i = 0; i < other.length_; ++i)
    {
        data_[length_ + i] = other.data_[i];
    }

    length_ += other.length_;
    data_[length_] = '\0';

    return *this;
}

bool string::operator==(const string &other) const
{
    if (length_ != other.length_)
        return false;

    for (size_t i = 0; i < length_; ++i)
    {
        if (data_[i] != other.data_[i])
        {
            return false;
        }
    }

    return true;
}

bool string::operator!=(const string &other) const
{
    return !(*this == other);
}

bool string::operator<(const string &other) const
{
    size_t min_len = length_ < other.length_ ? length_ : other.length_;

    for (size_t i = 0; i < min_len; ++i)
    {
        if (data_[i] != other.data_[i])
        {
            return data_[i] < other.data_[i];
        }
    }

    return length_ < other.length_;
}

bool string::operator>(const string &other) const
{
    return other < *this;
}

void string::clear()
{
    length_ = 0;
    if (data_)
    {
        data_[0] = '\0';
    }
}

void string::resize(size_t new_size)
{
    if (new_size == length_)
        return;

    if (new_size > capacity_)
    {
        reallocate(new_size + 1);
    }

    if (new_size > length_)
    {
        for (size_t i = length_; i < new_size; ++i)
        {
            data_[i] = '\0';
        }
    }

    length_ = new_size;
    data_[length_] = '\0';
}

string string::substr(size_t pos, size_t len) const
{
    if (pos >= length_)
    {
        return string();
    }

    size_t actual_len = len;
    if (pos + len > length_)
    {
        actual_len = length_ - pos;
    }

    string result;
    result.resize(actual_len);

    for (size_t i = 0; i < actual_len; ++i)
    {
        result.data_[i] = data_[pos + i];
    }

    result.data_[actual_len] = '\0';

    return result;
}