// Extracted SHA-NI (Intel SHA Extensions) variant of sha256_block_data_order
// from BoringSSL gen/bcm/sha256-x86_64-linux.S (BSD-3-Clause / OpenSSL dual).
// Generated upstream by sha256-x86_64.pl; do not edit the function body by
// hand. Other variants (nohw / ssse3 / avx) intentionally dropped; the
// mbedtls path in signature_verify.cpp covers CPUs without SHA-NI.

// Minimal CET stub so we do not need <openssl/asm_base.h>. This file is
// not built with -fcf-protection, so the endbr64 markers are no-ops.
#define _CET_ENDBR

#if defined(__x86_64__) && defined(__ELF__)
.text

.globl	sha256_block_data_order_hw
.hidden sha256_block_data_order_hw
.type	sha256_block_data_order_hw,@function
.align	64
sha256_block_data_order_hw:
.cfi_startproc	
_CET_ENDBR
	leaq	K256+128(%rip),%rcx
	movdqu	(%rdi),%xmm1
	movdqu	16(%rdi),%xmm2
	movdqa	512-128(%rcx),%xmm7

	pshufd	$0x1b,%xmm1,%xmm0
	pshufd	$0xb1,%xmm1,%xmm1
	pshufd	$0x1b,%xmm2,%xmm2
	movdqa	%xmm7,%xmm8
	palignr	$8,%xmm2,%xmm1
	punpcklqdq	%xmm0,%xmm2
	jmp	.Loop_shaext

.align	16
.Loop_shaext:
	movdqu	(%rsi),%xmm3
	movdqu	16(%rsi),%xmm4
	movdqu	32(%rsi),%xmm5
	pshufb	%xmm7,%xmm3
	movdqu	48(%rsi),%xmm6

	movdqa	0-128(%rcx),%xmm0
	paddd	%xmm3,%xmm0
	pshufb	%xmm7,%xmm4
	movdqa	%xmm2,%xmm10
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	nop
	movdqa	%xmm1,%xmm9
	sha256rnds2	%xmm2,%xmm1

	movdqa	32-128(%rcx),%xmm0
	paddd	%xmm4,%xmm0
	pshufb	%xmm7,%xmm5
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	leaq	64(%rsi),%rsi
	sha256msg1	%xmm4,%xmm3
	sha256rnds2	%xmm2,%xmm1

	movdqa	64-128(%rcx),%xmm0
	paddd	%xmm5,%xmm0
	pshufb	%xmm7,%xmm6
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm6,%xmm7
	palignr	$4,%xmm5,%xmm7
	nop
	paddd	%xmm7,%xmm3
	sha256msg1	%xmm5,%xmm4
	sha256rnds2	%xmm2,%xmm1

	movdqa	96-128(%rcx),%xmm0
	paddd	%xmm6,%xmm0
	sha256msg2	%xmm6,%xmm3
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm3,%xmm7
	palignr	$4,%xmm6,%xmm7
	nop
	paddd	%xmm7,%xmm4
	sha256msg1	%xmm6,%xmm5
	sha256rnds2	%xmm2,%xmm1
	movdqa	128-128(%rcx),%xmm0
	paddd	%xmm3,%xmm0
	sha256msg2	%xmm3,%xmm4
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm4,%xmm7
	palignr	$4,%xmm3,%xmm7
	nop
	paddd	%xmm7,%xmm5
	sha256msg1	%xmm3,%xmm6
	sha256rnds2	%xmm2,%xmm1
	movdqa	160-128(%rcx),%xmm0
	paddd	%xmm4,%xmm0
	sha256msg2	%xmm4,%xmm5
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm5,%xmm7
	palignr	$4,%xmm4,%xmm7
	nop
	paddd	%xmm7,%xmm6
	sha256msg1	%xmm4,%xmm3
	sha256rnds2	%xmm2,%xmm1
	movdqa	192-128(%rcx),%xmm0
	paddd	%xmm5,%xmm0
	sha256msg2	%xmm5,%xmm6
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm6,%xmm7
	palignr	$4,%xmm5,%xmm7
	nop
	paddd	%xmm7,%xmm3
	sha256msg1	%xmm5,%xmm4
	sha256rnds2	%xmm2,%xmm1
	movdqa	224-128(%rcx),%xmm0
	paddd	%xmm6,%xmm0
	sha256msg2	%xmm6,%xmm3
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm3,%xmm7
	palignr	$4,%xmm6,%xmm7
	nop
	paddd	%xmm7,%xmm4
	sha256msg1	%xmm6,%xmm5
	sha256rnds2	%xmm2,%xmm1
	movdqa	256-128(%rcx),%xmm0
	paddd	%xmm3,%xmm0
	sha256msg2	%xmm3,%xmm4
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm4,%xmm7
	palignr	$4,%xmm3,%xmm7
	nop
	paddd	%xmm7,%xmm5
	sha256msg1	%xmm3,%xmm6
	sha256rnds2	%xmm2,%xmm1
	movdqa	288-128(%rcx),%xmm0
	paddd	%xmm4,%xmm0
	sha256msg2	%xmm4,%xmm5
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm5,%xmm7
	palignr	$4,%xmm4,%xmm7
	nop
	paddd	%xmm7,%xmm6
	sha256msg1	%xmm4,%xmm3
	sha256rnds2	%xmm2,%xmm1
	movdqa	320-128(%rcx),%xmm0
	paddd	%xmm5,%xmm0
	sha256msg2	%xmm5,%xmm6
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm6,%xmm7
	palignr	$4,%xmm5,%xmm7
	nop
	paddd	%xmm7,%xmm3
	sha256msg1	%xmm5,%xmm4
	sha256rnds2	%xmm2,%xmm1
	movdqa	352-128(%rcx),%xmm0
	paddd	%xmm6,%xmm0
	sha256msg2	%xmm6,%xmm3
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm3,%xmm7
	palignr	$4,%xmm6,%xmm7
	nop
	paddd	%xmm7,%xmm4
	sha256msg1	%xmm6,%xmm5
	sha256rnds2	%xmm2,%xmm1
	movdqa	384-128(%rcx),%xmm0
	paddd	%xmm3,%xmm0
	sha256msg2	%xmm3,%xmm4
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm4,%xmm7
	palignr	$4,%xmm3,%xmm7
	nop
	paddd	%xmm7,%xmm5
	sha256msg1	%xmm3,%xmm6
	sha256rnds2	%xmm2,%xmm1
	movdqa	416-128(%rcx),%xmm0
	paddd	%xmm4,%xmm0
	sha256msg2	%xmm4,%xmm5
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	movdqa	%xmm5,%xmm7
	palignr	$4,%xmm4,%xmm7
	sha256rnds2	%xmm2,%xmm1
	paddd	%xmm7,%xmm6

	movdqa	448-128(%rcx),%xmm0
	paddd	%xmm5,%xmm0
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	sha256msg2	%xmm5,%xmm6
	movdqa	%xmm8,%xmm7
	sha256rnds2	%xmm2,%xmm1

	movdqa	480-128(%rcx),%xmm0
	paddd	%xmm6,%xmm0
	nop
	sha256rnds2	%xmm1,%xmm2
	pshufd	$0x0e,%xmm0,%xmm0
	decq	%rdx
	nop
	sha256rnds2	%xmm2,%xmm1

	paddd	%xmm10,%xmm2
	paddd	%xmm9,%xmm1
	jnz	.Loop_shaext

	pshufd	$0xb1,%xmm2,%xmm2
	pshufd	$0x1b,%xmm1,%xmm7
	pshufd	$0xb1,%xmm1,%xmm1
	punpckhqdq	%xmm2,%xmm1
	palignr	$8,%xmm7,%xmm2

	movdqu	%xmm1,(%rdi)
	movdqu	%xmm2,16(%rdi)
	ret
.cfi_endproc	
.size	sha256_block_data_order_hw,.-sha256_block_data_order_hw

.section	.rodata
.align	64
.type	K256,@object
K256:
.long	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5
.long	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5
.long	0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5
.long	0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5
.long	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3
.long	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3
.long	0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174
.long	0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174
.long	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc
.long	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc
.long	0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da
.long	0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da
.long	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7
.long	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7
.long	0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967
.long	0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967
.long	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13
.long	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13
.long	0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85
.long	0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85
.long	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3
.long	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3
.long	0xd192e819,0xd6990624,0xf40e3585,0x106aa070
.long	0xd192e819,0xd6990624,0xf40e3585,0x106aa070
.long	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5
.long	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5
.long	0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3
.long	0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3
.long	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208
.long	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208
.long	0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
.long	0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2

// Byte-reverse shuffle mask used by `pshufb` in the SHA-NI loop;
// accessed as `512-128(%rcx)` (= K256+384 from rcx=K256+128).
.long	0x00010203,0x04050607,0x08090a0b,0x0c0d0e0f
.long	0x00010203,0x04050607,0x08090a0b,0x0c0d0e0f

#endif
