#pragma once

/// @brief 通用的二分查找算法
/// @details comp(mid, target) 需返回：0 表示匹配；小于 0 表示目标在 mid 左侧；大于 0 表示目标在 mid 右侧
/// @tparam T 数组元素类型
/// @tparam S 查找目标类型
/// @tparam Compare 可调用对象类型，形如 int(T* mid, S* target)
/// @return 匹配到的元素指针，未命中返回 nullptr
template <typename T, typename S, typename Compare>
T *binary_search(T *first, T *last, S *target, Compare comp)
{
	if (first == nullptr || last == nullptr || target == nullptr)
		return nullptr;

	while (first <= last)
	{
		T *mid = first + ((last - first) >> 1);
		int comp_res = comp(mid, target);
		if (comp_res == 0)
			return mid;
		if (comp_res < 0)
			last = mid - 1;
		else
			first = mid + 1;
	}
	return nullptr;
}
