
/tmp/gp32-round4-build-clang/CMakeFiles/gp32emu.dir/src/audio/gp32_audio_resampler.c.o:     file format elf64-x86-64


Disassembly of section .text:

0000000000000330 <gp32_audio_resampler_output_frames>:
     330:	31 c0                	xor    %eax,%eax
     332:	48 85 ff             	test   %rdi,%rdi
     335:	0f 84 a6 01 00 00    	je     4e1 <gp32_audio_resampler_output_frames+0x1b1>
     33b:	48 85 f6             	test   %rsi,%rsi
     33e:	0f 84 9d 01 00 00    	je     4e1 <gp32_audio_resampler_output_frames+0x1b1>
     344:	85 d2                	test   %edx,%edx
     346:	0f 84 95 01 00 00    	je     4e1 <gp32_audio_resampler_output_frames+0x1b1>
     34c:	85 c9                	test   %ecx,%ecx
     34e:	0f 84 8d 01 00 00    	je     4e1 <gp32_audio_resampler_output_frames+0x1b1>
     354:	41 56                	push   %r14
     356:	53                   	push   %rbx
     357:	8b 1f                	mov    (%rdi),%ebx
     359:	45 31 d2             	xor    %r10d,%r10d
     35c:	48 85 db             	test   %rbx,%rbx
     35f:	74 30                	je     391 <gp32_audio_resampler_output_frames+0x61>
     361:	41 b9 00 00 00 00    	mov    $0x0,%r9d
     367:	39 4f 04             	cmp    %ecx,0x4(%rdi)
     36a:	75 7c                	jne    3e8 <gp32_audio_resampler_output_frames+0xb8>
     36c:	4c 8b 4f 08          	mov    0x8(%rdi),%r9
     370:	44 8b 57 14          	mov    0x14(%rdi),%r10d
     374:	39 d3                	cmp    %edx,%ebx
     376:	74 70                	je     3e8 <gp32_audio_resampler_output_frames+0xb8>
     378:	41 89 d3             	mov    %edx,%r11d
     37b:	4c 89 c8             	mov    %r9,%rax
     37e:	48 c1 e8 20          	shr    $0x20,%rax
     382:	74 12                	je     396 <gp32_audio_resampler_output_frames+0x66>
     384:	4c 89 c8             	mov    %r9,%rax
     387:	31 d2                	xor    %edx,%edx
     389:	48 f7 f3             	div    %rbx
     38c:	48 89 c7             	mov    %rax,%rdi
     38f:	eb 0e                	jmp    39f <gp32_audio_resampler_output_frames+0x6f>
     391:	45 31 c9             	xor    %r9d,%r9d
     394:	eb 52                	jmp    3e8 <gp32_audio_resampler_output_frames+0xb8>
     396:	44 89 c8             	mov    %r9d,%eax
     399:	31 d2                	xor    %edx,%edx
     39b:	f7 f3                	div    %ebx
     39d:	89 c7                	mov    %eax,%edi
     39f:	45 89 de             	mov    %r11d,%r14d
     3a2:	49 0f af d6          	imul   %r14,%rdx
     3a6:	48 89 d0             	mov    %rdx,%rax
     3a9:	48 c1 e8 20          	shr    $0x20,%rax
     3ad:	74 0d                	je     3bc <gp32_audio_resampler_output_frames+0x8c>
     3af:	48 89 d0             	mov    %rdx,%rax
     3b2:	31 d2                	xor    %edx,%edx
     3b4:	48 f7 f3             	div    %rbx
     3b7:	49 89 c1             	mov    %rax,%r9
     3ba:	eb 09                	jmp    3c5 <gp32_audio_resampler_output_frames+0x95>
     3bc:	89 d0                	mov    %edx,%eax
     3be:	31 d2                	xor    %edx,%edx
     3c0:	f7 f3                	div    %ebx
     3c2:	41 89 c1             	mov    %eax,%r9d
     3c5:	4c 89 c8             	mov    %r9,%rax
     3c8:	48 f7 d0             	not    %rax
     3cb:	31 d2                	xor    %edx,%edx
     3cd:	49 f7 f6             	div    %r14
     3d0:	4c 0f af f7          	imul   %rdi,%r14
     3d4:	4d 01 ce             	add    %r9,%r14
     3d7:	48 39 c7             	cmp    %rax,%rdi
     3da:	49 c7 c1 ff ff ff ff 	mov    $0xffffffffffffffff,%r9
     3e1:	4d 0f 46 ce          	cmovbe %r14,%r9
     3e5:	44 89 da             	mov    %r11d,%edx
     3e8:	41 83 fa 01          	cmp    $0x1,%r10d
     3ec:	48 83 de 00          	sbb    $0x0,%rsi
     3f0:	48 89 f0             	mov    %rsi,%rax
     3f3:	48 c1 e8 20          	shr    $0x20,%rax
     3f7:	48 c7 c0 ff ff ff ff 	mov    $0xffffffffffffffff,%rax
     3fe:	0f 85 da 00 00 00    	jne    4de <gp32_audio_resampler_output_frames+0x1ae>
     404:	48 c1 e6 20          	shl    $0x20,%rsi
     408:	49 39 f1             	cmp    %rsi,%r9
     40b:	0f 83 c0 00 00 00    	jae    4d1 <gp32_audio_resampler_output_frames+0x1a1>
     411:	41 81 f8 50 c3 00 00 	cmp    $0xc350,%r8d
     418:	bf 50 c3 00 00       	mov    $0xc350,%edi
     41d:	41 0f 4c f8          	cmovl  %r8d,%edi
     421:	81 ff b1 3c ff ff    	cmp    $0xffff3cb1,%edi
     427:	41 b8 b0 3c ff ff    	mov    $0xffff3cb0,%r8d
     42d:	44 0f 4d c7          	cmovge %edi,%r8d
     431:	f2 41 0f 2a c0       	cvtsi2sd %r8d,%xmm0
     436:	f2 0f 5e 05 00 00 00 	divsd  0x0(%rip),%xmm0        # 43e <gp32_audio_resampler_output_frames+0x10e>
     43d:	00 
     43e:	f2 0f 10 0d 00 00 00 	movsd  0x0(%rip),%xmm1        # 446 <gp32_audio_resampler_output_frames+0x116>
     445:	00 
     446:	f2 0f 58 c1          	addsd  %xmm1,%xmm0
     44a:	f2 0f 10 15 00 00 00 	movsd  0x0(%rip),%xmm2        # 452 <gp32_audio_resampler_output_frames+0x122>
     451:	00 
     452:	66 0f 28 da          	movapd %xmm2,%xmm3
     456:	f2 0f 5f d8          	maxsd  %xmm0,%xmm3
     45a:	89 c9                	mov    %ecx,%ecx
     45c:	0f 57 c0             	xorps  %xmm0,%xmm0
     45f:	f2 48 0f 2a c1       	cvtsi2sd %rcx,%xmm0
     464:	f2 0f 59 c3          	mulsd  %xmm3,%xmm0
     468:	89 d1                	mov    %edx,%ecx
     46a:	0f 57 db             	xorps  %xmm3,%xmm3
     46d:	f2 48 0f 2a d9       	cvtsi2sd %rcx,%xmm3
     472:	f2 0f 5f c8          	maxsd  %xmm0,%xmm1
     476:	f2 0f 59 1d 00 00 00 	mulsd  0x0(%rip),%xmm3        # 47e <gp32_audio_resampler_output_frames+0x14e>
     47d:	00 
     47e:	f2 0f 5e d9          	divsd  %xmm1,%xmm3
     482:	f2 0f 58 da          	addsd  %xmm2,%xmm3
     486:	f2 48 0f 2c d3       	cvttsd2si %xmm3,%rdx
     48b:	48 89 d7             	mov    %rdx,%rdi
     48e:	f2 0f 5c 1d 00 00 00 	subsd  0x0(%rip),%xmm3        # 496 <gp32_audio_resampler_output_frames+0x166>
     495:	00 
     496:	f2 48 0f 2c cb       	cvttsd2si %xmm3,%rcx
     49b:	48 c1 ff 3f          	sar    $0x3f,%rdi
     49f:	48 21 f9             	and    %rdi,%rcx
     4a2:	48 09 d1             	or     %rdx,%rcx
     4a5:	48 83 f9 01          	cmp    $0x1,%rcx
     4a9:	48 83 d1 00          	adc    $0x0,%rcx
     4ad:	48 89 f2             	mov    %rsi,%rdx
     4b0:	48 01 ca             	add    %rcx,%rdx
     4b3:	72 29                	jb     4de <gp32_audio_resampler_output_frames+0x1ae>
     4b5:	49 f7 d1             	not    %r9
     4b8:	4c 01 ce             	add    %r9,%rsi
     4bb:	48 89 f0             	mov    %rsi,%rax
     4be:	48 09 c8             	or     %rcx,%rax
     4c1:	48 c1 e8 20          	shr    $0x20,%rax
     4c5:	74 0e                	je     4d5 <gp32_audio_resampler_output_frames+0x1a5>
     4c7:	48 89 f0             	mov    %rsi,%rax
     4ca:	31 d2                	xor    %edx,%edx
     4cc:	48 f7 f1             	div    %rcx
     4cf:	eb 0a                	jmp    4db <gp32_audio_resampler_output_frames+0x1ab>
     4d1:	31 c0                	xor    %eax,%eax
     4d3:	eb 09                	jmp    4de <gp32_audio_resampler_output_frames+0x1ae>
     4d5:	89 f0                	mov    %esi,%eax
     4d7:	31 d2                	xor    %edx,%edx
     4d9:	f7 f1                	div    %ecx
     4db:	48 ff c0             	inc    %rax
     4de:	5b                   	pop    %rbx
     4df:	41 5e                	pop    %r14
     4e1:	c3                   	ret
