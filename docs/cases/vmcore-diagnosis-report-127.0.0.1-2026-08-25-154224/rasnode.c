// rasnode.c — enumerate per-CPU ARMv8 RAS extension error nodes (ERRIDR/ERX*)
// Read-only forensic sweep: for each CPU, print number of error nodes and
// each node's FR/CTRL/STATUS/ADDR/MISC0/MISC1. Never writes CTLR (no side effects).
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <asm/sysreg.h>

static void read_nodes(void *info)
{
	u64 mpidr = read_cpuid_mpidr();
	u64 erridr;
	int num, i;

	erridr = read_sysreg_s(SYS_ERRIDR_EL1);
	num = (int)(erridr & 0xffff) + 1; /* NUM field = number of nodes - 1 */

	pr_info("rasnode: cpu=%d mpidr=%016llx nodes=%d (ERRIDR=%016llx)\n",
		smp_processor_id(), mpidr, num, erridr);

	for (i = 0; i < num && i < 64; i++) {
		u64 fr, ctlr, status, addr, misc0, misc1;

		write_sysreg_s(i, SYS_ERRSELR_EL1);
		isb();
		fr     = read_sysreg_s(SYS_ERXFR_EL1);
		ctlr   = read_sysreg_s(SYS_ERXCTLR_EL1);
		status = read_sysreg_s(SYS_ERXSTATUS_EL1);
		addr   = read_sysreg_s(SYS_ERXADDR_EL1);
		misc0  = read_sysreg_s(SYS_ERXMISC0_EL1);
		misc1  = read_sysreg_s(SYS_ERXMISC1_EL1);

		pr_info("rasnode: cpu=%d node=%d FR=%016llx CTLR=%016llx STATUS=%016llx ADDR=%016llx MISC0=%016llx MISC1=%016llx\n",
			smp_processor_id(), i, fr, ctlr, status, addr, misc0, misc1);
	}
}

static int __init rasnode_init(void)
{
	pr_info("rasnode: sweeping RAS error nodes on all CPUs\n");
	on_each_cpu(read_nodes, NULL, 1);
	return 0;
}

static void __exit rasnode_exit(void) {}

module_init(rasnode_init);
module_exit(rasnode_exit);
MODULE_LICENSE("GPL");
