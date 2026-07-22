#ifndef XV6_CONSOLE_H
#define XV6_CONSOLE_H

/** 保持现有逐行读取、内核回显和行编辑语义。 */
#define CONSOLE_MODE_COOKED 0
/** 将 console 输入逐字节交给当前交互式 Shell，由用户态负责回显和编辑。 */
#define CONSOLE_MODE_RAW 1

#endif
