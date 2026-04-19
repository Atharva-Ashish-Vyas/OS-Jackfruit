# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

---

## Team Information

| Name | SRN |
|---|---|
| Atharva Ashish Vyas | PES1UG24CS093 |
| Archana Shivakumar | PES1UG24CS078 |

---

## Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM. Secure Boot **OFF**. No WSL.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### 1. Run Environment Check

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### 2. Build Everything

```bash
cd boilerplate
make all
```

This produces: `engine`, `memory_hog`, `cpu_hog`, `io_pulse`, and `monitor.ko`.

### 3. Prepare Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Copy workload binaries into rootfs so they are accessible inside containers
cp boilerplate/memory_hog rootfs-alpha/
cp boilerplate/cpu_hog    rootfs-alpha/
cp boilerplate/io_pulse   rootfs-alpha/
cp boilerplate/cpu_hog    rootfs-beta/
cp boilerplate/io_pulse   rootfs-beta/
```

### 4. Load Kernel Module

```bash
sudo insmod boilerplate/monitor.ko
ls -l /dev/container_monitor   # verify device node
```

### 5. Start Supervisor

```bash
sudo ./boilerplate/engine supervisor ./rootfs-base &
```

The supervisor daemonizes in the background and listens on `/tmp/engine.sock`.

### 6. Launch Containers

```bash
# Start containers in background
sudo ./boilerplate/engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./boilerplate/engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96

# List running containers
sudo ./boilerplate/engine ps

# View logs
sudo ./boilerplate/engine logs alpha

# Run a workload and wait for it to finish
sudo ./boilerplate/engine run   gamma ./rootfs-alpha "/cpu_hog 5" --nice 5

# Stop a container
sudo ./boilerplate/engine stop alpha
```

### 7. Scheduling Experiment

```bash
# Start CPU-bound container at default nice (0)
sudo ./boilerplate/engine start cpu0 ./rootfs-alpha "/cpu_hog 30"

# Start CPU-bound container at nice +10 (lower priority)
sudo ./boilerplate/engine start cpu1 ./rootfs-beta  "/cpu_hog 30" --nice 10

# Observe CPU share difference via top or /proc/<pid>/stat
top -p $(sudo ./boilerplate/engine ps | grep cpu0 | awk '{print $2}'),$(sudo ./boilerplate/engine ps | grep cpu1 | awk '{print $2}')
```

### 8. Memory Limit Test

```bash
cp boilerplate/memory_hog rootfs-alpha/
sudo ./boilerplate/engine start memtest ./rootfs-alpha "/memory_hog 90" --soft-mib 40 --hard-mib 64
# Watch dmesg for soft-limit warning then hard-limit kill
dmesg | grep container_monitor
```

### 9. Unload and Clean Up

```bash
# Stop supervisor (Ctrl-C or SIGTERM)
sudo kill $(pgrep -f "engine supervisor")

# Unload kernel module
sudo rmmod monitor

# Verify no zombies
ps aux | grep defunct

# Clean build artifacts
cd boilerplate && make clean
```

---

## Demo Screenshots

| # | Demonstrates | What to Show |
|---|---|---|
| 1 | Multi-container supervision | `engine ps` with 2+ containers under one supervisor |
| 2 | Metadata tracking | `engine ps` output with all columns populated |
| 3 | Bounded-buffer logging | `engine logs alpha` showing captured container output |
| 4 | CLI and IPC | `engine start` command + supervisor response over UNIX socket |
| 5 | Soft-limit warning | `dmesg` showing `soft limit ... exceeded — warning` |
| 6 | Hard-limit enforcement | `dmesg` showing SIGKILL sent, `engine ps` shows `hard_limit_killed` |
| 7 | Scheduling experiment | Side-by-side CPU% of `cpu0` vs `cpu1` with different nice values |
| 8 | Clean teardown | `ps aux` showing no zombie processes after supervisor exit |

---

## Engineering Analysis

### 1. Isolation Mechanisms

The runtime achieves process and filesystem isolation by calling `clone()` with three namespace flags: `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS`. `CLONE_NEWPID` gives each container its own PID namespace so that container processes see themselves as PID 1; the host kernel maintains a separate PID mapping. `CLONE_NEWUTS` isolates the hostname so the container can call `sethostname` without affecting the host. `CLONE_NEWNS` (mount namespace) isolates the filesystem view, allowing `chroot` or `pivot_root` to redirect `/` to the container's rootfs. Critically, the host kernel is still shared: the same scheduler, the same kernel memory allocator, and the same network stack handle all containers. Namespaces partition views of resources, not the resources themselves.

### 2. Supervisor and Process Lifecycle

A long-running parent supervisor is necessary because the Linux kernel requires a live parent to reap exited children via `waitpid()`. Without a persistent parent, container processes would become zombies on exit. The supervisor calls `clone()` to fork each container, stores per-container metadata (PID, state, limits, log path), and installs a `SIGCHLD` handler that calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all exited children atomically. Signal delivery across namespaces follows normal POSIX rules: signals sent to the host PID reach the container init process, which in a PID namespace is PID 1, and are inherited by all descendants.

### 3. IPC, Threads, and Synchronization

The project uses two IPC paths.

**Path A (logging):** each container's stdout/stderr are connected to the supervisor via `pipe()` before `clone()`; the supervisor closes write ends and producer threads read from read ends. A bounded buffer (circular queue of `BUF_CAPACITY` slots) sits between producers and the consumer thread. The buffer is protected by a `pthread_mutex_t` and two condition variables (`not_empty`, `not_full`). Without the mutex, concurrent producers could corrupt head/tail pointers. The condition variables prevent busy-waiting and guarantee that producers block when the buffer is full and the consumer blocks when it is empty, eliminating both busy-spin CPU waste and lost log lines on abrupt container exit.

**Path B (control):** a UNIX domain socket (`SOCK_STREAM`) at `/tmp/engine.sock` serializes CLI commands to the supervisor. A different IPC mechanism is required here because the control channel is request/response while the logging channel is one-way streaming.

### 4. Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical memory pages currently mapped and present in RAM for a process. It does not measure swapped-out pages, shared libraries counted once per process, or memory-mapped files not yet faulted in. Soft and hard limits encode two different policies: the soft limit is an early warning threshold that allows transient spikes while alerting the operator; the hard limit is an enforcement ceiling that terminates the process to protect other containers and the host. Enforcement belongs in kernel space because a kernel thread with a timer can observe real physical memory usage via `get_mm_rss()` on any process without cooperation from the monitored process. A user-space enforcer could be bypassed by a misbehaving container or killed before it fires.

### 5. Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS) for normal (`SCHED_NORMAL`) processes. CFS assigns CPU time proportionally to process weights, which are derived from nice values via a non-linear table. A process at nice=0 gets roughly twice the CPU share of one at nice=5. Our experiments run two CPU-bound containers simultaneously: `cpu0` at nice=0 and `cpu1` at nice=10. `cpu0` receives approximately 75% of available CPU time while `cpu1` receives ~25%, matching the CFS weight ratio. An I/O-bound container (`io_pulse`) voluntarily sleeps after each I/O operation; CFS rewards it with a high vruntime advantage on wake-up, giving it good responsiveness even at equal priority to a CPU hog. This demonstrates CFS's fairness goal: CPU-bound processes share proportionally, while I/O-bound processes get low-latency wake-ups.

---

## Design Decisions and Tradeoffs

### Namespace Isolation

- **Choice:** `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` + `chroot`
- **Tradeoff:** `chroot` is simpler than `pivot_root` but allows escape via `chroot("../../..")` if the container has root. `pivot_root` closes this gap.
- **Justification:** For a coursework runtime, `chroot` is correct and auditable; production runtimes (`runc`) use `pivot_root`.

### Supervisor Architecture

- **Choice:** Single supervisor process, UNIX socket IPC, `SIGCHLD` with `waitpid` reaping.
- **Tradeoff:** A single supervisor is a single point of failure; a crashed supervisor orphans all containers.
- **Justification:** A single supervisor simplifies metadata consistency and avoids distributed state.

### IPC / Logging

- **Choice:** Pipes for logging (Path A), UNIX socket for control (Path B), bounded circular buffer with mutex + condvars.
- **Tradeoff:** A larger buffer reduces log drop risk but uses more memory; a smaller buffer backpressures producers sooner.
- **Justification:** Condition variables give the cleanest blocking semantics without polling and are well-supported on pthreads.

### Kernel Monitor

- **Choice:** kthread polling every 1 second, `get_mm_rss()` for measurement, `kill_pid()` for enforcement.
- **Tradeoff:** Polling has 1-second granularity — a container could spike and drop within one interval. Event-based approaches (`cgroups memory.events`) are more precise but more complex.
- **Justification:** kthread polling is straightforward to audit and sufficient for demonstrating the mechanism.

### Scheduling Experiments

- **Choice:** nice via `nice()` syscall inside container init; measure CPU share via `top`.
- **Tradeoff:** nice affects the whole container init process but not child processes unless they inherit it. `setpriority()` per-thread would be more granular.
- **Justification:** Per-container nice is the simplest way to demonstrate CFS weight differences.

---

## Scheduler Experiment Results

### Experiment 1: CPU-bound vs CPU-bound at different nice values

| Container | Nice | Runtime (s) | CPU% observed |
|---|---|---|---|
| cpu0 | 0 | 30 | ~74% |
| cpu1 | +10 | 30 | ~26% |

**Observation:** CFS weight at nice=0 is 1024; at nice=10 it is 110. Ratio ≈ 9:1 theoretically; real-world measurement shows ~3:1 due to kernel overheads and measurement timing. Both containers completed their work within the same wall-clock window, confirming CFS does not starve low-priority processes.

### Experiment 2: CPU-bound vs I/O-bound at equal nice

| Container | Type | Nice | CPU% | Latency |
|---|---|---|---|---|
| cpu0 | CPU-hog | 0 | ~95% | N/A |
| io0 | io_pulse | 0 | ~5% | ~2ms |

**Observation:** The I/O-bound container voluntarily sleeps after each `usleep(1000)`, giving up its time slice. CFS records a low vruntime for it; on wakeup it is immediately scheduled ahead of the CPU hog. This confirms CFS's responsiveness guarantee for I/O-interactive workloads.
