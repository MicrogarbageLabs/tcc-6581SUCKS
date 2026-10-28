/*
 *  test.h -- what a compiler test includes.
 *
 *  A test is a main() that computes things and CHECKs them; run816 runs it
 *  and passes it if every CHECK matched. Keep inputs in volatile globals
 *  (V16, V32 below), or the compiler folds the whole check at compile
 *  time and tests nothing.
 *
 *    CHECK(expr, want)    expr and want as 32-bit values; a mismatch prints
 *                         the line, what it got and what it wanted
 *    t_puts / t_putc      print (the .expect file, when there is one, is
 *                         compared with all that is printed)
 *    t_timer_start / t_timer_stop(id)   print the cycles between them
 *
 *  Public domain (CC0). No warranty.
 */
#ifndef TEST_H
#define TEST_H

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned long long u32;
typedef signed long long s32;

void t_check(u16 id, u32 got, u32 want);
void t_putc(u16 ch);
void t_puts(const char *s);
void t_timer_start(void);
void t_timer_stop(u16 id);
void t_fail(u16 code);
void t_done(void);

#define CHECK(expr, want) t_check(__LINE__, (u32) (expr), (u32) (want))

#endif
