/*
 * App-local extension of the neuralSPOT Apollo4P linker script.
 *
 * Each slot is CKPT_HDR_SZ (16) + NN_LIVE_MAX (8064) = 8080 bytes.
 * The section is NOLOAD, 16-byte aligned, and placed after this app's loadable
 * MRAM image. It is absent from the deployable .bin and cannot overwrite the
 * secure bootloader or another application's fixed region.
 */
INCLUDE "./neuralspot/ns-core/src/apollo4p/gcc/linker_script.ld"

SECTIONS
{
    .bisen_camera_checkpoint (NOLOAD) :
    {
        . = ALIGN(16);
        __bisen_camera_checkpoint_start__ = .;
        . += 16160;  /* Two 8,080-byte slots. */
        . = ALIGN(16);
        __bisen_camera_checkpoint_end__ = .;
    } > MCU_MRAM
}
