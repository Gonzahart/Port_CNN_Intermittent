/*
 * App-local extension of the neuralSPOT Apollo4P linker script.
 *
 * Each slot is CKPT_HDR_SZ (16) + NN_LIVE_MAX (8064) = 8080 bytes.
 * The section is NOLOAD, 16-byte aligned, and placed after this app's loadable
 * MRAM image. It is absent from the deployable .bin. Applications are flashed
 * one at a time into the same application region; old checkpoints are not
 * guaranteed to survive switching images. HVR1 identifies this app's records.
 */
INCLUDE "./neuralspot/ns-core/src/apollo4p/gcc/linker_script.ld"

SECTIONS
{
    /*
     * Calibration results captured while the onboard J-Link USB is absent.
     * NOLOAD keeps startup code from clearing the records if reconnecting the
     * debugger happens to reset the MCU. This is volatile TCM, not MRAM.
     */
    .bisen_vdd_cal_retained (NOLOAD) :
    {
        . = ALIGN(8);
        __bisen_vdd_cal_retained_start__ = .;
        KEEP(*(.bisen_vdd_cal_retained))
        KEEP(*(.bisen_vdd_cal_retained.*))
        . = ALIGN(8);
        __bisen_vdd_cal_retained_end__ = .;
    } > MCU_TCM

    .bisen_camera_checkpoint (NOLOAD) :
    {
        . = ALIGN(16);
        __bisen_camera_checkpoint_start__ = .;
        . += 16160;  /* Two 8,080-byte slots. */
        . = ALIGN(16);
        __bisen_camera_checkpoint_end__ = .;
        __bisen_camera_session_start__ = .;
        . += 32;     /* Two atomic 16-byte continuous-run records. */
        . = ALIGN(16);
        __bisen_camera_session_end__ = .;
    } > MCU_MRAM
}
