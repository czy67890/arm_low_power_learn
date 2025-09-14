# 文件与作用
|文件|作用|
|u-boot.lds|链接脚本文件,定义各个段的大小与放置位置|
|u-boot.map|可以查看某个文件或者函数链接到了哪个地址|

# 启动流程分析
启动后uboot会开始执行reset
```asm
WEAK(save_boot_params)
#if (IS_ENABLED(CONFIG_BLOBLIST))
	/* Calculate the PC-relative address of saved_args */
	adr	x9, saved_args_offset
	ldr	w10, saved_args_offset
	add	x9, x9, w10, sxtw

	stp	x0, x1, [x9]
	stp	x2, x3, [x9, #16]
#endif
	b	save_boot_params_ret	/* back to my caller */
ENDPROC(save_boot_params)

#if (IS_ENABLED(CONFIG_BLOBLIST))
saved_args_offset:
	.long	saved_args - .	/* offset from current code to save_args */

	.section .data
	.align 2
	.global saved_args
saved_args:
	.rept 4
	.dword 0
	.endr
END(saved_args)
#endif
```

这段代码实现的作用很简单，将x0和x1,x2,x3保存到saved_args变量中，但是实现的很绕，其中ADR是相对寻址，使用相对寻址的原因如下
1. uboot可以装载到任意位置
2. 确保位置动态计算


```asm
#if defined(CONFIG_ARMV8_SPL_EXCEPTION_VECTORS) || !defined(CONFIG_XPL_BUILD)
.macro	set_vbar, regname, reg
	msr	\regname, \reg
.endm
	adr	x0, vectors
#else
.macro	set_vbar, regname, reg
.endm
#endif
	/*
	 * Could be EL3/EL2/EL1, Initial State:
	 * Little Endian, MMU Disabled, i/dCache Disabled
	 */
	switch_el x1, 3f, 2f, 1f
3:	set_vbar vbar_el3, x0
	mrs	x0, scr_el3
	orr	x0, x0, #0xf			/* SCR_EL3.NS|IRQ|FIQ|EA */
	msr	scr_el3, x0
	msr	cptr_el3, xzr			/* Enable FP/SIMD */
	b	0f
2:	mrs	x1, hcr_el2
	tbnz	x1, #HCR_EL2_E2H_BIT, 1f	/* HCR_EL2.E2H */
	orr	x1, x1, #HCR_EL2_AMO_EL2	/* Route SErrors to EL2 */
	msr	hcr_el2, x1
	set_vbar vbar_el2, x0
	mov	x0, #0x33ff
	msr	cptr_el2, x0			/* Enable FP/SIMD */
	b	0f
1:	set_vbar vbar_el1, x0
	mov	x0, #3 << 20
	msr	cpacr_el1, x0			/* Enable FP/SIMD */
0:
	msr	daifclr, #0x4			/* Unmask SError interrupts */

#if CONFIG_COUNTER_FREQUENCY
	branch_if_not_highest_el x0, 4f
	ldr	x0, =CONFIG_COUNTER_FREQUENCY
	msr	cntfrq_el0, x0			/* Initialize CNTFRQ */
#endif

4:	isb

	/*
	 * Enable SMPEN bit for coherency.
	 * This register is not architectural but at the moment
	 * this bit should be set for A53/A57/A72.
	 */
#ifdef CONFIG_ARMV8_SET_SMPEN
	switch_el x1, 3f, 1f, 1f
3:
	mrs     x0, S3_1_c15_c2_1               /* cpuectlr_el1 */
	orr     x0, x0, #0x40
	msr     S3_1_c15_c2_1, x0
	isb
1:
#endif

	/* Apply ARM core specific erratas */
	bl	apply_core_errata

	/*
	 * Cache/BPB/TLB Invalidate
	 * i-cache is invalidated before enabled in icache_enable()
	 * tlb is invalidated before mmu is enabled in dcache_enable()
	 * d-cache is invalidated before enabled in dcache_enable()
	 */

	/* Processor specific initialization */
	bl	lowlevel_init

#if defined(CONFIG_ARMV8_SPIN_TABLE) && !defined(CONFIG_XPL_BUILD)
	branch_if_master x0, master_cpu
	b	spin_table_secondary_jump
	/* never return */
#elif defined(CONFIG_ACPI_PARKING_PROTOCOL) && !defined(CONFIG_SPL_BUILD)
	branch_if_master x0, master_cpu
	/*
	 * Waits for ACPI parking protocol memory to be allocated and the spin-table
	 * code to be written. Once ready the secondary CPUs will jump and spin in
	 * their own 4KiB memory region, which is also used as mailbox, until released
	 * by the OS.
	 * The mechanism is similar to the DT enable-method = "spin-table", but works
	 * with ACPI enabled platforms.
	 */
	b	acpi_pp_secondary_jump
	/* never return */
#elif defined(CONFIG_ARMV8_MULTIENTRY)
	branch_if_master x0, master_cpu

	/*
	 * Slave CPUs
	 */
slave_cpu:
	wfe
	ldr	x1, =CPU_RELEASE_ADDR
	ldr	x0, [x1]
	cbz	x0, slave_cpu
	br	x0			/* branch to the given address */
#endif /* CONFIG_ARMV8_MULTIENTRY */
master_cpu:
	msr	SPSel, #1		/* make sure we use SP_ELx */
	bl	_main

/*-----------------------------------------------------------------------*/

WEAK(apply_core_errata)

	mov	x29, lr			/* Save LR */
	/* For now, we support Cortex-A53, Cortex-A57 specific errata */

	/* Check if we are running on a Cortex-A53 core */
	branch_if_a53_core x0, apply_a53_core_errata

	/* Check if we are running on a Cortex-A57 core */
	branch_if_a57_core x0, apply_a57_core_errata
0:
	mov	lr, x29			/* Restore LR */
	ret

apply_a53_core_errata:

#ifdef CONFIG_ARM_ERRATA_855873
	mrs	x0, midr_el1
	tst	x0, #(0xf << 20)
	b.ne	0b

	mrs	x0, midr_el1
	and	x0, x0, #0xf
	cmp	x0, #3
	b.lt	0b

	mrs	x0, S3_1_c15_c2_0	/* cpuactlr_el1 */
	/* Enable data cache clean as data cache clean/invalidate */
	orr	x0, x0, #1 << 44
	msr	S3_1_c15_c2_0, x0	/* cpuactlr_el1 */
	isb
#endif
	b 0b

apply_a57_core_errata:

#ifdef CONFIG_ARM_ERRATA_828024
	mrs	x0, S3_1_c15_c2_0	/* cpuactlr_el1 */
	/* Disable non-allocate hint of w-b-n-a memory type */
	orr	x0, x0, #1 << 49
	/* Disable write streaming no L1-allocate threshold */
	orr	x0, x0, #3 << 25
	/* Disable write streaming no-allocate threshold */
	orr	x0, x0, #3 << 27
	msr	S3_1_c15_c2_0, x0	/* cpuactlr_el1 */
	isb
#endif

#ifdef CONFIG_ARM_ERRATA_826974
	mrs	x0, S3_1_c15_c2_0	/* cpuactlr_el1 */
	/* Disable speculative load execution ahead of a DMB */
	orr	x0, x0, #1 << 59
	msr	S3_1_c15_c2_0, x0	/* cpuactlr_el1 */
	isb
#endif

#ifdef CONFIG_ARM_ERRATA_833471
	mrs	x0, S3_1_c15_c2_0	/* cpuactlr_el1 */
	/* FPSCR write flush.
	 * Note that in some cases where a flush is unnecessary this
	    could impact performance. */
	orr	x0, x0, #1 << 38
	msr	S3_1_c15_c2_0, x0	/* cpuactlr_el1 */
	isb
#endif

#ifdef CONFIG_ARM_ERRATA_829520
	mrs	x0, S3_1_c15_c2_0	/* cpuactlr_el1 */
	/* Disable Indirect Predictor bit will prevent this erratum
	    from occurring
	 * Note that in some cases where a flush is unnecessary this
	    could impact performance. */
	orr	x0, x0, #1 << 4
	msr	S3_1_c15_c2_0, x0	/* cpuactlr_el1 */
	isb
#endif

#ifdef CONFIG_ARM_ERRATA_833069
	mrs	x0, S3_1_c15_c2_0	/* cpuactlr_el1 */
	/* Disable Enable Invalidates of BTB bit */
	and	x0, x0, #0xE
	msr	S3_1_c15_c2_0, x0	/* cpuactlr_el1 */
	isb
#endif
	b 0b
ENDPROC(apply_core_errata)

/*-----------------------------------------------------------------------*/

WEAK(lowlevel_init)
	mov	x29, lr			/* Save LR */

#if defined(CONFIG_GICV2) || defined(CONFIG_GICV3)
	branch_if_slave x0, 1f
	ldr	x0, =GICD_BASE
	bl	gic_init_secure
1:
#if defined(CONFIG_GICV3)
	ldr	x0, =GICR_BASE
	bl	gic_init_secure_percpu
#elif defined(CONFIG_GICV2)
	ldr	x0, =GICD_BASE
	ldr	x1, =GICC_BASE
	bl	gic_init_secure_percpu
#endif
#endif

#ifdef CONFIG_ARMV8_MULTIENTRY
	branch_if_master x0, 2f

	/*
	 * Slave should wait for master clearing spin table.
	 * This sync prevent salves observing incorrect
	 * value of spin table and jumping to wrong place.
	 */
#if defined(CONFIG_GICV2) || defined(CONFIG_GICV3)
#ifdef CONFIG_GICV2
	ldr	x0, =GICC_BASE
#endif
	bl	gic_wait_for_interrupt
#endif

	/*
	 * All slaves will enter EL2 and optionally EL1.
	 */
#if defined(CONFIG_ARMV8_PSCI) && !defined(CONFIG_XPL_BUILD)
        bl      psci_setup_vectors
#endif
	adr	x4, lowlevel_in_el2
	ldr	x5, =ES_TO_AARCH64
	bl	armv8_switch_to_el2

lowlevel_in_el2:
#ifdef CONFIG_ARMV8_SWITCH_TO_EL1
	adr	x4, lowlevel_in_el1
	ldr	x5, =ES_TO_AARCH64
	bl	armv8_switch_to_el1

lowlevel_in_el1:
#endif

#endif /* CONFIG_ARMV8_MULTIENTRY */

2:
	mov	lr, x29			/* Restore LR */
	ret
ENDPROC(lowlevel_init)

WEAK(smp_kick_all_cpus)
	/* Kick secondary cpus up by SGI 0 interrupt */
#if defined(CONFIG_GICV2) || defined(CONFIG_GICV3)
	ldr	x0, =GICD_BASE
	b	gic_kick_secondary_cpus
#endif
	ret
ENDPROC(smp_kick_all_cpus)

/*-----------------------------------------------------------------------*/

ENTRY(c_runtime_cpu_setup)
#if defined(CONFIG_ARMV8_SPL_EXCEPTION_VECTORS) || !defined(CONFIG_XPL_BUILD)
	/* Relocate vBAR */
	adr	x0, vectors
	switch_el x1, 3f, 2f, 1f
3:	msr	vbar_el3, x0
	b	0f
2:	msr	vbar_el2, x0
	b	0f
1:	msr	vbar_el1, x0
0:
#endif

	ret
ENDPROC(c_runtime_cpu_setup)

WEAK(save_boot_params)
#if (IS_ENABLED(CONFIG_BLOBLIST))
	/* Calculate the PC-relative address of saved_args */
	adr	x9, saved_args_offset
	ldr	w10, saved_args_offset
	add	x9, x9, w10, sxtw

	stp	x0, x1, [x9]
	stp	x2, x3, [x9, #16]
#endif
	b	save_boot_params_ret	/* back to my caller */
ENDPROC(save_boot_params)
```

这里会根据EL等级选择不同的处理逻辑，将异常向量表进行加载到不同的寄存器VBAR_ELX(Vector Base Address Register)。vectors的定义如下
```asm
vectors:
	.align	7		/* Current EL Synchronous Thread */
	stp	x29, x30, [sp, #-16]!
	bl	_exception_entry
	bl	do_bad_sync
	b	exception_exit

_exception_entry:
	stp	x27, x28, [sp, #-16]!
	stp	x25, x26, [sp, #-16]!
	stp	x23, x24, [sp, #-16]!
	stp	x21, x22, [sp, #-16]!
	stp	x19, x20, [sp, #-16]!
	stp	x17, x18, [sp, #-16]!
	stp	x15, x16, [sp, #-16]!
	stp	x13, x14, [sp, #-16]!
	stp	x11, x12, [sp, #-16]!
	stp	x9, x10, [sp, #-16]!
	stp	x7, x8, [sp, #-16]!
	stp	x5, x6, [sp, #-16]!
	stp	x3, x4, [sp, #-16]!
	stp	x1, x2, [sp, #-16]!
	b	_save_el_regs			/* jump to the second part */

	.align	7		/* Current EL IRQ Thread */
	stp	x29, x30, [sp, #-16]!
	bl	_exception_entry
	bl	do_bad_irq
	b	exception_exit
```
实际上uboot的异常处理仅仅完成了bad_irq的处理，在打印寄存器的值之后，将调用panic_finish完成reset,代码如下
```c
static void panic_finish(void)
{
	putc('\n');
#if defined(CONFIG_PANIC_HANG)
	hang();
#else
	flush();  /* flush the panic message before reset */

	do_reset(NULL, 0, 0, NULL);
#endif
	while (1)
		;
}
```


apply_core_errata可以处理一些处理架构相关的错误代码，不在本文分析

lowlevel_init与master_cpu的处理，slave cpu将会执行wfe或者spin wait。master CPU使用msr	SPSel, #1确保处于EL1模式，并且跳转到_main,_main的定义如下
```asm
ENTRY(_main)

/*
 * Set up initial C runtime environment and call board_init_f(0).
 */
#if CONFIG_IS_ENABLED(HAVE_INIT_STACK)
	ldr	x0, =CONFIG_VAL(STACK)
#elif defined(CONFIG_INIT_SP_RELATIVE)
#if CONFIG_POSITION_INDEPENDENT
	adrp	x0, __bss_start     /* x0 <- Runtime &__bss_start */
	add	x0, x0, #:lo12:__bss_start
#else
	adr	x0, __bss_start
#endif
	add	x0, x0, #CONFIG_SYS_INIT_SP_BSS_OFFSET
#else
	ldr	x0, =(SYS_INIT_SP_ADDR)
#endif
	bic	sp, x0, #0xf	/* 16-byte alignment for ABI compliance */
	mov	x0, sp
	bl	board_init_f_alloc_reserve
	mov	sp, x0
	/* set up gd here, outside any C code */
	mov	x18, x0
	bl	board_init_f_init_reserve

#if defined(CONFIG_DEBUG_UART) && CONFIG_IS_ENABLED(SERIAL)
	bl	debug_uart_init
#endif

	mov	x0, #0
	bl	board_init_f

#if !defined(CONFIG_XPL_BUILD)
/*
 * Set up intermediate environment (new sp and gd) and call
 * relocate_code(addr_moni). Trick here is that we'll return
 * 'here' but relocated.
 */
	ldr	x0, [x18, #GD_START_ADDR_SP]	/* x0 <- gd->start_addr_sp */
	bic	sp, x0, #0xf	/* 16-byte alignment for ABI compliance */
	ldr	x18, [x18, #GD_NEW_GD]		/* x18 <- gd->new_gd */

	/* Skip relocation in case gd->gd_flags & GD_FLG_SKIP_RELOC */
	ldr	x0, [x18, #GD_FLAGS]		/* x0 <- gd->flags */
	tbnz	x0, 11, relocation_return	/* GD_FLG_SKIP_RELOC is bit 11 */

	adr	lr, relocation_return
#if CONFIG_POSITION_INDEPENDENT
	/* Add in link-vs-runtime offset */
	adrp	x0, _start		/* x0 <- Runtime value of _start */
	add	x0, x0, #:lo12:_start
	ldr	x9, _TEXT_BASE		/* x9 <- Linked value of _start */
	sub	x9, x9, x0		/* x9 <- Run-vs-link offset */
	add	lr, lr, x9
#if defined(CONFIG_ENV_RELOC_GD_ENV_ADDR)
	ldr	x0, [x18, #GD_ENV_ADDR]	/* x0 <- gd->env_addr */
	add	x0, x0, x9
	str	x0, [x18, #GD_ENV_ADDR]
#endif
#endif
	/* Add in link-vs-relocation offset */
	ldr	x9, [x18, #GD_RELOC_OFF]	/* x9 <- gd->reloc_off */
	add	lr, lr, x9	/* new return address after relocation */
	ldr	x0, [x18, #GD_RELOCADDR]	/* x0 <- gd->relocaddr */
	b	relocate_code

relocation_return:

/*
 * Set up final (full) environment
 */
	bl	c_runtime_cpu_setup		/* still call old routine */
#endif /* !CONFIG_XPL_BUILD */
#if !defined(CONFIG_XPL_BUILD) || CONFIG_IS_ENABLED(FRAMEWORK)
#if defined(CONFIG_XPL_BUILD)
	bl	spl_relocate_stack_gd           /* may return NULL */
	/* set up gd here, outside any C code, if new stack is returned */
	cmp	x0, #0
	csel	x18, x0, x18, ne
	/*
	 * Perform 'sp = (x0 != NULL) ? x0 : sp' while working
	 * around the constraint that conditional moves can not
	 * have 'sp' as an operand
	 */
	mov	x1, sp
	cmp	x0, #0
	csel	x0, x0, x1, ne
	mov	sp, x0
#endif

/*
 * Clear BSS section
 */
	ldr	x0, =__bss_start		/* this is auto-relocated! */
	ldr	x1, =__bss_end			/* this is auto-relocated! */
clear_loop:
	str	xzr, [x0], #8
	cmp	x0, x1
	b.lo	clear_loop

	/* call board_init_r(gd_t *id, ulong dest_addr) */
	mov	x0, x18				/* gd_t */
	ldr	x1, [x18, #GD_RELOCADDR]	/* dest_addr */
	b	board_init_r			/* PC relative jump */

	/* NOTREACHED - board_init_r() does not return */
#endif

ENDPROC(_main)
```
board_init_f_alloc_reserve将SP指针对齐到16bit,执行时的寄存器情况如下
|阶段|reg|值|
|-|-|-|
|SP对齐|x0|0x42000000|
