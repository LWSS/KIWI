; Win64 wrappers for the recovered x87 math operations.
; Only volatile registers and the caller-provided 32-byte home area are used.
; Preserve the caller's control word and use the original 53-bit precision.
.code

SAVE_PRECISION MACRO
    fnstcw WORD PTR [rsp+24]
    mov ax, WORD PTR [rsp+24]
    and ax, 0FCFFh
    or ax, 0200h
    mov WORD PTR [rsp+26], ax
    fldcw WORD PTR [rsp+26]
ENDM
RESTORE_PRECISION MACRO
    fldcw WORD PTR [rsp+24]
ENDM

x87_sin PROC
    SAVE_PRECISION
    movsd QWORD PTR [rsp+8], xmm0
    fld QWORD PTR [rsp+8]
    fsin
    fstp QWORD PTR [rsp+8]
    movsd xmm0, QWORD PTR [rsp+8]
    RESTORE_PRECISION
    ret
x87_sin ENDP

x87_cos PROC
    SAVE_PRECISION
    movsd QWORD PTR [rsp+8], xmm0
    fld QWORD PTR [rsp+8]
    fcos
    fstp QWORD PTR [rsp+8]
    movsd xmm0, QWORD PTR [rsp+8]
    RESTORE_PRECISION
    ret
x87_cos ENDP

x87_atan2 PROC
    SAVE_PRECISION
    movsd QWORD PTR [rsp+8], xmm0
    movsd QWORD PTR [rsp+16], xmm1
    fld QWORD PTR [rsp+8]
    fld QWORD PTR [rsp+16]
    fpatan
    fstp QWORD PTR [rsp+8]
    movsd xmm0, QWORD PTR [rsp+8]
    RESTORE_PRECISION
    ret
x87_atan2 ENDP

x87_MatTransVec2 PROC
    SAVE_PRECISION
    xor r9d, r9d
component:
    fld DWORD PTR [rdx]
    fmul DWORD PTR [rcx+r9+12]
    fld DWORD PTR [rdx+4]
    fmul DWORD PTR [rcx+r9+24]
    faddp st(1), st(0)
    fld DWORD PTR [rdx+8]
    fmul DWORD PTR [rcx+r9+36]
    faddp st(1), st(0)
    fstp DWORD PTR [r8+r9]
    add r9d, 4
    cmp r9d, 12
    jne component
    RESTORE_PRECISION
    ret
x87_MatTransVec2 ENDP
END
