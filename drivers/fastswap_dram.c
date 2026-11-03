#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>
#include <linux/fastswap.h>
#include "fastswap.h"

static unsigned long size_mb = 1024;
module_param(size_mb, ulong, 0444);
MODULE_PARM_DESC(size_mb, "far memory size in MiB; the swap device must not be larger");

static void *far_memory;

static int dram_store(unsigned type, pgoff_t index, struct folio *folio)
{
    u64 offset = fastswap_offset(index);
    void *src;

    if (folio_test_large(folio) || offset + PAGE_SIZE > size_mb << 20) {
        return -ENOSPC;
    }

    src = kmap_local_folio(folio, 0);
    copy_page(far_memory + offset, src);
    kunmap_local(src);

    return 0;
}

static int dram_load(unsigned type, pgoff_t index, struct folio *folio)
{
    void *dst = kmap_local_folio(folio, 0);

    copy_page(dst, far_memory + fastswap_offset(index));
    kunmap_local(dst);

    folio_mark_uptodate(folio);
    folio_unlock(folio);

    return 0;
}

static int dram_poll_load(int cpu)
{
    return 0;
}

static struct fastswap_ops dram_ops = {
    .store = dram_store,
    .load = dram_load,
    .load_async = dram_load,
    .poll_load = dram_poll_load,
};

static int __init dram_init(void)
{
    far_memory = vzalloc(size_mb << 20);
    if (!far_memory) {
        return -ENOMEM;
    }

    return fastswap_register_ops(&dram_ops);
}

module_init(dram_init);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("FastSwap DRAM backend");
