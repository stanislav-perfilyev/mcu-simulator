@ ARM Thumb assembly — Factorial of N (iterative)
@ Assembled to programs/factorial.bin for use with simulator
@
@ Algorithm:
@   r0 = N (input)
@   r1 = result (accumulator)
@   r0 = 5
@   r1 = 1
@ loop:
@   MUL r1, r0   ; r1 *= r0
@   SUB r0, #1   ; r0 -= 1
@   CMP r0, #1
@   BGT loop
@   ; r1 = 5! = 120
@   BKPT         ; signal end

.syntax unified
.thumb

.global _start
_start:
    MOV r0, #5      @ N = 5
    MOV r1, #1      @ result = 1
loop:
    MUL r1, r0      @ result *= r0
    SUB r0, r0, #1  @ r0 -= 1
    CMP r0, #1      @ if r0 > 1, loop
    BGT loop
    @ r1 = 120 = 5!
    @ BX LR to halt simulator (LR=0 with Thumb bit clear)
    BX  lr
