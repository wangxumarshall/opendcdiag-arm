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

    char path[512];
    snprintf(path, sizeof(path), "/sys/devices/system/edac/mc/mc%d/ce_count", cpu);

    FILE *fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &stats->ce_count) != 1) {
            stats->ce_count = 0;
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "/sys/devices/system/edac/mc/mc%d/ue_count", cpu);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%lu", &stats->ue_count) != 1) {
            stats->ue_count = 0;
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "/sys/devices/system/edac/mc/mc%d/ce_noinfo_count", cpu);
    fp = fopen(path, "r");
    if (fp) {
        uint64_t ce_noinfo = 0;
        if (fscanf(fp, "%lu", &ce_noinfo) == 1) {
            stats->ce_count += ce_noinfo;
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "/sys/devices/system/edac/mc/mc%d/ue_noinfo_count", cpu);
    fp = fopen(path, "r");
    if (fp) {
        uint64_t ue_noinfo = 0;
        if (fscanf(fp, "%lu", &ue_noinfo) == 1) {
            stats->ue_count += ue_noinfo;
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "/sys/devices/system/edac/mc/mc%d/reset_counters", cpu);
    fp = fopen(path, "w");
    if (fp) {
        fputc('1', fp);
        fclose(fp);
    }

    snprintf(stats->dimm_name, sizeof(stats->dimm_name), "EDAC_MC%d", cpu);
    snprintf(stats->dimm_id, sizeof(stats->dimm_id), "mc%d", cpu);

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

    struct {
        uint32_t cpu;
        uint32_t type;
        uint64_t ce_count;
        uint64_t ue_count;
    } ras_data = {};

    ras_data.cpu = static_cast<uint32_t>(cpu);

    ssize_t ret = ioctl(fd, 0, &ras_data);
    if (ret >= 0) {
        stats->ce_count = ras_data.ce_count;
        stats->ue_count = ras_data.ue_count;
    }

    close(fd);

    snprintf(stats->dimm_name, sizeof(stats->dimm_name), "VendorRAS");
    snprintf(stats->dimm_id, sizeof(stats->dimm_id), "hisi_ras");

    return 0;
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
