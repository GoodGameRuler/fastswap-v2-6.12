#ifndef FASTSWAP_H
#define FASTSWAP_H

#include <linux/mm_types.h>

static inline u64 fastswap_offset(pgoff_t index)
{
	return (u64)index << PAGE_SHIFT;
}

#endif
