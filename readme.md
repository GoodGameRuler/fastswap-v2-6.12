# FastSwap v2 for Linux 6.12

Limitations, known bugs and buffer constants are in `design.md`. The client runs the 6.12 kernel with `kernel/kernel.patch` applied to `linux-6.12-fastswap`. The server VM runs the 4.11 v2 kernel with the v2 `rdma_rxe.ko` and `rmserver`.

Results on soft-RoCE (32M cgroup, UDP netdev): DRAM `touch 64 1` 0.22 s. RDMA `touch 64 1` 6.1 s, `touch 128 3` 23.9 s. MGLRU on: 24.6 s. `page-cluster` 0: 42.5 s. Two concurrent `touch 64 2`: 19.2 s. 0 bad pages, 0 failed stores.

### Build

1. Build the 6.12 kernel as in "Make Config" to "Install Modules" (`../docs/fastswap_build_steps-6.12.md`).
2. `mkdir -p build/v612-test build/share/v612`
3. `cp fastswap-v2-6.12/drivers/{*.c,*.h,Makefile} build/v612-test/`
4. In the `kbuild-6.12` container:
   `cd /work/build/v612-test`
   `make -j4 KDIR=/work/build/6.12-fastswap`
   `cp fastswap_dram.ko fastswap_rdma.ko /work/build/share/v612/`
   `strip --strip-debug /work/build/share/v612/*.ko`
5. Build the far memory server as in "Build the Far Memory Server" (`../fastswap-v2-proposal/README.md`).

### Stage the rdma Tool

The 6.12 guest has soft-RoCE built in but no `rdma` command.

```bash
mkdir -p build/share/rdmahost
cp /usr/bin/rdma build/share/rdmahost/
cp -L /usr/lib/libmnl.so.0 /usr/lib/libcap.so.2 /usr/lib/libc.so.6 /usr/lib64/ld-linux-x86-64.so.2 build/share/rdmahost/
```

### Create the Client Swap Disk

```bash
qemu-img create -f raw build/swap-256m.img 256M
```

### Boot the Two VMs

Create fresh overlays before every run.

```bash
cd build
qemu-img create -f qcow2 -b rootfs.img -F raw server.qcow2
qemu-img create -f qcow2 -b rootfs.img -F raw client612.qcow2
cd ..
```

Server, in its own terminal:

```bash
systemd-run --user --scope -p MemoryHigh=2300M -p MemoryMax=2600M -p MemorySwapMax=0 -p CPUQuota=500% -p IOWeight=20 \
  qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 1G -nographic \
  -kernel build/4.11-fastswap-v2/arch/x86/boot/bzImage \
  -append "root=/dev/vda rw console=ttyS0 nokaslr" \
  -drive file=build/server.qcow2,format=qcow2,if=virtio \
  -netdev socket,id=n0,udp=127.0.0.1:5556,localaddr=127.0.0.1:5555 \
  -device virtio-net-pci,netdev=n0,mac=52:54:00:00:00:01 \
  -virtfs local,path=build/share,mount_tag=host,security_model=none,readonly=on
```

Client, in its own terminal.

```bash
systemd-run --user --scope -p MemoryHigh=2300M -p MemoryMax=2600M -p MemorySwapMax=0 -p CPUQuota=500% -p IOWeight=20 \
  qemu-system-x86_64 -enable-kvm -cpu host -smp 8 -m 1G -nographic \
  -kernel build/6.12-fastswap/arch/x86/boot/bzImage \
  -append "root=/dev/vda rw console=ttyS0 nokaslr cgroup_no_v1=memory" \
  -drive file=build/client612.qcow2,format=qcow2,if=virtio \
  -drive file=build/swap-256m.img,format=raw,if=virtio \
  -netdev socket,id=n0,udp=127.0.0.1:5555,localaddr=127.0.0.1:5556 \
  -device virtio-net-pci,netdev=n0,mac=52:54:00:00:00:02 \
  -virtfs local,path=build/share,mount_tag=host,security_model=none,readonly=on
```

Log in as `root` (empty password) on both. Do not use `-netdev socket` over TCP: Nagle and delayed ACK add about 41 ms per operation.

### Server Setup

The NIC is `ens3`. Load the v2 rxe module and start `rmserver`.

```bash
mkdir -p /mnt/host && mount -t 9p -o trans=virtio,version=9p2000.L,msize=262144 host /mnt/host
modprobe ib_core; modprobe rdma_cm; modprobe ib_uverbs; modprobe rdma_ucm; modprobe udp_tunnel; modprobe ip6_udp_tunnel
modprobe rdma_rxe; rmmod rdma_rxe; insmod /mnt/host/v2/rdma_rxe.ko
ip addr add 10.0.0.1/24 dev ens3; ip link set ens3 mtu 1500 up
echo ens3 > /sys/module/rdma_rxe/parameters/add
stdbuf -oL /mnt/host/v2/rmserver 50000 512 > /tmp/rm.log 2>&1 &
```

Restart `rmserver` before every client run:

```bash
pkill rmserver; stdbuf -oL /mnt/host/v2/rmserver 50000 512 > /tmp/rm.log 2>&1 &
```

### Client Setup

The NIC is `enp0s3`. Soft-RoCE is built into the 6.12 kernel.

```bash
mkdir -p /mnt/host && mount -t 9p -o trans=virtio,version=9p2000.L,msize=262144 host /mnt/host
mount -t debugfs none /sys/kernel/debug
ip addr add 10.0.0.2/24 dev enp0s3; ip link set enp0s3 mtu 1500 up
/mnt/host/rdmahost/ld-linux-x86-64.so.2 --library-path /mnt/host/rdmahost /mnt/host/rdmahost/rdma link add rxe0 type rxe netdev enp0s3
ping -c 2 10.0.0.1
```

Load the RDMA backend and enable the swap device.

```bash
insmod /mnt/host/v612/fastswap_rdma.ko server_ip=10.0.0.1 server_port=50000 client_ip=10.0.0.2
mkswap /dev/vdb; swapon /dev/vdb
```

Create the test cgroup.

```bash
mkdir -p /cgroup2 && mount -t cgroup2 nodev /cgroup2
echo +memory > /cgroup2/cgroup.subtree_control
mkdir /cgroup2/test
echo 32M > /cgroup2/test/memory.high
echo 3 > /proc/sys/vm/page-cluster
```

### Run the Workload

`touch <MiB> [passes]` (`workload/touch.c`) maps that much anonymous memory, writes a tag into every page, then reads every page back `passes` times and checks the tag. The cgroup limit is 32M, so most of the memory is swapped to the far memory server and faulted back in. `touch` prints the swap counters after each phase and ends with `<pages> pages, <bad> bad`.

```bash
echo $$ > /cgroup2/test/cgroup.procs
/mnt/host/touch 64 1
cat /sys/kernel/debug/frontswap/{succ_stores,loads,invalidates,failed_stores,curr_pages}
```

`touch 64 1` maps 64 MiB (16384 pages) and reads it once. Expect `16384 pages, 0 bad` and `failed_stores` 0. The counters are:

| Counter | Meaning |
|---|---|
| `succ_stores` | Pages written to far memory |
| `loads` | Pages read back from far memory |
| `invalidates` | Slots released |
| `failed_stores` | Writes that failed and went to the swap device instead |
| `curr_pages` | Pages in far memory now |

Reboot the client with a fresh overlay before each variant below, because a backend cannot be reloaded within one boot. Restart `rmserver` on the server each time.

#### Longer Run

```bash
/mnt/host/touch 128 3
```

128 MiB is four times the limit, and three read passes fault every page back in three times. This is the main timing run.

#### Readahead Off

```bash
echo 0 > /proc/sys/vm/page-cluster
/mnt/host/touch 128 3
```

`page-cluster` is the base-2 logarithm of the swap-in readahead window. The setup used 3, which is 8 pages. 0 reads only the faulted page, so the run shows how much the prefetch reads help.

#### MGLRU

```bash
echo y > /sys/kernel/mm/lru_gen/enabled
/mnt/host/touch 128 3
```

MGLRU replaces the two-list LRU that chooses which pages to evict. The run shows whether the eviction order changes the time.

#### Two Processes

```bash
for i in 1 2; do (bash -c 'echo $$ > /cgroup2/test/cgroup.procs; exec /mnt/host/touch 64 2' > /tmp/c$i.out 2>&1 &); done
```

Each background shell moves itself into the test cgroup, then replaces itself with `touch 64 2`. Both processes share the 32M limit and fault at the same time on different CPUs, which exercises several queue sets at once. When both finish, `cat /tmp/c1.out /tmp/c2.out` must show `0 bad` for each.

### Fewer Queues

Boot the client with `-smp 4` and load the module with `queues=2`. The server is unchanged.

```bash
insmod /mnt/host/v612/fastswap_rdma.ko server_ip=10.0.0.1 server_port=50000 client_ip=10.0.0.2 queues=2
```

### Reclaim CPU

```bash
cat /sys/module/kernel/parameters/fastswap_reclaim_cpu
echo 3 > /sys/module/kernel/parameters/fastswap_reclaim_cpu
echo 1 > /sys/kernel/tracing/events/workqueue/workqueue_execute_start/enable
echo 1 > /sys/kernel/tracing/tracing_on
```

Run the workload, then count the CPUs that ran the reclaim worker:

```bash
grep high_work_func /sys/kernel/tracing/trace | awk '{print $2}' | sort | uniq -c
```

The default is -1 (unpinned). After `echo 3` every run is on CPU 3.

### Fault Test

Start the workload in the background on the client:

```bash
(bash -c 'echo $$ > /cgroup2/test/cgroup.procs; exec /mnt/host/touch 128 3'; echo EXIT=$?) &
```

Four seconds later, in the server VM:

```bash
pkill rmserver
```

Within about 4 seconds the client prints `Bus error` and `EXIT=135`. The guest stays up. Check the log:

```bash
dmesg | grep -i -E 'WARN|BUG|lockdep|scheduling while|Call Trace|rcu|hung|stall'
```

### Swapoff

After a run:

```bash
swapoff /dev/vdb; swapon /dev/vdb
```

### DRAM Backend

Skip the server VM and the `rdma link add` and `ping` steps. Load the DRAM backend in place of the RDMA backend:

```bash
insmod /mnt/host/v612/fastswap_dram.ko size_mb=256
```

`rmmod fastswap_dram` prints `Device or resource busy`.

### Notes

- Load one backend per boot. No rmmod currently.
- The test kernel has no lockdep or `DEBUG_PREEMPT`: the splat greps cover less than on a debug kernel.
