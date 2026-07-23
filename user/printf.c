#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#include <stdarg.h>

static char digits[] = "0123456789ABCDEF";

/** Write one character to fd. */
static void
putc(int fd, char c)
{
  write(fd, &c, 1);
}

/** Print an unsigned 64-bit integer in the requested base. */
static void
printuint64(int fd, uint64 x, int base)
{
  char buf[32];
  int i = 0;

  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);

  while(--i >= 0)
    putc(fd, buf[i]);
}

/** Print a signed or unsigned 32-bit integer. */
static void
printint(int fd, int xx, int base, int sgn)
{
  uint x;

  if(sgn && xx < 0){
    putc(fd, '-');
    x = -(uint)xx;
  } else {
    x = xx;
  }
  printuint64(fd, x, base);
}

/** Print a fixed-width pointer. */
static void
printptr(int fd, uint64 x)
{
  putc(fd, '0');
  putc(fd, 'x');
  for(int i = 0; i < sizeof(uint64) * 2; i++, x <<= 4)
    putc(fd, digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the given fd. Understands %d, %l, %x, %p, %s, %c, and %%.
void
vprintf(int fd, const char *fmt, va_list ap)
{
  char *s;
  int state = 0;

  for(int i = 0; fmt[i]; i++){
    int c = fmt[i] & 0xff;
    if(state == 0){
      if(c == '%')
        state = '%';
      else
        putc(fd, c);
    } else if(state == '%'){
      if(c == 'd')
        printint(fd, va_arg(ap, int), 10, 1);
      else if(c == 'l')
        printuint64(fd, va_arg(ap, uint64), 10);
      else if(c == 'x')
        printint(fd, va_arg(ap, int), 16, 0);
      else if(c == 'p')
        printptr(fd, va_arg(ap, uint64));
      else if(c == 's'){
        s = va_arg(ap, char*);
        if(s == 0)
          s = "(null)";
        while(*s != 0)
          putc(fd, *s++);
      } else if(c == 'c'){
        putc(fd, va_arg(ap, uint));
      } else if(c == '%'){
        putc(fd, c);
      } else {
        putc(fd, '%');
        putc(fd, c);
      }
      state = 0;
    }
  }
}

void
fprintf(int fd, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintf(fd, fmt, ap);
}

void
printf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintf(1, fmt, ap);
}
