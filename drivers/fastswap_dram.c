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
    long page_index;

    if (offset + folio_size(folio) > size_mb << 20) {
        return -ENOSPC;
    }

    for (page_index = 0; page_index < folio_nr_pages(folio); page_index++) {
        void *src = kmap_local_folio(folio, page_index * PAGE_SIZE);

        copy_page(far_memory + offset + page_index * PAGE_SIZE, src);
        kunmap_local(src);
    }

    return 0;
}

static int dram_load(unsigned type, pgoff_t index, struct folio *folio)
{
    u64 offset = fastswap_offset(index);
    long page_index;

    for (page_index = 0; page_index < folio_nr_pages(folio); page_index++) {
        void *dst = kmap_local_folio(folio, page_index * PAGE_SIZE);

        copy_page(dst, far_memory + offset + page_index * PAGE_SIZE);
        kunmap_local(dst);
    }

    folio_mark_uptodate(folio);
    folio_unlock(folio);

    return 0;
}

static int dram_poll_load(int cpu)
{
    return 0;
}

static struct fastswap_ops dram_ops = {
    .owner = THIS_MODULE,
    .store = dram_store,
    .load = dram_load,
    .load_async = dram_load,
    .poll_load = dram_poll_load,
};

static int __init dram_init(void)
{
    int ret;

    far_memory = vzalloc(size_mb << 20);
    if (!far_memory) {
        return -ENOMEM;
    }

    ret = fastswap_register_ops(&dram_ops);
    if (ret) {
        vfree(far_memory);
    }

    return ret;
}

static void __exit dram_exit(void)
{
    fastswap_unregister_ops(&dram_ops);
    vfree(far_memory);
}

module_init(dram_init);
module_exit(dram_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("FastSwap DRAM backend");
