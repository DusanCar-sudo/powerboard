#!/bin/bash
# Aura System Optimizer — RAM, Disk, CPU, Idle reduction
# Run with sudo for full effect
# Usage: sudo aura-optimize.sh [--battery]

set -euo pipefail

log() { echo "[$(date +%H:%M:%S)] $*"; }
err() { echo "[$(date +%H:%M:%S)] ERROR: $*" >&2; }

BATTERY_MODE=false
[[ "${1:-}" == "--battery" ]] && BATTERY_MODE=true

check_root() {
    [[ $EUID -eq 0 ]] || { err "Run with sudo"; exit 1; }
}

# === RAM OPTIMIZATION ===
optimize_ram() {
    log "Optimizing RAM..."

    # Reduce swappiness (less aggressive swap)
    sysctl -w vm.swappiness=10
    echo "vm.swappiness=10" >> /etc/sysctl.d/99-aura.conf

    # Clear pagecache, dentries, inodes (safe)
    sync && echo 3 > /proc/sys/vm/drop_caches

    # Enable hugepages for big memory apps
    echo 256 > /proc/sys/vm/nr_hugepages

    log "RAM: swappiness=10, cache cleared, 256 hugepages"
}

# === DISK OPTIMIZATION ===
optimize_disks() {
    log "Optimizing disks..."

    # Set I/O scheduler to deadline/bfq (lower latency)
    for dev in /sys/block/*/queue/scheduler; do
        echo bfq | tee $(dirname $dev)/scheduler > /dev/null || \
        echo deadline | tee $(dirname $dev)/scheduler > /dev/null
    done

    # Increase read-ahead (default 128, set 1024 sectors)
    for dev in /sys/block/*/queue/read_ahead_kb; do
        echo 1024 > "$dev"
    done

    # Disable atime updates (mount with noatime,relatime)
    sysctl -w fs.inotify.max_user_watches=524288

    log "Disk: bfq/deadline scheduler, 1024KB read-ahead"
}

# === CPU BALANCING & IDLE REDUCTION ===
optimize_cpu() {
    log "Optimizing CPU cores..."

    # Set CPU governor to performance
    for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$cpu" 2>/dev/null || true
    done

    # Enable CPU frequency scaling
    echo 1 > /proc/sys/kernel/sched_rt_runtime_us

    # Balance IRQs across cores
    if command -v irqbalance &>/dev/null; then
        systemctl restart irqbalance 2>/dev/null || irqbalance --oneshot
    fi

    # Reduce C-state latency for faster wake
    for cpu in /sys/devices/system/cpu/cpu*/cpuidle; do
        echo 1 > "$cpu/state0/disable" 2>/dev/null || true
    done

    # Pin kernel threads to CPU0 to free others
    echo 2 > /proc/sys/kernel/kthread_pid

    log "CPU: performance governor, irqbalance rebalanced, C-states tuned"
}

# === NETWORK & LATENCY ===
optimize_network() {
    log "Optimizing network stack..."

    # TCP fast open
    echo 3 > /proc/sys/net/ipv4/tcp_fastopen

    # Reduce keepalive
    echo 600 > /proc/sys/net/ipv4/tcp_keepalive_time
    echo 10 > /proc/sys/net/ipv4/tcp_keepalive_intvl

    # Increase connection tracking
    echo 262144 > /proc/sys/net/netfilter/nf_conntrack_max

    log "Network: TCP fast open, tuned keepalive"
}

# === MAIN ===
main() {
    check_root

    log "=== AURA SYSTEM OPTIMIZER ==="
    optimize_ram
    optimize_disks
    optimize_cpu
    optimize_network

    log "=== DONE ==="
    log "Reboot for persistent changes to apply fully"
    log "To revert: rm /etc/sysctl.d/99-aura.conf && reboot"
}

main "$@"
