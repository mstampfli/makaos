	.text
	.file	"asm_offsets.c"
	.globl	asm_offsets                     # -- Begin function asm_offsets
	.p2align	4, 0x90
	.type	asm_offsets,@function
asm_offsets:                            # @asm_offsets
.Lfunc_begin0:
	.file	0 "/home/maka/dev/MakaOS/bootloader-kernel-in-NASM" "kernel/arch/x86_64/asm_offsets.c" md5 0x5b51bd979ed68fb8bdaa7fc692941912
	.loc	0 28 0                          # kernel/arch/x86_64/asm_offsets.c:28:0
	.cfi_sections .debug_frame
	.cfi_startproc
# %bb.0:
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
.Ltmp0:
	.loc	0 29 5 prologue_end             # kernel/arch/x86_64/asm_offsets.c:29:5
	#APP
	.ascii	"@@@ASMDEF CPU_TSS_RSP0 17716"
	#NO_APP
	.loc	0 34 5                          # kernel/arch/x86_64/asm_offsets.c:34:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_USER_RSP 17816"
	#NO_APP
	.loc	0 35 5                          # kernel/arch/x86_64/asm_offsets.c:35:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_USER_RIP 17824"
	#NO_APP
	.loc	0 36 5                          # kernel/arch/x86_64/asm_offsets.c:36:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_USER_RFLAGS 17832"
	#NO_APP
	.loc	0 37 5                          # kernel/arch/x86_64/asm_offsets.c:37:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_USER_RBP 17840"
	#NO_APP
	.loc	0 38 5                          # kernel/arch/x86_64/asm_offsets.c:38:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_USER_RBX 17848"
	#NO_APP
	.loc	0 39 5                          # kernel/arch/x86_64/asm_offsets.c:39:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_USER_R12 17856"
	#NO_APP
	.loc	0 40 5                          # kernel/arch/x86_64/asm_offsets.c:40:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_USER_R13 17864"
	#NO_APP
	.loc	0 41 5                          # kernel/arch/x86_64/asm_offsets.c:41:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_USER_R14 17872"
	#NO_APP
	.loc	0 42 5                          # kernel/arch/x86_64/asm_offsets.c:42:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_USER_R15 17880"
	#NO_APP
	.loc	0 43 5                          # kernel/arch/x86_64/asm_offsets.c:43:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_ARG5 17888"
	#NO_APP
	.loc	0 44 5                          # kernel/arch/x86_64/asm_offsets.c:44:5
	#APP
	.ascii	"@@@ASMDEF CPU_SC_ARG6 17896"
	#NO_APP
	.loc	0 45 5                          # kernel/arch/x86_64/asm_offsets.c:45:5
	#APP
	.ascii	"@@@ASMDEF CPU_SIG_DELIVER 17904"
	#NO_APP
	.loc	0 46 5                          # kernel/arch/x86_64/asm_offsets.c:46:5
	#APP
	.ascii	"@@@ASMDEF CPU_SIG_IN_SYSCALL 17905"
	#NO_APP
	.loc	0 47 5                          # kernel/arch/x86_64/asm_offsets.c:47:5
	#APP
	.ascii	"@@@ASMDEF CPU_SIG_RDI 17912"
	#NO_APP
	.loc	0 48 5                          # kernel/arch/x86_64/asm_offsets.c:48:5
	#APP
	.ascii	"@@@ASMDEF CPU_EXEC_REQUESTED 17920"
	#NO_APP
	.loc	0 49 5                          # kernel/arch/x86_64/asm_offsets.c:49:5
	#APP
	.ascii	"@@@ASMDEF CPU_EXEC_ENTRY 17928"
	#NO_APP
	.loc	0 50 5                          # kernel/arch/x86_64/asm_offsets.c:50:5
	#APP
	.ascii	"@@@ASMDEF CPU_EXEC_RSP 17936"
	#NO_APP
	.loc	0 51 5                          # kernel/arch/x86_64/asm_offsets.c:51:5
	#APP
	.ascii	"@@@ASMDEF CPU_EXEC_PML4 17944"
	#NO_APP
	.loc	0 52 1 epilogue_begin           # kernel/arch/x86_64/asm_offsets.c:52:1
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	retq
.Ltmp1:
.Lfunc_end0:
	.size	asm_offsets, .Lfunc_end0-asm_offsets
	.cfi_endproc
                                        # -- End function
	.section	.debug_abbrev,"",@progbits
	.byte	1                               # Abbreviation Code
	.byte	17                              # DW_TAG_compile_unit
	.byte	1                               # DW_CHILDREN_yes
	.byte	37                              # DW_AT_producer
	.byte	37                              # DW_FORM_strx1
	.byte	19                              # DW_AT_language
	.byte	5                               # DW_FORM_data2
	.byte	3                               # DW_AT_name
	.byte	37                              # DW_FORM_strx1
	.byte	114                             # DW_AT_str_offsets_base
	.byte	23                              # DW_FORM_sec_offset
	.byte	16                              # DW_AT_stmt_list
	.byte	23                              # DW_FORM_sec_offset
	.byte	27                              # DW_AT_comp_dir
	.byte	37                              # DW_FORM_strx1
	.byte	17                              # DW_AT_low_pc
	.byte	27                              # DW_FORM_addrx
	.byte	18                              # DW_AT_high_pc
	.byte	6                               # DW_FORM_data4
	.byte	115                             # DW_AT_addr_base
	.byte	23                              # DW_FORM_sec_offset
	.byte	0                               # EOM(1)
	.byte	0                               # EOM(2)
	.byte	2                               # Abbreviation Code
	.byte	46                              # DW_TAG_subprogram
	.byte	0                               # DW_CHILDREN_no
	.byte	17                              # DW_AT_low_pc
	.byte	27                              # DW_FORM_addrx
	.byte	18                              # DW_AT_high_pc
	.byte	6                               # DW_FORM_data4
	.byte	64                              # DW_AT_frame_base
	.byte	24                              # DW_FORM_exprloc
	.byte	122                             # DW_AT_call_all_calls
	.byte	25                              # DW_FORM_flag_present
	.byte	3                               # DW_AT_name
	.byte	37                              # DW_FORM_strx1
	.byte	58                              # DW_AT_decl_file
	.byte	11                              # DW_FORM_data1
	.byte	59                              # DW_AT_decl_line
	.byte	11                              # DW_FORM_data1
	.byte	39                              # DW_AT_prototyped
	.byte	25                              # DW_FORM_flag_present
	.byte	63                              # DW_AT_external
	.byte	25                              # DW_FORM_flag_present
	.byte	0                               # EOM(1)
	.byte	0                               # EOM(2)
	.byte	0                               # EOM(3)
	.section	.debug_info,"",@progbits
.Lcu_begin0:
	.long	.Ldebug_info_end0-.Ldebug_info_start0 # Length of Unit
.Ldebug_info_start0:
	.short	5                               # DWARF version number
	.byte	1                               # DWARF Unit Type
	.byte	8                               # Address Size (in bytes)
	.long	.debug_abbrev                   # Offset Into Abbrev. Section
	.byte	1                               # Abbrev [1] 0xc:0x23 DW_TAG_compile_unit
	.byte	0                               # DW_AT_producer
	.short	29                              # DW_AT_language
	.byte	1                               # DW_AT_name
	.long	.Lstr_offsets_base0             # DW_AT_str_offsets_base
	.long	.Lline_table_start0             # DW_AT_stmt_list
	.byte	2                               # DW_AT_comp_dir
	.byte	0                               # DW_AT_low_pc
	.long	.Lfunc_end0-.Lfunc_begin0       # DW_AT_high_pc
	.long	.Laddr_table_base0              # DW_AT_addr_base
	.byte	2                               # Abbrev [2] 0x23:0xb DW_TAG_subprogram
	.byte	0                               # DW_AT_low_pc
	.long	.Lfunc_end0-.Lfunc_begin0       # DW_AT_high_pc
	.byte	1                               # DW_AT_frame_base
	.byte	86
                                        # DW_AT_call_all_calls
	.byte	3                               # DW_AT_name
	.byte	0                               # DW_AT_decl_file
	.byte	28                              # DW_AT_decl_line
                                        # DW_AT_prototyped
                                        # DW_AT_external
	.byte	0                               # End Of Children Mark
.Ldebug_info_end0:
	.section	.debug_str_offsets,"",@progbits
	.long	20                              # Length of String Offsets Set
	.short	5
	.short	0
.Lstr_offsets_base0:
	.section	.debug_str,"MS",@progbits,1
.Linfo_string0:
	.asciz	"Ubuntu clang version 18.1.3 (1ubuntu1)" # string offset=0
.Linfo_string1:
	.asciz	"kernel/arch/x86_64/asm_offsets.c" # string offset=39
.Linfo_string2:
	.asciz	"/home/maka/dev/MakaOS/bootloader-kernel-in-NASM" # string offset=72
.Linfo_string3:
	.asciz	"asm_offsets"                   # string offset=120
	.section	.debug_str_offsets,"",@progbits
	.long	.Linfo_string0
	.long	.Linfo_string1
	.long	.Linfo_string2
	.long	.Linfo_string3
	.section	.debug_addr,"",@progbits
	.long	.Ldebug_addr_end0-.Ldebug_addr_start0 # Length of contribution
.Ldebug_addr_start0:
	.short	5                               # DWARF version number
	.byte	8                               # Address size
	.byte	0                               # Segment selector size
.Laddr_table_base0:
	.quad	.Lfunc_begin0
.Ldebug_addr_end0:
	.ident	"Ubuntu clang version 18.1.3 (1ubuntu1)"
	.section	".note.GNU-stack","",@progbits
	.addrsig
	.section	.debug_line,"",@progbits
.Lline_table_start0:
