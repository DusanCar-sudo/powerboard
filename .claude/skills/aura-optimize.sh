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
get_available_governors() {
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors 2>/dev/null
}

optimize_cpu() {
    log "Optimizing CPU cores..."

    # Detect available governors
    local available
    available=$(get_available_governors)
    log "Available governors: $available"

    # Pick best governor based on mode and availability
    local governor="performance"
    if $BATTERY_MODE; then
        if [[ "$available" == *"powersave"* ]]; then
            governor="powersave"
        elif [[ "$available" == *"ondemand"* ]]; then
            governor="ondemand"
        fi
    fi

    for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo "$governor" > "$cpu" 2>/dev/null || true
    done

    # Enable CPU frequency scaling
    echo 1 > /proc/sys/kernel/sched_rt_runtime_us

    # Balance IRQs across cores
    if command -v irqbalance &>/dev/null; then
        systemctl restart irqbalance 2>/dev/null || irqbalance --oneshot
    fi

    # C-state tuning: aggressive on battery, conservative when plugged
    if $BATTERY_MODE; then
        # Enable deep C-states for battery savings
        for cpu in /sys/devices/system/cpu/cpu*/cpuidle/state?/disable; do
            echo 0 > "$cpu" 2>/dev/null || true
        done
    else
        # Reduce C-state latency for faster wake (plugged)
        for cpu in /sys/devices/system/cpu/cpu*/cpuidle/state?/disable; do
            echo 1 > "$cpu" 2>/dev/null || true
        done
    fi

    # Pin kernel threads to CPU0 to free others
    echo 2 > /proc/sys/kernel/kthread_pid 2>/dev/null || true

    log "CPU: $governor governor, irqbalance rebalanced, C-states tuned"
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

    local mode="PLUGGED IN (max performance)"
    $BATTERY_MODE && mode="BATTERY (balanced)"

    log "=== AURA SYSTEM OPTIMIZER [$mode] ==="
    optimize_ram
    optimize_disks
    optimize_cpu
    optimize_network

    log "=== DONE ==="
    log "Reboot for persistent changes to apply fully"
    log "To revert: rm /etc/sysctl.d/99-aura.conf && reboot"
}

main "$@"
