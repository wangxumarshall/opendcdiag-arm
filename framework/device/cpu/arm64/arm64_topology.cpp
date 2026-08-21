/*
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "topology.h"
#include "sandstone_p.h"
#include "sandstone_utils.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <sys/auxv.h>
#endif

namespace {

static bool create_mock_topology(const char *)
{
    return false;
}

enum class TopologySource {
    ACPI_PPTT,
    Sysfs,
    DeviceTree,
    ProcCpuinfo,
};

class Arm64TopologyDetector
{
public:
    Arm64TopologyDetector();
    void detect(const LogicalProcessorSet &enabled_cpus);
    void sort();

private:
    bool detect_via_acpi_pptt();
    bool detect_via_sysfs();
    bool detect_via_device_tree();
    bool detect_via_proc_cpuinfo();

    bool parse_cpu_topology_sysfs(int cpu, Topology::Thread *info);
    bool parse_numa_topology_sysfs();
    bool parse_cache_info(int cpu, Topology::Thread *info);

    int detect_sysfs_cpu_count();
    bool read_sysfs_file(int dirfd, const char *filename, char *buf, size_t bufsize);
    bool read_sysfs_int(int dirfd, const char *filename, int *value);
    bool read_sysfs_int64(int dirfd, const char *filename, int64_t *value);

    TopologySource detect_package_info();
    int detect_core_type_from_midr(uint64_t midr);

    int last_package_id = -1;
    int sysfs_cpu_count = 0;
    TopologySource used_source = TopologySource::Sysfs;
};

static FILE *fopenat(int dfd, const char *name)
{
    FILE *f = nullptr;
    int fd = openat(dfd, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return f;
    f = fdopen(fd, "r");
    if (!f)
        close(fd);
    return f;
}

Arm64TopologyDetector::Arm64TopologyDetector()
{
}

bool Arm64TopologyDetector::read_sysfs_file(int dirfd, const char *filename, char *buf, size_t bufsize)
{
    auto_fd fd{ openat(dirfd, filename, O_RDONLY | O_CLOEXEC) };
    if (fd < 0)
        return false;

    ssize_t n = pread(fd, buf, bufsize - 1, 0);
    if (n <= 0)
        return false;

    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    return true;
}

bool Arm64TopologyDetector::read_sysfs_int(int dirfd, const char *filename, int *value)
{
    char buf[64];
    if (!read_sysfs_file(dirfd, filename, buf, sizeof(buf)))
        return false;

    char *endptr;
    long val = strtol(buf, &endptr, 10);
    if (endptr == buf || *endptr != '\0')
        return false;

    *value = static_cast<int>(val);
    return true;
}

bool Arm64TopologyDetector::read_sysfs_int64(int dirfd, const char *filename, int64_t *value)
{
    char buf[64];
    if (!read_sysfs_file(dirfd, filename, buf, sizeof(buf)))
        return false;

    char *endptr;
    long long val = strtoll(buf, &endptr, 10);
    if (endptr == buf || *endptr != '\0')
        return false;

    *value = static_cast<int64_t>(val);
    return true;
}

int Arm64TopologyDetector::detect_sysfs_cpu_count()
{
    int count = 0;
    int dfd = open("/sys/devices/system/cpu", O_RDONLY | O_DIRECTORY);
    if (dfd < 0)
        return 0;

    DIR *dir = fdopendir(dfd);
    if (!dir) {
        close(dfd);
        return 0;
    }

    while (struct dirent *entry = readdir(dir)) {
        if (strncmp(entry->d_name, "cpu", 3) == 0) {
            char *endptr;
            long cpu = strtol(entry->d_name + 3, &endptr, 10);
            if (endptr != entry->d_name + 3 && *endptr == '\0' && cpu >= 0) {
                if (cpu > count)
                    count = static_cast<int>(cpu);
            }
        }
    }
    closedir(dir);
    return count + 1;
}

bool Arm64TopologyDetector::parse_cache_info(int cpu, Topology::Thread *info)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", cpu);
    int cpufd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (cpufd < 0)
        return false;

    for (int level = 1; level <= 3; ++level) {
        char buf[256];
        snprintf(buf, sizeof(buf), "cache/index%d", level);
        int cachefd = openat(cpufd, buf, O_PATH | O_CLOEXEC);
        if (cachefd < 0)
            continue;

        FILE *f = fopenat(cachefd, "level");
        if (!f) {
            close(cachefd);
            continue;
        }
        int detected_level = 0;
        IGNORE_RETVAL(fscanf(f, "%d", &detected_level));
        fclose(f);

        if (detected_level != level) {
            close(cachefd);
            continue;
        }

        int cache_index = level - 1;
        if (cache_index >= 3) {
            close(cachefd);
            continue;
        }

        f = fopenat(cachefd, "size");
        if (!f) {
            close(cachefd);
            continue;
        }
        int size = 0;
        char suffix = '\0';
        IGNORE_RETVAL(fscanf(f, "%d%c", &size, &suffix));
        fclose(f);

        if (suffix == 'K')
            size *= 1024;
        else if (suffix == 'M')
            size *= 1024 * 1024;

        f = fopenat(cachefd, "type");
        if (!f) {
            info->cache[cache_index].cache_data = info->cache[cache_index].cache_instruction = size;
        } else {
            IGNORE_RETVAL(fscanf(f, "%255s", buf));
            fclose(f);

            if (strcmp(buf, "Instruction") == 0)
                info->cache[cache_index].cache_instruction = size;
            else if (strcmp(buf, "Data") == 0)
                info->cache[cache_index].cache_data = size;
            else
                info->cache[cache_index].cache_data = info->cache[cache_index].cache_instruction = size;
        }
        close(cachefd);
    }

    close(cpufd);
    return true;
}

bool Arm64TopologyDetector::parse_cpu_topology_sysfs(int cpu, Topology::Thread *info)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", cpu);
    int cpufd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (cpufd < 0)
        return false;

    bool success = true;

    int package_id = info->package_id;
    if (!read_sysfs_int(cpufd, "topology/physical_package_id", &package_id))
        success = false;
    else
        info->package_id = static_cast<decltype(info->package_id)>(package_id);

    int core_id = info->core_id;
    if (!read_sysfs_int(cpufd, "topology/core_id", &core_id))
        success = false;
    else
        info->core_id = static_cast<decltype(info->core_id)>(core_id);

    int module_id = info->module_id;
    if (!read_sysfs_int(cpufd, "topology/cluster_id", &module_id)) {
        info->module_id = info->core_id;
    } else {
        info->module_id = static_cast<decltype(info->module_id)>(module_id);
    }

    info->thread_id = 0;
    char buf[256];
    if (read_sysfs_file(cpufd, "topology/thread_siblings_list", buf, sizeof(buf))) {
        const char *ptr = buf;
        while (*ptr) {
            char *endptr;
            long val = strtol(ptr, &endptr, 10);
            if (endptr == ptr)
                break;
            if (static_cast<int>(val) == cpu) {
                break;
            }
            ++info->thread_id;
            if (*endptr == ',')
                ptr = endptr + 1;
            else if (*endptr == '-') {
                long end_val = strtol(endptr + 1, &endptr, 10);
                if (end_val > val) {
                    info->thread_id += static_cast<int>(end_val - val);
                    ptr = endptr;
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    } else {
        info->thread_id = 0;
    }

    char midr_buf[64];
    if (read_sysfs_file(cpufd, "regs/identification/midr_el1", midr_buf, sizeof(midr_buf))) {
        char *endptr;
        uint64_t midr = strtoull(midr_buf, &endptr, 16);
        if (endptr != midr_buf) {
            info->native_core_type = static_cast<NativeCoreType>(detect_core_type_from_midr(midr));
        }
    }

    close(cpufd);
    return success;
}

int Arm64TopologyDetector::detect_core_type_from_midr(uint64_t midr)
{
    uint32_t implementer = (midr >> 24) & 0xFF;
    uint32_t part = (midr >> 4) & 0xFFF;

    if (implementer == 0x41) {
        switch (part >> 8) {
        case 0xD:
        case 0xE:
            return core_type_efficiency;
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3:
            return core_type_performance;
        }
    } else if (implementer == 0x42 || implementer == 0x43) {
        return core_type_efficiency;
    }

    return core_type_unknown;
}

bool Arm64TopologyDetector::parse_numa_topology_sysfs()
{
    int dfd = open("/sys/devices/system/node", O_RDONLY | O_DIRECTORY);
    if (dfd < 0)
        return false;

    DIR *dir = fdopendir(dfd);
    if (!dir) {
        close(dfd);
        return false;
    }

    std::string cpulist;
    cpulist.reserve(256);

    while (struct dirent *entry = readdir(dir)) {
        std::string_view name(entry->d_name);
        if (!name.starts_with("node"))
            continue;
        name.remove_prefix(strlen("node"));

        char *endptr;
        long node_id = strtol(name.data(), &endptr, 10);
        if (endptr != name.end())
            continue;

        auto_fd listfd{ openat(dfd, (std::string(entry->d_name) + "/cpulist").c_str(),
                               O_RDONLY | O_CLOEXEC) };
        if (listfd < 0)
            continue;

        cpulist.resize(256);
        while (true) {
            ssize_t n = pread(listfd, &cpulist[0], cpulist.size(), 0);
            if (n <= 0)
                break;

            if (cpulist[n - 1] == '\n') {
                cpulist.resize(n - 1);
                break;
            }
            cpulist.resize(cpulist.size() * 2);
        }

        const char *ptr = cpulist.c_str();
        while (*ptr && device_info) {
            char *endptr;
            long start = strtol(ptr, &endptr, 10);
            if (endptr == ptr)
                break;

            long stop = start;
            if (*endptr == '-') {
                stop = strtol(endptr + 1, &endptr, 10);
            }

            for (int i = 0; i < sApp->thread_count; ++i) {
                if (device_info[i].cpu_number >= start && device_info[i].cpu_number <= stop) {
                    device_info[i].numa_id = static_cast<int>(node_id);
                }
            }

            if (*endptr == ',')
                ptr = endptr + 1;
            else if (*endptr == '\0')
                break;
            else
                ptr = endptr;
        }
    }

    closedir(dir);
    return true;
}

bool Arm64TopologyDetector::detect_via_sysfs()
{
    sysfs_cpu_count = detect_sysfs_cpu_count();
    if (sysfs_cpu_count == 0)
        return false;

    for (int i = 0; i < sApp->thread_count; ++i) {
        int cpu = device_info[i].cpu_number;
        if (!parse_cpu_topology_sysfs(cpu, &device_info[i]))
            return false;

        parse_cache_info(cpu, &device_info[i]);
    }

    parse_numa_topology_sysfs();

    for (int i = 0; i < sApp->thread_count; ++i) {
        if (device_info[i].numa_id < 0)
            device_info[i].numa_id = device_info[i].package_id;
    }

    return true;
}

bool Arm64TopologyDetector::detect_via_acpi_pptt()
{
    if (access("/sys/firmware/acpi/tables/PPTT", F_OK) != 0)
        return false;

    return detect_via_sysfs();
}

bool Arm64TopologyDetector::detect_via_device_tree()
{
    if (access("/proc/device-tree", F_OK) != 0)
        return false;

    return detect_via_sysfs();
}

bool Arm64TopologyDetector::detect_via_proc_cpuinfo()
{
    AutoClosingFile f{ fopen("/proc/cpuinfo", "r") };
    if (!f.f)
        return false;

    char *line = nullptr;
    size_t len = 0;
    ssize_t nread;
    int current_cpu = -1;

    while ((nread = getline(&line, &len, f)) != -1) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;

        char *lineend = strchr(line, '\n');
        if (lineend)
            *lineend = '\0';

        char key[64] = {};
        size_t key_len = colon - line;
        if (key_len >= sizeof(key))
            key_len = sizeof(key) - 1;
        memcpy(key, line, key_len);

        while (*colon == ' ' || *colon == '\t')
            ++colon;

        if (strcmp(key, "processor") == 0) {
            current_cpu = atoi(colon);
            continue;
        }

        if (current_cpu < 0)
            continue;

        Topology::Thread *info = nullptr;
        for (int i = 0; i < sApp->thread_count; ++i) {
            if (device_info[i].cpu_number == current_cpu) {
                info = &device_info[i];
                break;
            }
        }
        if (!info)
            continue;

        if (strcmp(key, "physical id") == 0 || strcmp(key, "package id") == 0) {
            info->package_id = atoi(colon);
        } else if (strcmp(key, "core id") == 0 || strcmp(key, "cpu id") == 0) {
            info->core_id = atoi(colon);
        } else if (strcmp(key, "thread") == 0) {
            info->thread_id = atoi(colon);
        } else if (strcmp(key, "numa node") == 0) {
            info->numa_id = atoi(colon);
        }
    }

    free(line);

    for (int i = 0; i < sApp->thread_count; ++i) {
        if (device_info[i].module_id < 0)
            device_info[i].module_id = device_info[i].core_id;
        if (device_info[i].numa_id < 0)
            device_info[i].numa_id = device_info[i].package_id;
    }

    return true;
}

void Arm64TopologyDetector::detect(const LogicalProcessorSet &enabled_cpus)
{
    assert(sApp->thread_count);
    assert(sApp->thread_count == enabled_cpus.count());
    device_info = sApp->shmem->device_info;

    for (int i = 0; i < sApp->thread_count; ++i) {
        device_info[i].package_id = -1;
        device_info[i].numa_id = -1;
        device_info[i].die_id = -1;
        device_info[i].module_id = -1;
        device_info[i].core_id = -1;
        device_info[i].thread_id = -1;
        device_info[i].hwid = -1;
        device_info[i].microcode = 0;
        device_info[i].native_core_type = core_type_unknown;

        std::fill(std::begin(device_info[i].cache), std::end(device_info[i].cache),
                  cache_info_t{-1, -1});
    }

    int i = 0;
    for (LogicalProcessor lp = enabled_cpus.next(); lp != LogicalProcessor::None; ++i) {
        auto info = device_info + i;
        info->cpu_number = int(lp);
        lp = enabled_cpus.next(LogicalProcessor(int(lp) + 1));
    }
    assert(i == sApp->thread_count);

    if (SandstoneConfig::Debug) {
        if (char *mock_topo = getenv("SANDSTONE_MOCK_TOPOLOGY")) {
            if (create_mock_topology(mock_topo))
                return;
        }
    }

    bool detected = detect_via_acpi_pptt();
    if (!detected) {
        detected = detect_via_sysfs();
        used_source = TopologySource::Sysfs;
    }
    if (!detected) {
        detected = detect_via_device_tree();
        used_source = TopologySource::DeviceTree;
    }
    if (!detected) {
        detected = detect_via_proc_cpuinfo();
        used_source = TopologySource::ProcCpuinfo;
    }

    for (int j = 0; j < sApp->thread_count; ++j) {
        if (device_info[j].module_id < 0)
            device_info[j].module_id = device_info[j].core_id;
        if (device_info[j].numa_id < 0)
            device_info[j].numa_id = device_info[j].package_id;
    }
}

void Arm64TopologyDetector::sort()
{
    std::sort(device_info, device_info + sApp->thread_count, [](const cpu_info_t &cpu1, const cpu_info_t &cpu2) {
        if (cpu1.package_id != cpu2.package_id)
            return cpu1.package_id < cpu2.package_id;
        if (cpu1.core_id != cpu2.core_id)
            return cpu1.core_id < cpu2.core_id;
        return cpu1.cpu_number < cpu2.cpu_number;
    });
}

} // anonymous namespace

void arm64_topology_init(const LogicalProcessorSet &enabled_cpus)
{
    Arm64TopologyDetector detector;
    detector.detect(enabled_cpus);
    detector.sort();
}
