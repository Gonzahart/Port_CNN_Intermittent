095a: engine-r2a, output-stationary, 2026-09-29

Read README.md for application, checkpoint identity, timing and test limits.
README-original.md is the unchanged September 28 application documentation;
its HVR1 references and the unchanged linker's HVR1 comment are historical.
The active OS default is BISEN_HARVEST_CHECKPOINT_MAGIC=0x4F533031 (OS01).
Only the five original engine files and module.mk change against the
46-file original-source record. Three engine headers are added.
No calibration, policy, weights, checkpoint codec or linker layout changed.
Native/ARM acceptance of this package is pending accept_os.sh; no board run.
