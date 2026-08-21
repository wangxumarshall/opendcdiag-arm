/*
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARM64_CPUID_H
#define ARM64_CPUID_H

#include "cpu_features.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

static inline device_features_t arm64_detect_cpu_features_from_hwcap(unsigned long hwcap, unsigned long hwcap2)
{
    const unsigned long registers[] = {
        hwcap,
        hwcap2,
    };
    device_features_t features = 0;

    for (size_t index = 0; index < (sizeof(arm64_locators) / sizeof(arm64_locators[0])); ++index) {
        const struct Arm64HwcapLocator *locator = &arm64_locators[index];
        if (locator->source >= (sizeof(registers) / sizeof(registers[0]))) {
            continue;
        }
        if (locator->bit >= (sizeof(unsigned long) * 8)) {
            continue;
        }
        if ((registers[locator->source] & (1UL << locator->bit)) != 0) {
            features |= CPU_FEATURE_CONSTANT(index);
        }
    }

    return features;
}

static inline bool arm64_has_feature_mask(device_features_t all_features, device_features_t required)
{
    return (all_features & required) == required;
}

#ifdef __linux__
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>

#define KUNPENG920_MIDR    0x480D0100

#ifndef AT_HWCAP2
#define AT_HWCAP2 26
#endif

static inline uint64_t read_midr_from_sysfs(int cpu_id)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1", cpu_id);

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    uint64_t midr = 0;
    if (fscanf(f, "%lx", &midr) != 1)
        midr = 0;

    fclose(f);
    return midr;
}

static inline unsigned long get_hwcap(void)
{
    return getauxval(AT_HWCAP);
}

static inline unsigned long get_hwcap2(void)
{
    return getauxval(AT_HWCAP2);
}

static inline bool is_kunpeng920(void)
{
    int cpu_count = sysconf(_SC_NPROCESSORS_CONF);
    if (cpu_count <= 0)
        cpu_count = 1;

    for (int i = 0; i < cpu_count; ++i) {
        uint64_t midr = read_midr_from_sysfs(i);
        if ((midr & 0xFFFF0000) == KUNPENG920_MIDR)
            return true;
    }
    return false;
}

static inline device_features_t arm64_runtime_features(void)
{
    return arm64_detect_cpu_features_from_hwcap(get_hwcap(), get_hwcap2());
}

#else

static inline uint64_t read_midr_from_sysfs(int cpu_id)
{
    (void)cpu_id;
    return 0;
}

static inline unsigned long get_hwcap(void)
{
    return 0;
}

static inline unsigned long get_hwcap2(void)
{
    return 0;
}

static inline bool is_kunpeng920(void)
{
    return false;
}

static inline device_features_t arm64_runtime_features(void)
{
    return 0;
}

#endif

struct cpuinfo_features {
    bool has_fp;
    bool has_simd;
    bool has_neon;
    bool has_crc32;
    bool has_crypto;
    bool has_sve;
    bool has_sve2;
};

static inline struct cpuinfo_features parse_cpuinfo_features(void)
{
    device_features_t runtime_features = arm64_runtime_features();
    struct cpuinfo_features features = {0};
    features.has_fp = arm64_has_feature_mask(runtime_features, cpu_feature_fp);
    features.has_simd = arm64_has_feature_mask(runtime_features, cpu_feature_asimd);
    features.has_neon = arm64_has_feature_mask(runtime_features, cpu_feature_asimd);
    features.has_crc32 = arm64_has_feature_mask(runtime_features, cpu_feature_crc32);
    features.has_crypto = arm64_has_feature_mask(
        runtime_features,
        cpu_feature_aes | cpu_feature_pmull | cpu_feature_sha1 | cpu_feature_sha2
    );
    features.has_sve = arm64_has_feature_mask(runtime_features, cpu_feature_sve);
    features.has_sve2 = arm64_has_feature_mask(runtime_features, cpu_feature_sve2);
    return features;
}

static inline bool arm64_has_neon(void)
{
    struct cpuinfo_features f = parse_cpuinfo_features();
    return f.has_neon;
}

static inline bool arm64_has_crc32(void)
{
    struct cpuinfo_features f = parse_cpuinfo_features();
    return f.has_crc32;
}

static inline bool arm64_has_crypto(void)
{
    struct cpuinfo_features f = parse_cpuinfo_features();
    return f.has_crypto;
}

static inline bool arm64_has_fp(void)
{
    struct cpuinfo_features f = parse_cpuinfo_features();
    return f.has_fp;
}

static inline bool arm64_has_simd(void)
{
    struct cpuinfo_features f = parse_cpuinfo_features();
    return f.has_simd;
}

static inline bool arm64_has_sve(void)
{
    struct cpuinfo_features f = parse_cpuinfo_features();
    return f.has_sve;
}

static inline bool arm64_has_sve2(void)
{
    struct cpuinfo_features f = parse_cpuinfo_features();
    return f.has_sve2;
}

#endif
