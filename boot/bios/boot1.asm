org 0x7C00 ; Bootloader starts at memory address 0x7C00
bits 16

start: 
  xor ax, ax
  mov ds, ax
  mov es, ax

  cli
  mov ss, ax
  mov sp, 0x7C00
  sti

  mov [boot_drive], dl
  mov ah, 0x42
  mov dl, [boot_drive]
  mov si, dap_boot2
  int 0x13
  jc disk_error
  
  cli
  mov dl, [boot_drive]; to ENSURE dl contains the boot drive for stage 2
  jmp 0x800:0x0000; jump to stage 2 bootloader 

disk_error:
  hlt
  jmp disk_error

boot_drive: db 0

dap_boot2: 
  db 16 ;size
  db 0; reserved 0
  dw 32; how many sectors to store 
  dw 0; destination offset
  dw 0x800; destination segment
  dq 1; disk sector location (lba)

times 510 - ($ - $$) db 0
dw 0xAA55
 
