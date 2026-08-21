/*
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "arm64_privilege.h"
#include "arm64_ras.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#ifdef __aarch64__
#include <arm_acle.h>
#endif

extern "C" {
int kunpeng920_ecc_init(void);
int kunpeng920_ecc_read_errors(memory_error_stats *stats, int cpu);
}

static uint64_t crc64_update_byte(uint64_t crc, uint8_t byte)
{
    constexpr uint64_t polynomial = 0x42F0E1EBA9EA3693ULL;
    crc ^= static_cast<uint64_t>(byte) << 56;
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & (1ULL << 63)) ? ((crc << 1) ^ polynomial) : (crc << 1);
    }
    return crc;
}

enum sdc_detection_method
{
    SDC_METHOD_HW_ECC,
    SDC_METHOD_CRC32,
    SDC_METHOD_CRC64,
    SDC_METHOD_CHECKSUM
};

struct SdcConfig
{
    bool enable_ecc_check;
    bool enable_software_check;
    uint32_t ce_threshold;
    uint32_t ue_threshold;
    bool enable_injection;

    SdcConfig()
        : enable_ecc_check(true)
        , enable_software_check(true)
        , ce_threshold(0)
        , ue_threshold(0)
        , enable_injection(false)
    {
    }
};

class SdcDetector
{
public:
    SdcDetector();
    ~SdcDetector();

    int init();
    int detect(sdc_detection_method method);
    int run_detection(sdc_detection_method method);
    int detect_via_crc32(const void *data, size_t len, uint32_t *crc_out);
    int detect_via_crc64(const void *data, size_t len, uint64_t *crc_out);
    int detect_via_checksum(const void *data, size_t len, uint32_t *checksum_out);
    int detect_via_ecc(memory_error_stats *stats, int cpu);

    void set_config(const SdcConfig &config);
    SdcConfig get_config() const;

private:
    SdcConfig m_config;
    bool m_initialized;
    bool m_ecc_available;
    bool m_crc_available;

    bool check_ecc_available();
    bool check_crc_available();
    int init_ecc();
};

SdcDetector::SdcDetector()
    : m_initialized(false)
    , m_ecc_available(false)
    , m_crc_available(false)
{
}

SdcDetector::~SdcDetector()
{
}

void SdcDetector::set_config(const SdcConfig &config)
{
    m_config = config;
}

SdcConfig SdcDetector::get_config() const
{
    return m_config;
}

bool SdcDetector::check_ecc_available()
{
    return m_ecc_available;
}

bool SdcDetector::check_crc_available()
{
#ifdef __aarch64__
    uint64_t id_aa64pfr0 = 0;
    __asm__ volatile("mrs %0, ID_AA64PFR0_EL1" : "=r"(id_aa64pfr0));
    uint32_t crc_feature = (id_aa64pfr0 >> 4) & 0xF;
    return (crc_feature == 1);
#else
    return false;
#endif
}

int SdcDetector::init_ecc()
{
    return kunpeng920_ecc_init();
}

int SdcDetector::init()
{
    if (m_initialized) {
        return 0;
    }

    if (m_config.enable_ecc_check) {
        int ret = init_ecc();
        if (ret == 0) {
            m_ecc_available = true;
        }
    }

    if (m_config.enable_software_check) {
        m_crc_available = check_crc_available();
    }

    m_initialized = true;
    return 0;
}

int SdcDetector::detect_via_ecc(memory_error_stats *stats, int cpu)
{
    if (!m_ecc_available) {
        return -1;
    }

    if (!stats) {
        return -1;
    }

    return kunpeng920_ecc_read_errors(stats, cpu);
}

int SdcDetector::detect_via_crc32(const void *data, size_t len, uint32_t *crc_out)
{
    if (!data || !crc_out) {
        return -1;
    }

    if (!m_crc_available) {
        return -1;
    }

#ifdef __aarch64__
    const uint64_t *ptr = static_cast<const uint64_t *>(data);
    size_t num_words = len / 8;
    size_t remaining = len % 8;

    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0; i < num_words; ++i) {
        crc = __crc32cd(crc, ptr[i]);
    }

    if (remaining > 0) {
        const uint8_t *byte_ptr = reinterpret_cast<const uint8_t *>(ptr + num_words);
        for (size_t i = 0; i < remaining; ++i) {
            crc = __crc32cb(crc, byte_ptr[i]);
        }
    }

    *crc_out = static_cast<uint32_t>(crc);
    return 0;
#else
    return -1;
#endif
}

int SdcDetector::detect_via_crc64(const void *data, size_t len, uint64_t *crc_out)
{
    if (!data || !crc_out) {
        return -1;
    }

    if (!m_crc_available) {
        return -1;
    }

#ifdef __aarch64__
    const uint64_t *ptr = static_cast<const uint64_t *>(data);
    size_t num_words = len / 8;
    size_t remaining = len % 8;

    uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;

    for (size_t i = 0; i < num_words; ++i) {
        const uint8_t *word_bytes = reinterpret_cast<const uint8_t *>(&ptr[i]);
        for (size_t j = 0; j < sizeof(uint64_t); ++j) {
            crc = crc64_update_byte(crc, word_bytes[j]);
        }
    }

    if (remaining > 0) {
        const uint8_t *byte_ptr = reinterpret_cast<const uint8_t *>(ptr + num_words);
        for (size_t i = 0; i < remaining; ++i) {
            crc = crc64_update_byte(crc, byte_ptr[i]);
        }
    }

    *crc_out = crc;
    return 0;
#else
    return -1;
#endif
}

int SdcDetector::detect_via_checksum(const void *data, size_t len, uint32_t *checksum_out)
{
    if (!data || !checksum_out) {
        return -1;
    }

    const uint32_t *ptr = static_cast<const uint32_t *>(data);
    size_t num_words = len / 4;
    size_t remaining = len % 4;

    uint32_t checksum = 0;

    for (size_t i = 0; i < num_words; ++i) {
        checksum += ptr[i];
    }

    if (remaining > 0) {
        const uint8_t *byte_ptr = reinterpret_cast<const uint8_t *>(ptr + num_words);
        for (size_t i = 0; i < remaining; ++i) {
            checksum += byte_ptr[i];
        }
    }

    *checksum_out = checksum;
    return 0;
}

int SdcDetector::run_detection(sdc_detection_method method)
{
    switch (method) {
    case SDC_METHOD_HW_ECC:
        return m_ecc_available ? 0 : -1;
    case SDC_METHOD_CRC32:
    case SDC_METHOD_CRC64:
        return m_crc_available ? 0 : -1;
    case SDC_METHOD_CHECKSUM:
        return 0;
    default:
        return -1;
    }
}

int SdcDetector::detect(sdc_detection_method method)
{
    if (!m_initialized) {
        int ret = init();
        if (ret != 0) {
            return ret;
        }
    }

    if (method == SDC_METHOD_HW_ECC) {
        if (!m_config.enable_ecc_check) {
            return -1;
        }
        return 0;
    }

    if (method == SDC_METHOD_CRC32 || method == SDC_METHOD_CRC64) {
        if (!m_config.enable_software_check) {
            return -1;
        }
        if (!m_crc_available) {
            return -1;
        }
        return 0;
    }

    if (method == SDC_METHOD_CHECKSUM) {
        if (!m_config.enable_software_check) {
            return -1;
        }
        return 0;
    }

    return -1;
}

static SdcDetector g_sdc_detector;

extern "C" {

int arm64_sdc_init(void)
{
    return g_sdc_detector.init();
}

int arm64_sdc_detect(int method)
{
    return g_sdc_detector.detect(static_cast<sdc_detection_method>(method));
}

int arm64_sdc_detect_via_ecc(memory_error_stats *stats, int cpu)
{
    return g_sdc_detector.detect_via_ecc(stats, cpu);
}

int arm64_sdc_detect_via_crc32(const void *data, size_t len, uint32_t *crc_out)
{
    return g_sdc_detector.detect_via_crc32(data, len, crc_out);
}

int arm64_sdc_detect_via_crc64(const void *data, size_t len, uint64_t *crc_out)
{
    return g_sdc_detector.detect_via_crc64(data, len, crc_out);
}

int arm64_sdc_detect_via_checksum(const void *data, size_t len, uint32_t *checksum_out)
{
    return g_sdc_detector.detect_via_checksum(data, len, checksum_out);
}

void arm64_sdc_set_config(bool enable_ecc, bool enable_software,
                          uint32_t ce_thresh, uint32_t ue_thresh,
                          bool enable_injection)
{
    SdcConfig config;
    config.enable_ecc_check = enable_ecc;
    config.enable_software_check = enable_software;
    config.ce_threshold = ce_thresh;
    config.ue_threshold = ue_thresh;
    config.enable_injection = enable_injection;
    g_sdc_detector.set_config(config);
}

} // extern "C"
