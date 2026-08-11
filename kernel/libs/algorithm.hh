#pragma once

namespace util
{
    // 用单次求值的函数替代旧 MAX/MIN 宏，避免参数带副作用时被执行两次。
    template <typename Left, typename Right>
    constexpr auto max(Left left, Right right)
    {
        using Result = decltype(true ? left : right);
        const Result normalized_left = left;
        const Result normalized_right = right;
        return normalized_left > normalized_right ? normalized_left : normalized_right;
    }

    template <typename Left, typename Right>
    constexpr auto min(Left left, Right right)
    {
        using Result = decltype(true ? left : right);
        const Result normalized_left = left;
        const Result normalized_right = right;
        return normalized_left < normalized_right ? normalized_left : normalized_right;
    }
} // namespace util
