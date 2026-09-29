// engine-080 build contract. Included by nn_engine.h AFTER NN_DATAFLOW.
// Source API names are unchanged. Link symbols include flow/SIMD so separately
// compiled callers, engine, kernels, checkpoint and backend cannot silently mix
// these options. No context or serialized byte changes; SIMD is NOT seed input.
#ifndef NN_BUILD_H
#define NN_BUILD_H

#ifndef NN_SIMD
#define NN_SIMD 1
#endif
#if NN_DATAFLOW != 0 && NN_DATAFLOW != 1
#error "NN_DATAFLOW must be 0 (OS) or 1 (IS)"
#endif
#if NN_SIMD != 0 && NN_SIMD != 1
#error "NN_SIMD must be 0 or 1"
#endif

#define NN_CFG_CAT_(a, b) a##b
#define NN_CFG_CAT(a, b) NN_CFG_CAT_(a, b)
#if NN_DATAFLOW == 0 && NN_SIMD == 0
#define NN_CFG_SUFFIX _df0_simd0
#elif NN_DATAFLOW == 0 && NN_SIMD == 1
#define NN_CFG_SUFFIX _df0_simd1
#elif NN_DATAFLOW == 1 && NN_SIMD == 0
#define NN_CFG_SUFFIX _df1_simd0
#else
#define NN_CFG_SUFFIX _df1_simd1
#endif
#define NN_CFG_NAME(name) NN_CFG_CAT(name, NN_CFG_SUFFIX)

#define k_layers NN_CFG_NAME(k_layers)
#define nn_kernels NN_CFG_NAME(nn_kernels)
#define nn_begin NN_CFG_NAME(nn_begin)
#define nn_step NN_CFG_NAME(nn_step)
#define nn_run NN_CFG_NAME(nn_run)
#define nn_scores NN_CFG_NAME(nn_scores)
#define nn_argmax NN_CFG_NAME(nn_argmax)
#define nn_in_progress NN_CFG_NAME(nn_in_progress)
#define nn_abandon NN_CFG_NAME(nn_abandon)
#define nn_position_valid NN_CFG_NAME(nn_position_valid)
#define nn_acc_required NN_CFG_NAME(nn_acc_required)
#define nn_live NN_CFG_NAME(nn_live)
#define nn_live_bytes NN_CFG_NAME(nn_live_bytes)
#define nn_total_units NN_CFG_NAME(nn_total_units)
#define nn_units_done NN_CFG_NAME(nn_units_done)
#define nn_requant_policy NN_CFG_NAME(nn_requant_policy)
#define nn_weight_count NN_CFG_NAME(nn_weight_count)
#define nn_weight_bytes NN_CFG_NAME(nn_weight_bytes)
#define nn_weight NN_CFG_NAME(nn_weight)
#define nn_weights_packed NN_CFG_NAME(nn_weights_packed)
#define nn_weights_admissible NN_CFG_NAME(nn_weights_admissible)
#define nn_layer_path NN_CFG_NAME(nn_layer_path)
#define nn_path_name NN_CFG_NAME(nn_path_name)
#define nn_simd_is_cmax NN_CFG_NAME(nn_simd_is_cmax)
#define nn_simd_implementation NN_CFG_NAME(nn_simd_implementation)

#define ckpt_init NN_CFG_NAME(ckpt_init)
#define ckpt_save NN_CFG_NAME(ckpt_save)
#define ckpt_restore NN_CFG_NAME(ckpt_restore)
#define ckpt_peek NN_CFG_NAME(ckpt_peek)
#define ckpt_clear NN_CFG_NAME(ckpt_clear)
#define ckpt_cfg_seed NN_CFG_NAME(ckpt_cfg_seed)
#define ckpt_cfg_seed_bytes NN_CFG_NAME(ckpt_cfg_seed_bytes)
#define ckpt_acc_width NN_CFG_NAME(ckpt_acc_width)
#define ckpt_acc_bound NN_CFG_NAME(ckpt_acc_bound)
#define ckpt_acc_entries NN_CFG_NAME(ckpt_acc_entries)
#define ckpt_bytes NN_CFG_NAME(ckpt_bytes)
#define ckpt_write_us NN_CFG_NAME(ckpt_write_us)
#define ckpt_save_scan NN_CFG_NAME(ckpt_save_scan)
#define ckpt_restore_scan NN_CFG_NAME(ckpt_restore_scan)
#define ckpt_scan_pending NN_CFG_NAME(ckpt_scan_pending)
#define ckpt_scan_bytes NN_CFG_NAME(ckpt_scan_bytes)
#define ckpt_scan_write_us NN_CFG_NAME(ckpt_scan_write_us)
#define ckpt_scan_invalidate NN_CFG_NAME(ckpt_scan_invalidate)
#define ckpt_dev_init NN_CFG_NAME(ckpt_dev_init)
#define ckpt_dev_read NN_CFG_NAME(ckpt_dev_read)
#define ckpt_dev_write NN_CFG_NAME(ckpt_dev_write)
#define g_ckpt_restore_cleanup_failed NN_CFG_NAME(g_ckpt_restore_cleanup_failed)
#define g_ckpt_dev_programs NN_CFG_NAME(g_ckpt_dev_programs)
#define g_ckpt_dev_bytes NN_CFG_NAME(g_ckpt_dev_bytes)
#endif
