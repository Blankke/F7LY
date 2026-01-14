#include "common.hh"

#include "printer.hh"

namespace
{
constexpr size_t kBitsPerByte = sizeof(uchar) * 8;

inline uchar *bit_ptr(void *data, uint kbit)
{
	return static_cast<uchar *>(data) + (kbit / kBitsPerByte);
}

inline uchar bit_mask(uint kbit)
{
	return static_cast<uchar>(1U << (kbit % kBitsPerByte));
}
} // namespace

void bit_set(void *data, uint kbit)
{
	if (data == nullptr)
	{
		panic("bit_set on nullptr");
	}
	uchar *p = bit_ptr(data, kbit);
	*p = static_cast<uchar>(*p | bit_mask(kbit));
}

void bit_reset(void *data, uint kbit)
{
	if (data == nullptr)
	{
		panic("bit_reset on nullptr");
	}
	uchar *p = bit_ptr(data, kbit);
	*p = static_cast<uchar>(*p & static_cast<uchar>(~bit_mask(kbit)));
}

bool bit_test(void *data, uint kbit)
{
	if (data == nullptr)
	{
		return false;
	}
	uchar *p = bit_ptr(data, kbit);
	return (*p & bit_mask(kbit)) != 0;
}

int compare(const char *s, const char *t, uint len)
{
	if (s == nullptr || t == nullptr)
	{
		return (s == t) ? 0 : (s ? 1 : -1);
	}
	return memcmp(s, t, len);
}

int lowest_bit(uint64 x)
{
	if (x == 0)
		return -1;
	return __builtin_ctzll(x);
}

int highest_bit(uint64 x)
{
	if (x == 0)
		return -1;
	return 63 - __builtin_clzll(x);
}

namespace math
{
	uint64 power(uint64 x, uint64 y)
	{
		uint64 ans = 1;
		while (y)
		{
			if (y & 1UL)
				ans *= x;
			x *= x;
			y >>= 1;
		}
		return ans;
	}
} // namespace math
