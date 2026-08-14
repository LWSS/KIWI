;
; poly2d_asm.asm — exact replica of original AreaX2AndCentroidFor2dPoly
; Assembled with: ml64 /c poly2d_asm.asm
;

.data
ALIGN 16
three_dbl DQ 4008000000000000h  ; 3.0 as double

.code

; float AreaX2AndCentroidFor2dPoly(float *points /*rcx*/, int numPoints /*edx*/, float *centroidOut /*r8*/)
AreaX2AndCentroidFor2dPoly PROC

    sub     rsp, 78h
    movss   xmm3, dword ptr [rcx]
    movss   xmm2, dword ptr [rcx+8]
    movdqa  [rsp+60h], xmm6
    movdqa  [rsp+50h], xmm7
    movdqa  [rsp+40h], xmm8
    movdqa  [rsp+30h], xmm9
    lea     r9d, [rdx-1]
    lea     rdx, [rcx+4]
    cmp     r9d, 1
    movsxd  rax, r9d
    movss   xmm9, dword ptr [rdx]
    movss   xmm8, dword ptr [rcx+0Ch]
    movss   xmm7, dword ptr [rcx+rax*8+4]
    movss   xmm5, dword ptr [rcx+rax*8]
    movdqa  [rsp+20h], xmm10
    movaps  xmm6, xmm7
    movdqa  [rsp+10h], xmm12
    movdqa  [rsp], xmm13
    movss   xmm10, dword ptr [rcx+rax*8-4]
    subss   xmm6, xmm8
    addss   xmm8, xmm9
    movaps  xmm1, xmm2
    movaps  xmm4, xmm3
    mulss   xmm6, xmm3
    movaps  xmm0, xmm6
    subss   xmm4, dword ptr [rcx+rax*8-8]
    addss   xmm8, xmm7
    subss   xmm10, xmm9
    mulss   xmm10, xmm5
    mulss   xmm8, xmm6
    mulss   xmm4, xmm7
    addss   xmm0, xmm10
    addss   xmm7, xmm9
    addss   xmm1, xmm3
    addss   xmm7, dword ptr [rcx+rax*8-4]
    cvtss2sd xmm13, xmm0
    subss   xmm2, xmm5
    movaps  xmm0, xmm3
    mulss   xmm2, xmm9
    movdqa  xmm9, [rsp+30h]
    mulss   xmm7, xmm10
    movdqa  xmm10, [rsp+20h]
    addss   xmm0, xmm5
    addss   xmm1, xmm5
    addss   xmm8, xmm7
    mulss   xmm1, xmm2
    addss   xmm0, dword ptr [rcx+rax*8-8]
    cvtss2sd xmm8, xmm8
    mulss   xmm4, xmm0
    addss   xmm4, xmm1
    cvtss2sd xmm12, xmm4
    jle     epilogue
    lea     ecx, [r9-1]
    ; alignment nops
    DB 66h, 66h, 66h, 90h
    DB 66h, 66h, 90h
    DB 66h, 66h, 90h

loop_top:
    movss   xmm7, dword ptr [rdx]
    prefetchnta byte ptr [rdx+40h]
    movss   xmm5, dword ptr [rdx+10h]
    movss   xmm4, dword ptr [rdx+4]
    movss   xmm2, dword ptr [rdx+0Ch]
    movaps  xmm6, xmm7
    movss   xmm1, dword ptr [rdx-4]
    movss   xmm3, dword ptr [rdx+8]
    subss   xmm6, xmm5
    add     rdx, 8
    dec     rcx
    mulss   xmm6, xmm4
    addss   xmm4, xmm2
    cvtss2sd xmm0, xmm6
    subss   xmm2, xmm1
    mulss   xmm2, xmm3
    addss   xmm3, xmm5
    addsd   xmm13, xmm0
    addss   xmm4, xmm1
    mulss   xmm4, xmm2
    cvtss2sd xmm0, xmm4
    addss   xmm3, xmm7
    addsd   xmm12, xmm0
    mulss   xmm3, xmm6
    cvtss2sd xmm0, xmm3
    addsd   xmm8, xmm0
    jnz     loop_top

epilogue:
    movdqa  xmm7, [rsp+50h]
    movdqa  xmm6, [rsp+60h]
    movsd   xmm1, xmm13
    mulsd   xmm1, three_dbl
    divsd   xmm12, xmm1
    divsd   xmm8, xmm1
    cvtsd2ss xmm0, xmm12
    movdqa  xmm12, [rsp+10h]
    movss   dword ptr [r8], xmm0
    cvtsd2ss xmm0, xmm8
    movdqa  xmm8, [rsp+40h]
    movss   dword ptr [r8+4], xmm0
    cvtsd2ss xmm0, xmm13
    movdqa  xmm13, [rsp]
    add     rsp, 78h
    ret

AreaX2AndCentroidFor2dPoly ENDP

END
