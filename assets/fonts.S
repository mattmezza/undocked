.section .rodata
.balign 8
.globl geist_start, geist_end, geist_mono_start, geist_mono_end
geist_start:
.incbin "assets/Geist-Regular.ttf"
geist_end:
.balign 8
geist_mono_start:
.incbin "assets/GeistMono-Regular.ttf"
geist_mono_end:
.section .note.GNU-stack,"",%progbits
