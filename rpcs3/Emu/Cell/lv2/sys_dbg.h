#pragma once

/* Data structure for SPU thread context */
typedef struct {
  u128	gpr[128];		/* GPR[0-127]*/
  u32		npc;				/* Next Program Counter */
  u128	fpscr;				/* FPSCR */
  u32		srr0;				/* SRR0 */
  u32		spu_status;			/* SPU STATUS */
  u64		spu_cfg;			/* SPU CFG */
  u32		mb_stat;			/* MB_stat */
  u32		ppu_mb;				/* PPU Mail Box */
  u32		spu_mb[4];		/* SPU Mail Box */
  u32	decrementer;			/* Decrementer */
  //u64		mfc_cq_sr[16];	/* MFC Command Queue Save/Restore Register */
} sys_dbg_spu_thread_context2_t;

void sys_dbg_get_console_type(vm::ps3::ptr<u64> Console_Type);
error_code sys_dbg_read_spu_thread_context2(u32 id, vm::ps3::ptr<sys_dbg_spu_thread_context2_t> spu_context);
