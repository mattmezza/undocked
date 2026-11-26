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
.section .rodata
.balign 8
.globl geist_medium_start, geist_medium_end, undocked_icon_masks
geist_medium_start:
.incbin "assets/Geist-Medium.ttf"
geist_medium_end:
.balign 8
undocked_icon_masks:
.incbin "assets/icons.alpha"
