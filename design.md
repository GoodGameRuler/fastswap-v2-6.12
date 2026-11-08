# FastSwap v2 (Linux 6.12): Design Notes

## Limitations
- comp_vector set to 0
- Large folios need `CONFIG_TRANSPARENT_HUGEPAGE` and a non-rotational swap device (`cluster_info` exists only then)
- A run that mixes far memory slots and swap disk slots is a fail-safe SIGBUS, and the `can_swapin_thp` gate makes it fall back to order 0 instead
- Large swap-in happens only on synchronous swap devices. On `/dev/vdb` every swap-in is order 0
- 64 KiB writes on soft-RoCE hit retry-exceeded errors under load and fall back to the swap disk with correct data
- One backend at a time. `rmmod` is refused while pages are in far memory, so run `swapoff` first
- `curr_pages` stays above zero after a workload (5 to 53 slots in tests, cause not found), so `rmmod` always needs `swapoff`
- `CONFIG_FASTSWAP` excludes `ZSWAP`
- The swap area must not be larger than the far memory buffer (`size_mb`)
- IPv4 only
- No reconnect after a server failure
