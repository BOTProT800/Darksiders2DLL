; Test-only reproduction of the supported game's caller frame at 0x9FF0CD.
; RCX = function, RDX = destination, R8D = size, R9 = zero-based file index.
; The same indirect call is used for the recorder and the production detour.
; This is not linked into dinput8.dll and never installs a game hook.
PUBLIC InvokeResourceReadFrame
.code
InvokeResourceReadFrame PROC FRAME
    sub rsp, 0A8h
    .allocstack 0A8h
    .endprolog
    mov rax, rcx
    mov qword ptr [rsp+30h], r9
    lea rcx, [rsp+70h]
    call rax
    add rsp, 0A8h
    ret
InvokeResourceReadFrame ENDP
END
