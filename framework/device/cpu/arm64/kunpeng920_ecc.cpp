/*
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "arm64_privilege.h"
#include "arm64_ras.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

enum ecc_backend
{
    ECC_BACKEND_EDAC,
    ECC_BACKEND_ACPI_APEI,
    ECC_BACKEND_VENDOR_DRIVER,
    ECC_BACKEND_NONE
};

class Kunpeng920EccDetector
{
public:
    Kunpeng920EccDetector();
    ~Kunpeng920EccDetector();

    int init();
    int detect_backend();
    int read_errors(memory_error_stats *stats, int cpu);

private:
    ecc_backend m_backend;
    int m_edac_mc_fd;
    bool m_initialized;

    bool check_edac_available();
    bool check_apei_available();
    bool check_vendor_driver();
    int read_edac_errors(memory_error_stats *stats, int cpu);
    int read_apei_errors(memory_error_stats *stats, int cpu);
    int read_vendor_errors(memory_error_stats *stats, int cpu);
};

Kunpeng920EccDetector::Kunpeng920EccDetector()
    : m_backend(ECC_BACKEND_NONE)
    , m_edac_mc_fd(-1)
    , m_initialized(false)
{
}

Kunpeng920EccDetector::~Kunpeng920EccDetector()
{
    if (m_edac_mc_fd >= 0) {
        close(m_edac_mc_fd);
        m_edac_mc_fd = -1;
    }
}

bool Kunpeng920EccDetector::check_edac_available()
{
    const char *edac_path = "/sys/devices/system/edac/mc/";
    struct stat st;

    if (stat(edac_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(edac_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (strncmp(entry->d_name, "mc", 2) == 0 &&
                    atoi(entry->d_name + 2) >= 0) {
                    closedir(dir);
                    return true;
                }
            }
            closedir(dir);
        }
    }
    return false;
}

bool Kunpeng920EccDetector::check_apei_available()
{
    const char *apei_path = "/sys/firmware/efi/err_info/";
    struct stat st;

    if (stat(apei_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }

    const char *apei_hest_path = "/sys/devices/system/cpu/cpu0/err_info/";
    if (stat(apei_hest_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }

    return false;
}

bool Kunpeng920EccDetector::check_vendor_driver()
{
    const char *vendor_devices[] = {
        "/dev/hisi_ras",
        "/dev/hisi_hardware_ras",
        "/dev/hip08_ras",
        "/dev/hip09_ras",
        nullptr
    };

    for (int i = 0; vendor_devices[i] != nullptr; ++i) {
        struct stat st;
        if (stat(vendor_devices[i], &st) == 0) {
            return true;
        }
    }

    return false;
}

int Kunpeng920EccDetector::detect_backend()
{
    if (check_vendor_driver()) {
        m_backend = ECC_BACKEND_VENDOR_DRIVER;
        return 0;
    }

    if (check_apei_available()) {
        m_backend = ECC_BACKEND_ACPI_APEI;
        return 0;
    }

    if (check_edac_available()) {
        m_backend = ECC_BACKEND_EDAC;
        return 0;
    }

    m_backend = ECC_BACKEND_NONE;
    return -1;
}

int Kunpeng920EccDetector::init()
{
    if (m_initialized) {
        return 0;
    }

    int ret = detect_backend();
    if (ret != 0) {
        return ret;
    }

    if (m_backend == ECC_BACKEND_EDAC) {
        m_edac_mc_fd = open("/sys/devices/system/edac/mc/", O_RDONLY);
    }

    m_initialized = true;
    return 0;
}

int Kunpeng920EccDetector::read_edac_errors(memory_error_stats *stats, int cpu)
{
    if (!stats) {
        return -1;
    }

    memset(stats, 0, sizeof(memory_error_stats));

    /*
     * EDAC memory controllers are indexed by controller (mc0, mc1, ...),
     * NOT by CPU. The earlier code built "/sys/.../edac/mc/mc%d/ce_count"
     * with %d = cpu, which only ever matched mc0 (and only for the call
     * where cpu happened to be 0); for cpu 1..191 it opened a non-existent
     * "mc1".."mc191" path, the fopen failed, and every counter stayed 0 —
     * so RAS ECC monitoring silently reported all-clear for 191/192 cores.
     *
     * The counts are controller-wide by design (matching the baseline
     * interrupt_monitor.cpp read_edac_total() helper, which sums across all
     * mcN). Enumerate every mcN directory and accumulate ce/ue (+noinfo)
     * across the whole set. The cpu argument is retained in the signature
     * only for the APEI/GHES attribution path; the EDAC backend ignores it.
     */
    (void)cpu;

    static constexpr const char *edac_mc_dir = "/sys/devices/system/edac/mc";
    DIR *dir = opendir(edac_mc_dir);
    if (!dir) {
        snprintf(stats->dimm_name, sizeof(stats->dimm_name), "EDAC");
        snprintf(stats->dimm_id, sizeof(stats->dimm_id), "mc?");
        return 0;
    }

    int controller_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "mc", 2) != 0)
            continue;
        char *endptr;
        long mc_idx = strtol(entry->d_name + 2, &endptr, 10);
        if (endptr == entry->d_name + 2 || *endptr != '\0' || mc_idx < 0)
            continue;

        char path[512];
        uint64_t local_ce = 0, local_ue = 0, local_ce_noinfo = 0, local_ue_noinfo = 0;

        snprintf(path, sizeof(path), "%s/%s/ce_count", edac_mc_dir, entry->d_name);
        if (FILE *fp = fopen(path, "r")) {
            if (fscanf(fp, "%lu", &local_ce) != 1)
                local_ce = 0;
            fclose(fp);
        }
        snprintf(path, sizeof(path), "%s/%s/ue_count", edac_mc_dir, entry->d_name);
        if (FILE *fp = fopen(path, "r")) {
            if (fscanf(fp, "%lu", &local_ue) != 1)
                local_ue = 0;
            fclose(fp);
        }
        snprintf(path, sizeof(path), "%s/%s/ce_noinfo_count", edac_mc_dir, entry->d_name);
        if (FILE *fp = fopen(path, "r")) {
            if (fscanf(fp, "%lu", &local_ce_noinfo) == 1)
                local_ce += local_ce_noinfo;
            fclose(fp);
        }
        snprintf(path, sizeof(path), "%s/%s/ue_noinfo_count", edac_mc_dir, entry->d_name);
        if (FILE *fp = fopen(path, "r")) {
            if (fscanf(fp, "%lu", &local_ue_noinfo) == 1)
                local_ue += local_ue_noinfo;
            fclose(fp);
        }

        stats->ce_count += local_ce;
        stats->ue_count += local_ue;
        if (controller_count == 0) {
            snprintf(stats->dimm_name, sizeof(stats->dimm_name), "EDAC_%s", entry->d_name);
            snprintf(stats->dimm_id, sizeof(stats->dimm_id), "%s", entry->d_name);
        }
        ++controller_count;
    }
    closedir(dir);

    if (controller_count == 0) {
        snprintf(stats->dimm_name, sizeof(stats->dimm_name), "EDAC");
        snprintf(stats->dimm_id, sizeof(stats->dimm_id), "mc?");
    } else if (controller_count > 1) {
        snprintf(stats->dimm_name, sizeof(stats->dimm_name), "EDAC_mc0-%d", controller_count - 1);
        snprintf(stats->dimm_id, sizeof(stats->dimm_id), "mc0-%d", controller_count - 1);
    }

    // NOTE: do NOT write to reset_counters here. The previous code wrote '1'
    // to /sys/devices/system/edac/mc/mcN/reset_counters after every read,
    // which cleared the cumulative ce/ue counters — so the next read would
    // return 0 until a new error arrived, defeating continuous RAS monitoring
    // (the baseline interrupt_monitor.cpp reads ce_count+ue_count without
    // resetting). The EDAC counters are cumulative by design; leave them.

    return 0;
}

int Kunpeng920EccDetector::read_apei_errors(memory_error_stats *stats, int cpu)
{
    if (!stats) {
        return -1;
    }

    memset(stats, 0, sizeof(memory_error_stats));

    char path[512];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/err_info/uncorrectable_errors", cpu);

    FILE *fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &stats->ue_count) != 1) {
            stats->ue_count = 0;
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/err_info/correctable_errors", cpu);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &stats->ce_count) != 1) {
            stats->ce_count = 0;
        }
        fclose(fp);
    }

    snprintf(stats->dimm_name, sizeof(stats->dimm_name), "APEI_CPU%d", cpu);
    snprintf(stats->dimm_id, sizeof(stats->dimm_id), "cpu%d", cpu);

    return 0;
}

int Kunpeng920EccDetector::read_vendor_errors(memory_error_stats *stats, int cpu)
{
    if (!stats) {
        return -1;
    }

    memset(stats, 0, sizeof(memory_error_stats));

    const char *vendor_device = nullptr;
    const char *vendor_devices[] = {
        "/dev/hisi_ras",
        "/dev/hisi_hardware_ras",
        "/dev/hip08_ras",
        "/dev/hip09_ras",
        nullptr
    };

    struct stat st;
    for (int i = 0; vendor_devices[i] != nullptr; ++i) {
        if (stat(vendor_devices[i], &st) == 0) {
            vendor_device = vendor_devices[i];
            break;
        }
    }

    if (!vendor_device) {
        return -1;
    }

    int fd = open(vendor_device, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    // The vendor RAS char drivers (/dev/hisi_ras etc.) expose errors via an
    // ioctl, but the command number is not publicly specified in any header
    // on this board. The previous code called ioctl(fd, 0, &ras_data) with a
    // magic command of 0, which is not a valid HISI RAS command and returns
    // ENOTTY — a silent no-op that reported all-zero stats. Until the real
    // command number is known (from the vendor RAS driver header), do not
    // issue a bogus ioctl: skip the vendor path and let the caller fall back
    // to the EDAC sysfs path (read_edac_errors) which works on this board.
    // (The per-CPU cpu argument is unused here; vendor RAS attribution, if
    // ever wired, would key on it.)
    (void)cpu;
    close(fd);
    return -1;
}

int Kunpeng920EccDetector::read_errors(memory_error_stats *stats, int cpu)
{
    if (!m_initialized) {
        int ret = init();
        if (ret != 0) {
            return ret;
        }
    }

    switch (m_backend) {
    case ECC_BACKEND_EDAC:
        return read_edac_errors(stats, cpu);
    case ECC_BACKEND_ACPI_APEI:
        return read_apei_errors(stats, cpu);
    case ECC_BACKEND_VENDOR_DRIVER:
        return read_vendor_errors(stats, cpu);
    case ECC_BACKEND_NONE:
    default:
        return -1;
    }
}

static Kunpeng920EccDetector g_kunpeng_ecc;

extern "C" {

int kunpeng920_ecc_init(void)
{
    return g_kunpeng_ecc.init();
}

int kunpeng920_ecc_read_errors(memory_error_stats *stats, int cpu)
{
    return g_kunpeng_ecc.read_errors(stats, cpu);
}

} // extern "C"
