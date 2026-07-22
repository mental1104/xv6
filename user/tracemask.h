#ifndef XV6_USER_TRACEMASK_H
#define XV6_USER_TRACEMASK_H

// 描述 trace 参数解析是否成功；解析失败时，具体状态用于指导调用方
// 向用户输出对应的错误信息。
enum trace_mask_status {
  TRACE_MASK_OK = 0,       // 解析成功，输出掩码有效。
  TRACE_MASK_EMPTY = -1,   // 输入字符串为空，或输出指针无效。
  TRACE_MASK_FORMAT = -2,  // 逗号分隔列表中出现空字段。
  TRACE_MASK_UNKNOWN = -3, // 字段不是已注册的系统调用名称。
  TRACE_MASK_RANGE = -4,   // 十进制掩码超出有符号 int 的取值范围。
};

/*
 * 将面向用户的 trace 参数转换为现有 trace 系统调用所需的整数掩码。
 *
 * 参数：
 *   spec：十进制掩码（如 "32"）、单个系统调用名称（如 "read"），
 *         或逗号分隔的名称列表（如 "read,write"）。
 *   mask：输出参数；仅在返回 TRACE_MASK_OK 时写入最终掩码。
 *
 * 返回值：
 *   返回 enum trace_mask_status 中的一个状态值。解析失败时不会修改
 *   *mask，便于调用方和测试识别意外产生的部分结果。
 */
int trace_parse_mask(const char *spec, int *mask);

#endif
