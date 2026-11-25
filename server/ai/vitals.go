package ai

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"syscall"
)

func getSystemVitals() string {
	cpuLoad := getCPULoad()
	cpuTemp := getCPUTemp()
	memInfo := getMemoryInfo()
	diskInfo := getDiskInfo()

	return fmt.Sprintf("CPU Load: %s, Temp: %s, RAM: %s, Disk: %s", cpuLoad, cpuTemp, memInfo, diskInfo)
}

func getCPULoad() string {
	data, err := os.ReadFile("/proc/loadavg")
	if err != nil {
		return "N/A"
	}
	fields := strings.Fields(string(data))
	if len(fields) > 0 {
		return fields[0]
	}
	return "N/A"
}

func getMemoryInfo() string {
	data, err := os.ReadFile("/proc/meminfo")
	if err != nil {
		return "N/A"
	}
	var totalKB, availKB float64
	for _, line := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(line, "MemTotal:") {
			fields := strings.Fields(line)
			if len(fields) >= 2 {
				totalKB, _ = strconv.ParseFloat(fields[1], 64)
			}
		} else if strings.HasPrefix(line, "MemAvailable:") {
			fields := strings.Fields(line)
			if len(fields) >= 2 {
				availKB, _ = strconv.ParseFloat(fields[1], 64)
			}
		}
	}
	if totalKB == 0 {
		return "N/A"
	}
	availGB := availKB / (1024 * 1024)
	totalGB := totalKB / (1024 * 1024)
	return fmt.Sprintf("%.1f GB free of %.1f GB", availGB, totalGB)
}

func getCPUTemp() string {
	for i := 0; i < 4; i++ {
		path := fmt.Sprintf("/sys/class/thermal/thermal_zone%d/temp", i)
		data, err := os.ReadFile(path)
		if err == nil {
			raw := strings.TrimSpace(string(data))
			milli, err := strconv.Atoi(raw)
			if err == nil && milli > 0 && milli < 120000 {
				return fmt.Sprintf("%d°C", milli/1000)
			}
		}
	}
	return "N/A"
}

func getDiskInfo() string {
	var stat syscall.Statfs_t
	err := syscall.Statfs("/", &stat)
	if err != nil {
		return "N/A"
	}
	freeBytes := stat.Bavail * uint64(stat.Bsize)
	totalBytes := stat.Blocks * uint64(stat.Bsize)

	freeGB := float64(freeBytes) / (1024 * 1024 * 1024)
	totalGB := float64(totalBytes) / (1024 * 1024 * 1024)
	return fmt.Sprintf("%.1f GB free of %.1f GB", freeGB, totalGB)
}
