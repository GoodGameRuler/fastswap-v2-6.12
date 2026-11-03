/*
 * Allocate N MiB, write every page, then read every page back and verify.
 * Run inside a cgroup whose memory.high is below N so pages go to far memory.
 * Usage: touch <MiB> [passes]
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_TAG_MASK 0x5a5a5a5aUL

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

static void print_counters(const char *phase) {
    printf("%-8s stores=%lu loads=%lu curr_pages=%lu failed=%lu\n", phase,
            read_frontswap_counter("succ_stores"), read_frontswap_counter("loads"),
            read_frontswap_counter("curr_pages"), read_frontswap_counter("failed_stores"));
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
    uint8_t *memory;

    memory = mmap(NULL, page_count * page_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (memory == MAP_FAILED) {
        perror("mmap");
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
