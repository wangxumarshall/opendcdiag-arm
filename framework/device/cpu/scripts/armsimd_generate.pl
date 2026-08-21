#!/usr/bin/env perl

# Copyright 2026 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0

# Generate cpu_features.h for AArch64 from simd-arm.conf.
#
# Emits an ARM64-NATIVE symbol contract (decoupled from x86's, so an
# x86-side rename can neither drop this arch's features nor collide):
# device_features_t, CPU_FEATURE_CONSTANT, cpu_feature_* / cpu_* macros,
# features_string, features_indices, arm64_locators (count-only matters
# for to_string), and Arm64Architecture/arm64_architectures.  The locator
# carries {HWCAP set, bit} pairs instead of CPUID leaf+bit.  Consumers
# (cpu_device.cpp, cpuid_internal.h) select this contract via
# #if defined(__aarch64__) and the x86 contract via #else, so the two
# arches never share a symbol name.

use strict;
$\ = "\n";
$/ = "\n";

# Read input from the file specified as the first argument, output to second.
my $input_conf_file = shift @ARGV;
my $output_file = shift @ARGV // "";
open(my $fh, '<', $input_conf_file) or die $!;

# header guard derived from the output path (like the x86 generator).
my $headerguard = "";
if ($output_file ne "") {
    $headerguard = uc($output_file);
    $headerguard =~ s/[^A-Z0-9_]/_/g;
}

my @features;          # list of feature hashes
my %feature_ids;       # name -> index
my @architecture_names;
my %architectures;
my $maxarchnamelen = 0;

while (<$fh>) {
    chomp;
    my $orig = $_;
    s/\s*#.*$//;          # strip comments
    s/^\s+//;
    s/\s+$//;
    next if $_ eq "";

    if (/^arch=\s*(.*)/) {
        my $rest = $1;
        my ($arch, $based, $featlist) = split(/\s+/, $rest, 3);
        $based //= "<>";
        $featlist //= "";
        my @flist = grep { $_ ne "" } split(/\s*,\s*/, $featlist);
        die("Unknown base architecture \"$based\" for $arch\n")
            unless $based eq "<>" or grep { $_ eq $based } @architecture_names;

        my $id = lc($arch);
        $id =~ s/[^A-Za-z0-9_]/_/g;

        my $prettyname = $arch;
        $prettyname =~ s/_/ /g;
        $maxarchnamelen = length($prettyname) if length($prettyname) > $maxarchnamelen;

        my @basefeatures = ();
        @basefeatures = @{$architectures{$based}->{allfeatures}} if $based ne "<>";
        my @allfeatures = sort { $feature_ids{$a} <=> $feature_ids{$b} } (@basefeatures, @flist);

        $architectures{$arch} = {
            name        => $prettyname,
            id          => $id,
            base        => $based,
            features    => [@flist],
            allfeatures => \@allfeatures,
        };
        push @architecture_names, $arch
            unless grep { $_ eq $arch } @architecture_names;
        next;
    }

    # feature line: <name> <source> <bit> [required-feature]
    my @cols = split;
    next unless @cols >= 3;
    my ($name, $source, $bit, $depend) = @cols;
    $depend //= "";
    $depend =~ s/,.*//;    # only first dependency matters for the header

    my $id = $name;
    $id =~ s/[^A-Za-z0-9_]/_/g;
    push @features, {
        name   => $name,
        id     => uc($id),
        source => $source,     # HWCAP or HWCAP2
        bit    => int($bit),
        depend => $depend,
    };
    $feature_ids{$name} = $#features;
}
close($fh);

# Emit output




print qq|// This is a generated file. DO NOT EDIT.
// Please see $0
#ifndef $headerguard
#define $headerguard

#include <stdint.h>

typedef unsigned __int128 device_features_t;
#define CPU_FEATURE_CONSTANT(bit) (((device_features_t) 1) << (bit))|;

my $lastsource = "";
for (my $i = 0; $i < scalar @features; ++$i) {
    my $f = $features[$i];
    print "\n// in $f->{source}:" if $f->{source} ne $lastsource;
    $lastsource = $f->{source};
    printf  "#define cpu_feature_%-31s (CPU_FEATURE_CONSTANT(%d))\n", lc($f->{id}), $i;
}

# Architecture macros (inherited like x86's)
print "\n// ARM architectures";
for (@architecture_names) {
    my $arch = $architectures{$_};
    my $base = $arch->{base};
    $base = ($base eq "<>") ? "0" : "cpu_" . lc($base);
    $base =~ s/[^A-Za-z0-9_]/_/g;
    printf  "#define cpu_%-19s (%s", lc($arch->{id}), $base;
    for my $f (@{$arch->{features}}) {
        my @m = grep { $_->{name} eq $f } @features;
        if (scalar @m == 1) {
            printf  " \\\n%33s| cpu_feature_%s", " ", lc($m[0]->{id});
        } else {
            warn "unknown feature '$f' for arch '$arch->{name}'\n";
        }
    }
    print ")";
}

# device_compiler_features: map compiler __ARM_FEATURE_* macros to features
print q|

static const device_features_t device_compiler_features = 0|;

# Mapping of feature name -> compiler macro that indicates it at compile time.
my %compiler_macro = (
    fp        => '__ARM_FEATURE_FP',
    asimd     => '__ARM_NEON',
    aes       => '__ARM_FEATURE_AES',
    sha1      => '__ARM_FEATURE_CRYPTO',
    sha2      => '__ARM_FEATURE_SHA2',
    sha3      => '__ARM_FEATURE_SHA3',
    sm3       => '__ARM_FEATURE_SM3',
    sm4       => '__ARM_FEATURE_SM4',
    crc32     => '__ARM_FEATURE_CRC32',
    atomics   => '__ARM_FEATURE_ATOMICS',
    fphp      => '__ARM_FEATURE_FP16_SCALAR_ARITHMETIC',
    asimdhp   => '__ARM_FEATURE_FP16_VECTOR_ARITHMETIC',
    asimdrdm  => '__ARM_FEATURE_QRDMX',
    jscvt     => '__ARM_FEATURE_JCVT',
    fcma      => '__ARM_FEATURE_FCMA',
    lrcpc     => '__ARM_FEATURE_RCPC',
    dcpop     => '__ARM_FEATURE_DCPOP',
    asimddp   => '__ARM_FEATURE_DOTPROD',
    sha512    => '__ARM_FEATURE_SHA512',
    sve       => '__ARM_FEATURE_SVE',
    asimdfhm  => '__ARM_FEATURE_FP16_FML',
    flagm     => '__ARM_FEATURE_FLAGM',
    ssbs      => '__ARM_FEATURE_SSBS',
    sve2      => '__ARM_FEATURE_SVE2',
    bf16      => '__ARM_FEATURE_BF16',
    i8mm      => '__ARM_FEATURE_I8MM',
    rng       => '__ARM_FEATURE_RNG',
    bti       => '__ARM_FEATURE_BTI',
    mte       => '__ARM_FEATURE_MTE',
    sve2p1    => '__ARM_FEATURE_SVE2P1',
    sme       => '__ARM_FEATURE_SME',
    sme2      => '__ARM_FEATURE_SME2',
    mops      => '__ARM_FEATURE_MOPS',
);

for my $f (@features) {
    my $macro = $compiler_macro{$f->{name}};
    next unless $macro;
    printf  "#ifdef %s\n         | cpu_feature_%s\n#endif\n", $macro, lc($f->{id});
}
print '        ;';

# C++ enums (ARM64-specific names; NOT shared with the x86 generator, so this
# arch's feature symbols are decoupled from x86 and cannot be silently dropped
# by an x86-side rename). Same enum shape, ARM64-native names.
print q|
#if (defined __cplusplus) && __cplusplus >= 201103L
enum Arm64CpuFeatures : device_features_t {|;
for (@features) {
    print "    CpuFeature$_->{id} = cpu_feature_" . lc($_->{id}) . ",";
}
print "}; // enum Arm64CpuFeatures\n";
print "enum Arm64cpuArchitectures : device_features_t {";
for (@architecture_names) {
    my $a = $architectures{$_};
    my $name = $a->{name};
    $name =~ s/[^A-Za-z0-9]//g;
    print "    CpuArch$name = cpu_" . lc($a->{id}) . ",";
}
print "}; // enum Arm64cpuArchitectures";
print "\n#endif /* C++11 */";

# String table + indices (used by device_features_to_string)
print "\n// -- implementation start --";
my $offset = 0;
my @offsets;
print "static const char features_string[] =";
for my $f (@features) {
    printf  "    \" %s\\0\"", $f->{name};
    push @offsets, $offset;
    $offset += 2 + length($f->{name});
}
print "    \"\\0\";";

my $idxtype = $offset > 255 ? "uint16_t" : "uint8_t";
printf  "\nstatic const %s features_indices[] = {", $idxtype;
for (my $j = 0; $j < scalar @offsets; ++$j) {
    printf  "%s%3d,", ($j % 8 ? " " : "\n    "), $offsets[$j];
}
print "\n};";

# Locator table.  Each entry encodes {HWCAP set (0=HWCAP, 1=HWCAP2), bit}.
# We pack it as a struct so cpuid_internal.h's ARM detect_cpu() can decode it;
# cpu_device.cpp's device_features_to_string only relies on array length.
# ARM64-native symbol name (arm64_locators) — decoupled from x86's
# x86_locators so an x86-side change can neither drop this arch's features
# nor collide with them.
print "
// HWCAP set: 0 = AT_HWCAP, 1 = AT_HWCAP2
struct ArmHwcapLocator
{
    uint8_t hwcap_set;
    uint8_t bit;
};

static const struct ArmHwcapLocator arm64_locators[] = {";
for my $f (@features) {
    # NONE source = synthetic, never set (hwcap_set 2 matches no real HWCAP word)
    my $s = ($f->{source} eq 'HWCAP2') ? 1 : ($f->{source} eq 'NONE' ? 2 : 0);
    printf "    { %d, %2d }, // %s\n", $s, $f->{bit}, $f->{name};
}
print "};";

# Architecture table (for dump_device_info) — ARM64-native names.
print qq|
struct Arm64Architecture
{
    device_features_t features;
    char name[$maxarchnamelen + 1];
};

static const struct Arm64Architecture arm64_architectures[] = {|;
my %sorted_archs;
for (@architecture_names) {
    my $arch = $architectures{$_};
    my $key = sprintf "%02d_%s", scalar(@{$arch->{allfeatures}}), join(',', @{$arch->{allfeatures}});
    $sorted_archs{$key} = $arch;
}
for (sort { $b <=> $a } keys %sorted_archs) {
    my $arch = $sorted_archs{$_};
    next if $arch->{base} eq "<>";
    my $id = lc($arch->{id});
    my $feat = "cpu_" . $id;
    printf "    { %s, \"%s\" },\n", $feat, $arch->{name};
}
print "};";

print "\n#endif // $headerguard";

