/*
 * App-local extension of the neuralSPOT Apollo4P linker script.
 *
 * The generic script supplies the boot-compatible 0x18000 application image.
 * This section is NOLOAD and is placed by ld after all loadable MRAM sections;
 * it therefore has no fixed MRAM address and does not add checkpoint bytes to
 * the deployable .bin. The map/objdump checks are mandatory before enabling
 * MRAM writes on target.
 */
INCLUDE "./neuralspot/ns-core/src/apollo4p/gcc/linker_script.ld"

SECTIONS
{
    .bisen_retained (NOLOAD) :
    {
        . = ALIGN(8);
        __bisen_retained_start__ = .;
        KEEP(*(.bisen_retained))
        KEEP(*(.bisen_retained.*))
        . = ALIGN(8);
        __bisen_retained_end__ = .;
    } > MCU_TCM

    .bisen_checkpoint (NOLOAD) :
    {
        . = ALIGN(16);
        __bisen_checkpoint_start__ = .;
        . += 160;  /* Two 80-byte, 16-byte-aligned checkpoint records. */
        . = ALIGN(16);
        __bisen_checkpoint_end__ = .;
    } > MCU_MRAM
}
