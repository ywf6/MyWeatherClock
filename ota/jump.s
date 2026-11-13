        AREA |.text|, CODE, READONLY
        THUMB
        PRESERVE8
        EXPORT jump_app
jump_app PROC
        LDR R1, [R0]
        LDR R2, [R0, #4]
        MSR MSP, R1
        BX R2
        ENDP
        END
