; Test-only reproduction of the supported game's caller frame at 0x9FF0CD.
; RCX = function, RDX = destination, R8D = size, R9 = zero-based file index.
; The same indirect call is used for the recorder and the production detour.
; This is not linked into dinput8.dll and never installs a game hook.
PUBLIC InvokeResourceReadFrame
PUBLIC InvokeEngineFrame
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

; One 0x9FEE5C frame F = RSP+30h ([F+30h] = file index, wrapper at F+70h)
; for both paths, so direct reads and skips share the wrapper as in the game.
; Fifth argument zero: the direct read at 0x9FF0CD (RSP = F at the call).
; Otherwise the skip path: 0x100120 called from 0x9FF040 keeps that value as
; its return address at F-8, and its prologue (push rdi; sub rsp,20h) leaves
; RSP = F-30h at its read call.
InvokeEngineFrame PROC FRAME
    sub rsp, 0D8h
    .allocstack 0D8h
    .endprolog
    mov rax, rcx
    mov qword ptr [rsp+60h], r9
    lea rcx, [rsp+0A0h]
    mov r10, qword ptr [rsp+100h]
    test r10, r10
    jnz skip_path
    add rsp, 30h
    call rax
    sub rsp, 30h
    jmp done
skip_path:
    mov qword ptr [rsp+28h], r10
    call rax
done:
    add rsp, 0D8h
    ret
InvokeEngineFrame ENDP
END
