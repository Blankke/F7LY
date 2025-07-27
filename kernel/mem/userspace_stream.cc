//
// Copied from Li shuang ( pseudonym ) on 2024-08-07
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

#include "mem/userspace_stream.hh"

#include "klib.hh"

using namespace mem;

UserspaceStream::UserspaceStream( void *u_addr_start, u64 u_size, PT *u_pt )
	: _start_addr( (ulong) u_addr_start )
	, _end_addr( _start_addr + u_size )
	, _pt( u_pt )
{}

using RC = UserspaceStream::RetCode;

RC UserspaceStream::open()
{
	_ptr = _start_addr;
	_update_cache( (void *) _ptr );

	return rc_ok;
}

RC UserspaceStream::close()
{
	_ptr = 0;
	return rc_ok;
}

UserspaceStream &UserspaceStream::operator<<( UsRangeDesc &rd )
{
	if ( _ptr == 0 )
	{
		_errno = rc_not_open;
		return *this;
	}

	auto [buf, len] = rd;

	len = ( _ptr + len > _end_addr ) ? ( _end_addr - _ptr ) : len;
	while ( len > 0 )
	{
		if ( _cache_ptr + len <= _cache_end )
		{
			memcpy( (void *) _cache_ptr, (void *) buf, len );
			_cache_ptr += len;
			_ptr	   += len;
			_errno		= rc_ok;

			return *this;
		}
		else
		{
			u64 l = _cache_end - _cache_ptr;
			memcpy( (void *) _cache_ptr, (void *) buf, l );
			_cache_ptr += l;
			_ptr	   += l;
			buf		   += l;
			len		   -= l;
			_update_cache( (void *) _ptr );
		}
	}
	_errno = rc_ok;

	return *this;
}

UserspaceStream &UserspaceStream::operator>>( UsRangeDesc &rd )
{
	// printfCyan("[UserspaceStream::operator>>] _ptr: %p, _end_addr: %p\n", _ptr, _end_addr);
	if (_ptr == 0)
	{
		_errno = rc_not_open;
		return *this;
	}
// printfCyan("[UserspaceStream::operator>>] _ptr: %p, _end_addr: %p\n", _ptr, _end_addr);
	auto [buf, len] = rd;

	len = ( _ptr + len > _end_addr ) ? ( _end_addr - _ptr ) : len;
	while ( len > 0 )
	{
		// printfCyan("[UserspaceStream::operator>>] _cache_ptr: %p, _cache_end: %p, len: %u\n", _cache_ptr, _cache_end, len);
		
		// 检查缓存是否有效
		if (_cache_ptr >= _cache_end) {
			// printfRed("[UserspaceStream::operator>>] Invalid cache: _cache_ptr >= _cache_end\n");
			_errno = rc_fail;
			return *this;
		}
		
		if ( _cache_ptr + len <= _cache_end )
		{
			// printfGreen("simple copy: len=%u\n", len);
			memcpy( (void *) buf, (void *) _cache_ptr, len );
			_cache_ptr += len;
			_ptr	   += len;
			return *this;
		}
		else
		{
			u64 l = _cache_end - _cache_ptr;
			// printfRed("split copy: l=%u, len=%u\n", l, len);
			
			// 如果 l == 0，说明当前缓存已满，需要移动到下一个页面
			if (l == 0) {
				_ptr++;  // 移动到下一个字节
				_update_cache( (void *) _ptr );
				continue;
			}
			
			memcpy( (void *) buf, (void *) _cache_ptr, l );
			_cache_ptr += l;
			_ptr	   += l;
			buf		   += l;
			len		   -= l;
			_update_cache( (void *) _ptr );
		}
	}

	return *this;
}


