
/tmp/gp32-round4-build-gcc-final/CMakeFiles/gp32emu.dir/src/audio/gp32_audio_resampler.c.o:     file format elf64-x86-64


Disassembly of section .text:

0000000000000b40 <gp32_audio_resampler_output_frames>:
     b40:	f3 0f 1e fa          	endbr64
     b44:	48 85 ff             	test   %rdi,%rdi
     b47:	0f 84 23 01 00 00    	je     c70 <gp32_audio_resampler_output_frames+0x130>
     b4d:	48 85 f6             	test   %rsi,%rsi
     b50:	0f 84 1a 01 00 00    	je     c70 <gp32_audio_resampler_output_frames+0x130>
     b56:	41 89 d1             	mov    %edx,%r9d
     b59:	85 d2                	test   %edx,%edx
     b5b:	0f 84 0f 01 00 00    	je     c70 <gp32_audio_resampler_output_frames+0x130>
     b61:	85 c9                	test   %ecx,%ecx
     b63:	0f 84 07 01 00 00    	je     c70 <gp32_audio_resampler_output_frames+0x130>
     b69:	53                   	push   %rbx
     b6a:	8b 07                	mov    (%rdi),%eax
     b6c:	41 bb 01 00 00 00    	mov    $0x1,%r11d
     b72:	45 31 d2             	xor    %r10d,%r10d
     b75:	85 c0                	test   %eax,%eax
     b77:	0f 85 fb 00 00 00    	jne    c78 <gp32_audio_resampler_output_frames+0x138>
     b7d:	4c 29 de             	sub    %r11,%rsi
     b80:	48 c7 c0 ff ff ff ff 	mov    $0xffffffffffffffff,%rax
     b87:	48 89 f3             	mov    %rsi,%rbx
     b8a:	48 c1 eb 20          	shr    $0x20,%rbx
     b8e:	0f 85 d3 00 00 00    	jne    c67 <gp32_audio_resampler_output_frames+0x127>
     b94:	48 c1 e6 20          	shl    $0x20,%rsi
     b98:	31 c0                	xor    %eax,%eax
     b9a:	49 39 f2             	cmp    %rsi,%r10
     b9d:	0f 83 c4 00 00 00    	jae    c67 <gp32_audio_resampler_output_frames+0x127>
     ba3:	b8 50 c3 00 00       	mov    $0xc350,%eax
     ba8:	66 0f ef c0          	pxor   %xmm0,%xmm0
     bac:	89 c9                	mov    %ecx,%ecx
     bae:	44 89 ca             	mov    %r9d,%edx
     bb1:	41 39 c0             	cmp    %eax,%r8d
     bb4:	f2 0f 10 15 00 00 00 	movsd  0x0(%rip),%xmm2        # bbc <gp32_audio_resampler_output_frames+0x7c>
     bbb:	00 
     bbc:	f2 0f 10 1d 00 00 00 	movsd  0x0(%rip),%xmm3        # bc4 <gp32_audio_resampler_output_frames+0x84>
     bc3:	00 
     bc4:	66 0f ef c9          	pxor   %xmm1,%xmm1
     bc8:	44 0f 4f c0          	cmovg  %eax,%r8d
     bcc:	f2 48 0f 2a c9       	cvtsi2sd %rcx,%xmm1
     bd1:	b8 b0 3c ff ff       	mov    $0xffff3cb0,%eax
     bd6:	66 0f 28 e3          	movapd %xmm3,%xmm4
     bda:	41 39 c0             	cmp    %eax,%r8d
     bdd:	44 0f 4c c0          	cmovl  %eax,%r8d
     be1:	f2 41 0f 2a c0       	cvtsi2sd %r8d,%xmm0
     be6:	f2 0f 5e 05 00 00 00 	divsd  0x0(%rip),%xmm0        # bee <gp32_audio_resampler_output_frames+0xae>
     bed:	00 
     bee:	f2 0f 58 c2          	addsd  %xmm2,%xmm0
     bf2:	f2 0f 5f e0          	maxsd  %xmm0,%xmm4
     bf6:	f2 0f 59 cc          	mulsd  %xmm4,%xmm1
     bfa:	66 0f 28 c1          	movapd %xmm1,%xmm0
     bfe:	f2 0f c2 c2 06       	cmpnlesd %xmm2,%xmm0
     c03:	66 0f 54 c8          	andpd  %xmm0,%xmm1
     c07:	66 0f 55 c2          	andnpd %xmm2,%xmm0
     c0b:	66 0f 56 c8          	orpd   %xmm0,%xmm1
     c0f:	66 0f ef c0          	pxor   %xmm0,%xmm0
     c13:	f2 48 0f 2a c2       	cvtsi2sd %rdx,%xmm0
     c18:	f2 0f 59 05 00 00 00 	mulsd  0x0(%rip),%xmm0        # c20 <gp32_audio_resampler_output_frames+0xe0>
     c1f:	00 
     c20:	f2 0f 5e c1          	divsd  %xmm1,%xmm0
     c24:	f2 0f 10 0d 00 00 00 	movsd  0x0(%rip),%xmm1        # c2c <gp32_audio_resampler_output_frames+0xec>
     c2b:	00 
     c2c:	f2 0f 58 c3          	addsd  %xmm3,%xmm0
     c30:	66 0f 2f c1          	comisd %xmm1,%xmm0
     c34:	0f 83 a6 00 00 00    	jae    ce0 <gp32_audio_resampler_output_frames+0x1a0>
     c3a:	f2 48 0f 2c c8       	cvttsd2si %xmm0,%rcx
     c3f:	48 85 c9             	test   %rcx,%rcx
     c42:	b8 01 00 00 00       	mov    $0x1,%eax
     c47:	48 0f 44 c8          	cmove  %rax,%rcx
     c4b:	48 89 f0             	mov    %rsi,%rax
     c4e:	48 01 c8             	add    %rcx,%rax
     c51:	0f 82 ad 00 00 00    	jb     d04 <gp32_audio_resampler_output_frames+0x1c4>
     c57:	4c 29 d6             	sub    %r10,%rsi
     c5a:	31 d2                	xor    %edx,%edx
     c5c:	48 8d 46 ff          	lea    -0x1(%rsi),%rax
     c60:	48 f7 f1             	div    %rcx
     c63:	48 83 c0 01          	add    $0x1,%rax
     c67:	5b                   	pop    %rbx
     c68:	c3                   	ret
     c69:	0f 1f 80 00 00 00 00 	nopl   0x0(%rax)
     c70:	31 c0                	xor    %eax,%eax
     c72:	c3                   	ret
     c73:	0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
     c78:	3b 4f 04             	cmp    0x4(%rdi),%ecx
     c7b:	0f 85 fc fe ff ff    	jne    b7d <gp32_audio_resampler_output_frames+0x3d>
     c81:	8b 57 14             	mov    0x14(%rdi),%edx
     c84:	45 31 db             	xor    %r11d,%r11d
     c87:	4c 8b 57 08          	mov    0x8(%rdi),%r10
     c8b:	85 d2                	test   %edx,%edx
     c8d:	41 0f 94 c3          	sete   %r11b
     c91:	44 39 c8             	cmp    %r9d,%eax
     c94:	0f 84 e3 fe ff ff    	je     b7d <gp32_audio_resampler_output_frames+0x3d>
     c9a:	89 c7                	mov    %eax,%edi
     c9c:	31 d2                	xor    %edx,%edx
     c9e:	4c 89 d0             	mov    %r10,%rax
     ca1:	44 89 cb             	mov    %r9d,%ebx
     ca4:	48 f7 f7             	div    %rdi
     ca7:	49 89 c2             	mov    %rax,%r10
     caa:	48 89 d0             	mov    %rdx,%rax
     cad:	31 d2                	xor    %edx,%edx
     caf:	48 0f af c3          	imul   %rbx,%rax
     cb3:	48 f7 f7             	div    %rdi
     cb6:	31 d2                	xor    %edx,%edx
     cb8:	48 89 c7             	mov    %rax,%rdi
     cbb:	48 f7 d0             	not    %rax
     cbe:	48 f7 f3             	div    %rbx
     cc1:	4c 39 d0             	cmp    %r10,%rax
     cc4:	73 32                	jae    cf8 <gp32_audio_resampler_output_frames+0x1b8>
     cc6:	4c 29 de             	sub    %r11,%rsi
     cc9:	31 c0                	xor    %eax,%eax
     ccb:	5b                   	pop    %rbx
     ccc:	48 c1 ee 20          	shr    $0x20,%rsi
     cd0:	0f 95 c0             	setne  %al
     cd3:	48 f7 d8             	neg    %rax
     cd6:	c3                   	ret
     cd7:	66 0f 1f 84 00 00 00 	nopw   0x0(%rax,%rax,1)
     cde:	00 00 
     ce0:	f2 0f 5c c1          	subsd  %xmm1,%xmm0
     ce4:	f2 48 0f 2c c8       	cvttsd2si %xmm0,%rcx
     ce9:	48 0f ba f9 3f       	btc    $0x3f,%rcx
     cee:	e9 4c ff ff ff       	jmp    c3f <gp32_audio_resampler_output_frames+0xff>
     cf3:	0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
     cf8:	4c 0f af d3          	imul   %rbx,%r10
     cfc:	49 01 fa             	add    %rdi,%r10
     cff:	e9 79 fe ff ff       	jmp    b7d <gp32_audio_resampler_output_frames+0x3d>
     d04:	48 83 c8 ff          	or     $0xffffffffffffffff,%rax
     d08:	5b                   	pop    %rbx
     d09:	c3                   	ret
