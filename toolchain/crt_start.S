/* minimal glibc crt1 for aarch64 */
	.text
	.globl _start
	.type _start,%function
_start:
	mov x29, #0
	mov x30, #0
	mov x5, x0              /* rtld_fini */
	ldr x1, [sp]            /* argc */
	add x2, sp, #8          /* argv */
	mov x6, sp              /* stack_end */
	adrp x0, :got:main
	ldr x0, [x0, #:got_lo12:main]
	adrp x3, :got:__my_csu_init
	ldr x3, [x3, #:got_lo12:__my_csu_init]
	mov x4, #0
	bl __libc_start_main
	brk #0
	.size _start, .-_start
	.section .note.GNU-stack,"",%progbits
