#include "kernel/syscall.h"
#include "kernel/syscall_names.h"
#include "user/tracemask.h"

// trace(int) 接收有符号 int，因此十进制掩码最大只能取 INT_MAX。
#define TRACE_MASK_INT_MAX 2147483647

/*
 * 判断 spec 是否为非空的纯十进制数字字符串。
 *
 * 参数：
 *   spec：待分类的 trace 参数字符串。
 *
 * 返回值：
 *   字符串非空且只包含十进制数字时返回非零，否则返回 0。
 */
static int
is_decimal(const char *spec)
{
  int index; // 当前检查的字符在 spec 中的下标。

  if(spec[0] == 0)
    return 0;

  for(index = 0; spec[index] != 0; index++){
    if(spec[index] < '0' || spec[index] > '9')
      return 0;
  }
  return 1;
}

/*
 * 将纯十进制字符串解析为整数掩码，并在乘法和加法发生前检查溢出。
 *
 * 参数：
 *   spec：已经由 is_decimal() 验证过的非空十进制字符串。
 *   mask：输出参数；解析成功时接收最终整数掩码。
 *
 * 返回值：
 *   成功返回 TRACE_MASK_OK；超出有符号 int 范围时返回 TRACE_MASK_RANGE。
 */
static int
parse_decimal(const char *spec, int *mask)
{
  int index;     // 当前处理的数字字符在 spec 中的下标。
  int value = 0; // 已处理前缀转换得到的十进制累计值。

  for(index = 0; spec[index] != 0; index++){
    int digit = spec[index] - '0'; // 当前字符对应的数值。

    // 将 value * 10 + digit <= INT_MAX 变形后判断，避免检查过程本身溢出。
    if(value > (TRACE_MASK_INT_MAX - digit) / 10)
      return TRACE_MASK_RANGE;
    value = value * 10 + digit;
  }

  *mask = value;
  return TRACE_MASK_OK;
}

/*
 * 将名称表中的完整系统调用名称，与用户参数中的一个定长字段进行比较。
 *
 * 参数：
 *   name：来自 syscall_names[]、以 NUL 结尾的标准系统调用名称。
 *   start：用户参数中待比较字段的首字符地址。
 *   length：字段长度，不包含逗号分隔符。
 *
 * 返回值：
 *   两者长度和内容完全一致时返回非零，否则返回 0。
 */
static int
name_matches(const char *name, const char *start, int length)
{
  int index; // 当前同时比较两个字符串的字符下标。

  for(index = 0; index < length; index++){
    if(name[index] == 0 || name[index] != start[index])
      return 0;
  }
  return name[length] == 0;
}

/*
 * 根据用户参数中的一个名称字段查找对应的系统调用编号。
 *
 * 参数：
 *   start：原始 trace 参数中当前名称字段的首字符地址。
 *   length：当前名称字段的字符数量。
 *
 * 返回值：
 *   查找成功时返回正数 SYS_* 编号；名称未知时返回 -1。
 */
static int
lookup_syscall(const char *start, int length)
{
  int syscall_number; // 当前在共享 syscall_names 名称表中尝试匹配的编号。

  for(syscall_number = 1;
      syscall_number < (int)SYSCALL_NAME_COUNT;
      syscall_number++){
    if(syscall_names[syscall_number] &&
       name_matches(syscall_names[syscall_number], start, length))
      return syscall_number;
  }
  return -1;
}

/*
 * 将十进制掩码或逗号分隔的系统调用名称列表，转换为现有 trace(int)
 * 系统调用所使用的整数位掩码。
 *
 * 参数：
 *   spec：十进制掩码、单个系统调用名称，或逗号分隔的名称列表。
 *   mask：输出参数；只有完整输入全部验证通过后才写入最终掩码。
 *
 * 返回值：
 *   返回 enum trace_mask_status 中的对应状态值。
 */
int
trace_parse_mask(const char *spec, int *mask)
{
  const char *field_start; // 当前正在解析字段的首字符地址。
  const char *cursor;      // 当前检查是否为逗号或字符串结尾的字符地址。
  unsigned int result = 0; // 所有已验证名称通过按位或组合得到的累计掩码。

  if(spec == 0 || mask == 0 || spec[0] == 0)
    return TRACE_MASK_EMPTY;

  // 当全部字符都是数字时保留原 Lab2 的整数接口；否则按系统调用名称解析。
  if(is_decimal(spec))
    return parse_decimal(spec, mask);

  field_start = spec;
  for(cursor = spec; ; cursor++){
    if(*cursor == ',' || *cursor == 0){
      int field_length = cursor - field_start; // 刚结束字段的字符数量。
      int syscall_number; // 当前字段匹配得到的 SYS_* 编号。

      if(field_length == 0)
        return TRACE_MASK_FORMAT;

      syscall_number = lookup_syscall(field_start, field_length);
      if(syscall_number < 0)
        return TRACE_MASK_UNKNOWN;

      // 重复名称天然幂等：对已经置位的比特再次按位或不会改变累计掩码。
      result |= 1U << syscall_number;
      if(*cursor == 0)
        break;
      field_start = cursor + 1;
    }
  }

  *mask = result;
  return TRACE_MASK_OK;
}