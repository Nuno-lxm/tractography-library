#!/bin/bash
#SBATCH --account=I20240002X
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --time=00:15:00
#SBATCH --exclusive
#SBATCH --partition=normal-x86
#SBATCH --job-name=node_bench
#SBATCH --no-requeue
#SBATCH --output=/projects/I20240002/nunolxm/output/tests/isolated/node_bench/node_bench_%j_%N.out

# Quick per-core speed check of an x86 node: clock and boost settings, then
# single-core gzip, Python and openssl timings and an all-core openssl run.
# Run it on a suspect node and a control node at the same time and compare.
#
# Usage:
#   sbatch --nodelist=cnx135 node_bench.sh
#   sbatch --exclude=cnx135 node_bench.sh

set -uo pipefail

core=2
sample="/projects/I20240002/nunolxm/data/resample_dwi/mgh1/S1/dwi.nii.gz"
raw="/dev/shm/node_bench_$SLURM_JOB_ID.raw"
trap 'rm -f "$raw"' EXIT

# MHz of $core, read from /proc/cpuinfo
core_mhz() {
    awk -v c="$core" '$1 == "processor" { p = $3 } p == c && /^cpu MHz/ { print $4; exit }' /proc/cpuinfo
}

# Seconds taken by a command pinned to $core, with the core's MHz 2 s in
timed() {
    local t0 t1 pid mhz
    t0=$(date +%s.%N)
    taskset -c "$core" "$@" > /dev/null &
    pid=$!
    sleep 2; mhz=$(core_mhz)
    wait "$pid"
    t1=$(date +%s.%N)
    printf '%7.2f s  (core %d at %s MHz)\n' "$(echo "$t1 - $t0" | bc)" "$core" "$mhz"
}

echo "Node: $(hostname)  $(date '+%F %T')  job $SLURM_JOB_ID"
lscpu | grep -E '^(Model name|CPU\(s\)|Socket\(s\)|CPU max MHz|CPU min MHz|CPU MHz)'
for f in /sys/devices/system/cpu/cpufreq/boost \
         /sys/devices/system/cpu/cpu$core/cpufreq/{scaling_driver,scaling_governor,cpuinfo_max_freq,scaling_max_freq,scaling_cur_freq}; do
    [[ -r "$f" ]] && echo "$f: $(cat "$f")"
done
command -v cpupower > /dev/null && cpupower frequency-info 2> /dev/null | grep -E 'driver|governor|boost|limits|current'
echo "Idle core $core: $(core_mhz) MHz"
uptime
free -g | head -2

# 512 MiB of real DWI data in memory, so the timings don't touch Lustre
gzip -dc "$sample" | head -c 536870912 > "$raw"

echo "gzip -6, 512 MiB, single core:"
for i in 1 2 3; do printf '  run %d ' "$i"; timed gzip -6 -c "$raw"; done

echo "Python loop, single core:"
for i in 1 2 3; do
    printf '  run %d ' "$i"
    timed python3 -c 's = 0
for i in range(40000000): s += i * i'
done

echo "openssl sha256, single core (16384-byte blocks, kB/s):"
taskset -c "$core" openssl speed -seconds 5 sha256 2> /dev/null | tail -1

echo "openssl sha256, all 128 cores (kB/s):"
openssl speed -multi 128 -seconds 5 sha256 2> /dev/null | tail -1

echo "Done: $(date '+%F %T')"
