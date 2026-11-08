/*
 * Like touch, but the region is aligned to ALIGNMENT_BYTES and marked MADV_HUGEPAGE so that the
 * kernel can use large folios for it. Run with the 64 KiB mTHP size enabled
 * (hugepages-64kB/enabled = always) and inside a cgroup whose memory.high is below N.
 * Usage: touch_thp <MiB> [passes]
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_TAG_MASK 0x5a5a5a5aUL
#define ALIGNMENT_BYTES (64UL * 1024)
#define MTHP_STATS_DIR "/sys/kernel/mm/transparent_hugepage/hugepages-64kB/stats"

static uint64_t read_frontswap_counter(const char *name) {
    char path[128];
    uint64_t value = 0;
    FILE *file;

    snprintf(path, sizeof(path), "/sys/kernel/debug/frontswap/%s", name);

    file = fopen(path, "r");
    if (file) {
        if (fscanf(file, "%lu", &value) != 1) {
            value = 0;
        }
        fclose(file);
    }

    return value;
}

static uint64_t read_mthp_stat(const char *name) {
    char path[160];
    uint64_t value = 0;
    FILE *file;

    snprintf(path, sizeof(path), "%s/%s", MTHP_STATS_DIR, name);

    file = fopen(path, "r");
    if (file) {
        if (fscanf(file, "%lu", &value) != 1) {
            value = 0;
        }
        fclose(file);
    }

    return value;
}

static void print_counters(const char *phase) {
    printf("%-8s stores=%lu loads=%lu curr_pages=%lu failed=%lu large_stores=%lu\n", phase,
            read_frontswap_counter("succ_stores"), read_frontswap_counter("loads"),
            read_frontswap_counter("curr_pages"), read_frontswap_counter("failed_stores"),
            read_frontswap_counter("large_stores"));
    printf("%-8s mthp64k: anon_fault_alloc=%lu swpout=%lu swpout_fallback=%lu split=%lu\n", phase,
            read_mthp_stat("anon_fault_alloc"), read_mthp_stat("swpout"),
            read_mthp_stat("swpout_fallback"), read_mthp_stat("split"));
    fflush(stdout);
}

static uint64_t expected_tag(size_t page_index) {
    return page_index ^ PAGE_TAG_MASK;
}

int main(int argc, char **argv) {
    size_t size_mib = argc > 1 ? strtoul(argv[1], NULL, 0) : 64;
    int read_passes = argc > 2 ? atoi(argv[2]) : 1;
    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t page_count = size_mib * 1024 * 1024 / page_size;
    size_t bad_pages = 0;
    uint8_t *mapping;
    uint8_t *memory;

    mapping = mmap(NULL, page_count * page_size + ALIGNMENT_BYTES, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (mapping == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    memory = (uint8_t *)(((uintptr_t)mapping + ALIGNMENT_BYTES - 1) & ~(ALIGNMENT_BYTES - 1));

    if (madvise(memory, page_count * page_size, MADV_HUGEPAGE)) {
        perror("madvise(MADV_HUGEPAGE)");
        return 1;
    }

    print_counters("start");
    for (size_t page_index = 0; page_index < page_count; page_index++) {
        *(uint64_t *)(memory + page_index * page_size) = expected_tag(page_index);
    }
    print_counters("written");

    for (int pass = 0; pass < read_passes; pass++) {
        for (size_t page_index = 0; page_index < page_count; page_index++) {
            if (*(uint64_t *)(memory + page_index * page_size) != expected_tag(page_index)) {
                bad_pages++;
            }
        }

        print_counters("read");
    }

    printf("%zu pages, %zu bad\n", page_count, bad_pages);

    return bad_pages != 0;
}
