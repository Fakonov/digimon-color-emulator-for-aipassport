	.file	"delay_func.c"
	.option nopic
	.attribute arch, "rv32i2p1_m2p0_c2p0"
	.attribute unaligned_access, 0
	.attribute stack_align, 16
	.text
	.align	1
	.globl	delay_loop
	.type	delay_loop, @function
delay_loop:
	lw	a7,20(a0)
	li	a5,20480
	li	a6,65536
	slli	a7,a7,20
	addi	a5,a5,886
	srli	a7,a7,19
	addi	sp,sp,-16
	mv	t1,a6
	sw	a5,28(a0)
	mv	a1,a6
	add	a7,a0,a7
	li	t3,-65536
	lhu	t2,34(a7)
	lw	a5,24(a0)
	sw	s0,12(sp)
	addi	a6,a6,-769
	addi	t1,t1,-999
	addi	a1,a1,-961
	addi	t3,t3,1
	li	t4,999
	li	t5,64
	j	.L2
.L8:
	ori	a3,a5,320
.L4:
	addi	t0,t2,1
	add	a5,t2,t3
	srli	a2,t0,16
	seqz	a5,a5
	xor	a4,a4,a2
	slli	a5,a5,8
	slli	a2,a2,6
	or	a5,a5,a2
	slli	a4,a4,7
	and	a3,a3,a6
	srli	a2,t0,6
	or	a5,a5,a4
	or	t6,t6,a3
	andi	a4,a2,512
	and	t6,t6,a1
	or	a5,a5,a4
	slli	t2,t0,16
	or	a5,a5,t6
	srli	t2,t2,16
	sw	a5,24(a0)
	sh	t2,34(a7)
.L2:
	srli	a4,t2,15
	add	t0,t2,t1
	seqz	s0,t2
	srli	a3,t0,16
	slli	t6,a4,9
	xori	a2,a4,1
	slli	s0,s0,8
	xor	a2,a2,a3
	and	a5,a5,a6
	or	t6,t6,s0
	srli	t0,t0,6
	or	a5,a5,t6
	slli	a2,a2,7
	andi	t0,t0,512
	or	a2,a2,t0
	and	a5,a5,a1
	or	a5,a2,a5
	slli	a3,a3,6
	slli	a4,a4,16
	or	a3,a3,a5
	srli	a4,a4,16
	andi	a2,a3,320
	beq	t2,t4,.L8
	sw	a3,24(a0)
	bne	a2,t5,.L4
	lw	s0,12(sp)
	li	a5,20480
	addi	a5,a5,895
	sw	t2,16(a0)
	sw	a5,28(a0)
	addi	sp,sp,16
	jr	ra
	.size	delay_loop, .-delay_loop
	.ident	"GCC: (crosstool-NG esp-14.2.0_20251107) 14.2.0"
	.section	.note.GNU-stack,"",@progbits
