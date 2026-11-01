	.text
	.globl _start
_start:
	xor %ebp,%ebp
	mov %rdx,%r9            /* rtld_fini */
	pop %rsi                /* argc */
	mov %rsp,%rdx           /* argv */
	and $-16,%rsp
	push %rax
	push %rsp               /* stack_end */
	mov main@GOTPCREL(%rip),%rdi
	mov __my_csu_init@GOTPCREL(%rip),%rcx
	xor %r8d,%r8d
	call *__libc_start_main@GOTPCREL(%rip)
	hlt
	.section .note.GNU-stack,"",%progbits
