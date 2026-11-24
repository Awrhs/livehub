// Package sysinfo 采集主机资源使用情况（Linux /proc 与 statfs），供管理后台展示与磁盘保护。
package sysinfo

import (
	"bufio"
	"os"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

// Disk 指定路径所在文件系统的容量（字节）。
func Disk(path string) (total, free uint64, err error) {
	var st syscall.Statfs_t
	if err = syscall.Statfs(path, &st); err != nil {
		return 0, 0, err
	}
	return st.Blocks * uint64(st.Bsize), st.Bavail * uint64(st.Bsize), nil
}

// Snapshot 一次采样结果。
type Snapshot struct {
	CPUPercent float64 `json:"cpu_percent"`
	CPUs       int     `json:"cpus"`
	MemTotal   uint64  `json:"mem_total"`
	MemUsed    uint64  `json:"mem_used"`
	DiskTotal  uint64  `json:"disk_total"`
	DiskFree   uint64  `json:"disk_free"`
	NetTxBps   float64 `json:"net_tx_bps"` // 出口带宽（比特/秒）
	NetRxBps   float64 `json:"net_rx_bps"`
	Load1      float64 `json:"load1"`
}

// Sampler 周期采样（CPU 与网络需要两次读数求差）。
type Sampler struct {
	Path string // 磁盘统计的路径（数据目录）

	mu       sync.Mutex
	last     Snapshot
	prevIdle uint64
	prevAll  uint64
	prevTx   uint64
	prevRx   uint64
	prevAt   time.Time
}

func (s *Sampler) Latest() Snapshot {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.last
}

// Sample 采样一次并更新 Latest。
func (s *Sampler) Sample() Snapshot {
	var snap Snapshot
	snap.CPUs = cpuCount()
	idle, all := cpuTimes()
	tx, rx := netBytes()
	now := time.Now()
	snap.MemTotal, snap.MemUsed = memInfo()
	snap.DiskTotal, snap.DiskFree, _ = Disk(s.Path)
	snap.Load1 = loadAvg()

	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.prevAt.IsZero() {
		if d := all - s.prevAll; d > 0 && all >= s.prevAll {
			snap.CPUPercent = 100 * (1 - float64(idle-s.prevIdle)/float64(d))
		}
		if secs := now.Sub(s.prevAt).Seconds(); secs > 0 && tx >= s.prevTx && rx >= s.prevRx {
			snap.NetTxBps = float64(tx-s.prevTx) * 8 / secs
			snap.NetRxBps = float64(rx-s.prevRx) * 8 / secs
		}
	}
	s.prevIdle, s.prevAll, s.prevTx, s.prevRx, s.prevAt = idle, all, tx, rx, now
	s.last = snap
	return snap
}

func cpuCount() int {
	f, err := os.Open("/proc/cpuinfo")
	if err != nil {
		return 0
	}
	defer f.Close()
	n := 0
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if strings.HasPrefix(sc.Text(), "processor") {
			n++
		}
	}
	return n
}

func cpuTimes() (idle, all uint64) {
	b, err := os.ReadFile("/proc/stat")
	if err != nil {
		return 0, 0
	}
	line := strings.SplitN(string(b), "\n", 2)[0]
	fs := strings.Fields(line)
	for i, f := range fs[1:] {
		v, _ := strconv.ParseUint(f, 10, 64)
		all += v
		if i == 3 || i == 4 { // idle + iowait
			idle += v
		}
	}
	return
}

func memInfo() (total, used uint64) {
	f, err := os.Open("/proc/meminfo")
	if err != nil {
		return 0, 0
	}
	defer f.Close()
	var avail uint64
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		fs := strings.Fields(sc.Text())
		if len(fs) < 2 {
			continue
		}
		v, _ := strconv.ParseUint(fs[1], 10, 64)
		switch fs[0] {
		case "MemTotal:":
			total = v * 1024
		case "MemAvailable:":
			avail = v * 1024
		}
	}
	if total > avail {
		used = total - avail
	}
	return
}

func netBytes() (tx, rx uint64) {
	f, err := os.Open("/proc/net/dev")
	if err != nil {
		return 0, 0
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := sc.Text()
		i := strings.Index(line, ":")
		if i < 0 {
			continue
		}
		name := strings.TrimSpace(line[:i])
		if name == "lo" {
			continue
		}
		fs := strings.Fields(line[i+1:])
		if len(fs) < 9 {
			continue
		}
		r, _ := strconv.ParseUint(fs[0], 10, 64)
		t, _ := strconv.ParseUint(fs[8], 10, 64)
		rx += r
		tx += t
	}
	return
}

func loadAvg() float64 {
	b, err := os.ReadFile("/proc/loadavg")
	if err != nil {
		return 0
	}
	fs := strings.Fields(string(b))
	if len(fs) == 0 {
		return 0
	}
	v, _ := strconv.ParseFloat(fs[0], 64)
	return v
}
