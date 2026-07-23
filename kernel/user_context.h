#ifndef XV6_USER_CONTEXT_H
#define XV6_USER_CONTEXT_H

#define USER_CONTEXT_GPR_COUNT 31
#define USER_CONTEXT_RA_INDEX 0
#define USER_CONTEXT_SP_INDEX 1
#define USER_CONTEXT_A0_INDEX 9

/**
 * 保存一次用户态执行现场。
 *
 * gpr[] 按 trapframe 中 ra 到 t6 的连续顺序对应 RISC-V x1 到 x31。内核与
 * 用户态调度器共享该布局；修改顺序时必须同步检查 save_user_context()、
 * restore_user_context() 和首次线程上下文初始化逻辑。
 */
struct user_context {
  uint64 epc;
  uint64 gpr[USER_CONTEXT_GPR_COUNT];
};

#endif
