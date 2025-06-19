#pragma once

template <typename T>
struct ReadOnly
{
    volatile T value;

    operator T() const { return value; }
    ReadOnly &operator=(const T &) = delete;
};

template <typename T>
struct WriteOnly
{
    volatile T value;

    void operator=(T val) { value = val; }
    operator T() = delete;
};

template <typename T>
struct Volatile
{
    volatile T value;

    operator T() const { return value; }
    Volatile &operator=(T val)
    {
        value = val;
        return *this;
    }
};