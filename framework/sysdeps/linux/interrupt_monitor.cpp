/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <interrupt_monitor.hpp>
#include <sandstone_p.h>

#include <dirent.h>
#include <string>

#if defined(__x86_64__)
constexpr const char * const proc_interrupts_file = "/proc/interrupts";

static bool is_kernel_blank(char c)
{
    // kernel only uses spaces for the /proc/interrupts file but we accept tabs
    return c == ' ' || c == '\t';
}

static char *skip_to_non_blank(char *ptr)
{
    char c;
    for ( ; (c = *ptr); ++ptr) {
        if (!is_kernel_blank(c))
            break;
    }
    return ptr;
}

static const std::vector<int> &parse_header(char *line, ssize_t nread)
{
    static struct {
        std::string header_line;
        std::vector<int> result;
    } cache;

    if (nread <= 0) {
        cache = {};
        return cache.result;
    }

    std::string_view header_line(line, nread);
    if (cache.header_line == header_line)
        return cache.result;

    // cache this result for later
    cache.header_line = header_line;

    // read every CPU number to create the mapping
    static const char cpu[] = "CPU";
    char *ptr = skip_to_non_blank(line);
    while (strncmp(ptr, cpu, strlen(cpu)) == 0) {
        char *endptr;
        ptr += strlen(cpu);
        unsigned long n = strtoul(ptr, &endptr, 10);
        if (n == 0 && ptr == endptr)
            break;

        cache.result.push_back(n);
        ptr = skip_to_non_blank(endptr);
    }

    return cache.result;
}

// this function reads the interrupts file and returns a list of integers corresponding
// to the contents of the line that matches the input header prefix, for example "MCE:"
std::vector<uint32_t> InterruptMonitor::get_interrupt_counts(InterruptType type)
{
    static_assert(InterruptMonitorWorks);
    static AutoClosingFile f = { fopen(proc_interrupts_file, "r") };

    const char *hdr = [type] {
        switch (type) {
        case MCE:
            return "MCE:";
        case Thermal:
            return "TRM:";
        }
        assert(false && "Should not have reached here");
        __builtin_unreachable();
        return static_cast<const char *>(nullptr);
    }();

    std::vector<uint32_t> result;
    if (!f)
        return result;

    char *line = nullptr;
    size_t len = 0;
    auto free_line = scopeExit([&] { free(line); });

    // read the header and create the CPU mapping
    ssize_t nread = getline(&line, &len, f);
    const std::vector<int> &cpu_mapping = parse_header(line, nread);
    if (cpu_mapping.size() == 0) {
        // failed to parse the header!
        fclose(f);
        f.f = nullptr;
        return result;
    }

    // static code analyzers: we trust the kernel
    result.resize(cpu_mapping.back() + 1);

    while (getline(&line, &len, f) != -1) {
        // Skip any blanks at the start of the line
        char *ptr = skip_to_non_blank(line);
        if (strncmp(ptr, hdr, strlen(hdr)) != 0)
            continue;

        ptr = ptr + strlen(hdr);
        for (int i = 0; *ptr != '\0'; ++i) {
            char *endptr;
            errno = 0;
            uint64_t n = strtoull(ptr, &endptr, 10);
            if (n == 0 && ptr == endptr)
                break;
            // static code analyzers: we trust the kernel
            result[cpu_mapping[i]] = n;
            ptr = endptr;
        }
        break;
    }

    // reset the file pointer for the next time we get called
    fseek(f, 0, SEEK_SET);
    return result;
}

#elif defined(__aarch64__)
/*
 * On AArch64 there are no x86-style per-CPU MCE/TRM interrupt lines in
 * /proc/interrupts. Hardware error (RAS) reporting is done through the
 * EDAC subsystem, which exposes per-memory-controller correctable (ce)
 * and uncorrectable (ue) error counts under
 *   /sys/devices/system/edac/mc/mcN/ce_count
 *   /sys/devices/system/edac/mc/mcN/ue_count
 * (one directory per memory controller).
 *
 * We aggregate those into a single "machine-check/hardware-error" count
 * (MCE type). The count is controller-wide rather than per-CPU, so we
 * place it at index 0 of the returned vector and leave the rest as 0;
 * mce_check treats any nonzero delta as a hardware error detected
 * during the run, which is the right semantics for RAS events.
 *
 * Thermal interrupt counts are not exposed this way on ARM, so the
 * Thermal type returns an empty vector (count_thermal_events() -> 0).
 */

static uint64_t read_edac_total(const char *counter_name)
{
    constexpr const char *edac_mc_dir = "/sys/devices/system/edac/mc";
    DIR *dir = opendir(edac_mc_dir);
    if (!dir)
        return 0;

    uint64_t total = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != nullptr) {
        if (strncmp(de->d_name, "mc", 2) != 0)
            continue;
        std::string path = std::string(edac_mc_dir) + "/" + de->d_name + "/" + counter_name;
        AutoClosingFile f{ fopen(path.c_str(), "r") };
        if (f) {
            unsigned long long n = 0;
            if (fscanf(f, "%llu", &n) == 1)
                total += n;
        }
    }
    closedir(dir);
    return total;
}

std::vector<uint32_t> InterruptMonitor::get_interrupt_counts(InterruptType type)
{
    static_assert(InterruptMonitorWorks);
    std::vector<uint32_t> result;
    if (type != MCE)
        return result;   // no ARM equivalent for the TRM interrupt line

    uint64_t count = read_edac_total("ce_count") + read_edac_total("ue_count");

    // Size the vector to cover all logical CPUs so callers that index by
    // OS CPU number (mce_check_run) don't go out of bounds. The single
    // controller-wide count lives at index 0.
    int max_cpu = 0;
    for (int i = 0; i < device_count(); ++i)
        max_cpu = std::max(max_cpu, int(device_info[i].cpu_number));
    result.assign(max_cpu + 1, 0);
    result[0] = uint32_t(count);
    return result;
}

#endif // __x86_64__ / __aarch64__
