; x87_trig_cod2rad.asm — x87 sinf/cosf for x64
; Uses x87 hardware fsin/fcos at double precision (_PC_53)
; to potentially match the original cod2rad64.exe CRT.
;
; Assembled with: ml64.exe /nologo /c x87_trig_cod2rad.asm
; Win64 ABI: float arg in XMM0, float return in XMM0

.code

; float x87_sinf(float x)
x87_sinf PROC
    sub     rsp, 16
    fnstcw  WORD PTR [rsp+8]
    mov     ax, WORD PTR [rsp+8]
    and     ax, 0FCFFh          ; clear precision bits
    or      ax, 0200h           ; set _PC_53 (double precision)
    mov     WORD PTR [rsp+10], ax
    fldcw   WORD PTR [rsp+10]
    movss   DWORD PTR [rsp], xmm0
    fld     DWORD PTR [rsp]     ; load as float -> x87 extended
    fsin
    fstp    DWORD PTR [rsp]     ; store as float
    movss   xmm0, DWORD PTR [rsp]
    fldcw   WORD PTR [rsp+8]    ; restore control word
    add     rsp, 16
    ret
x87_sinf ENDP

; float x87_cosf(float x)
x87_cosf PROC
    sub     rsp, 16
    fnstcw  WORD PTR [rsp+8]
    mov     ax, WORD PTR [rsp+8]
    and     ax, 0FCFFh
    or      ax, 0200h
    mov     WORD PTR [rsp+10], ax
    fldcw   WORD PTR [rsp+10]
    movss   DWORD PTR [rsp], xmm0
    fld     DWORD PTR [rsp]
    fcos
    fstp    DWORD PTR [rsp]
    movss   xmm0, DWORD PTR [rsp]
    fldcw   WORD PTR [rsp+8]
    add     rsp, 16
    ret
x87_cosf ENDP

END
