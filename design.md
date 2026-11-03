# FastSwap v2 (Linux 6.12): Design Notes

## Limitations
- comp_vector set to 0
- No large folio swapping
- One backend per boot: the hook cannot unregister, so `rmmod` is refused
- `CONFIG_FASTSWAP` excludes `ZSWAP`
- The swap area must not be larger than the far memory buffer (`size_mb`)
- IPv4 only
- No reconnect after a server failure
