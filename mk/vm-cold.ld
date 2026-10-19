/* Augment the pinned SDK script without editing the SDK. */
SECTIONS {
  .p8p_vm_cold : ALIGN(64) {
    *(.p8p_vm_cold)
  } > SDRAM
}
INSERT AFTER .bss;
