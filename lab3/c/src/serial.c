// Some notes on implementation:
//
// - Normally, memory barriers must be inserted at key positions, e.g. after
//   configuring the peripheral and before enabling it, to prevent memory access
//   reordering by the CPU to break the communication with the peripheral.
//   However, Cortex-A53 issues instructions in-order, so omitting the memory
//   barriers should be fine.
//
// - Due to a quirk in the AXI bus of the SoC (see [bcm2835-datasheet],
//   section 1.3, Peripheral access precautions for correct memory ordering (pp.
//   7)), memory barriers are required at certain positions to ensure the
//   correctness of peripheral accesses. In particular, it is required to put a
//   memory write barrier before the first write to a peripheral and a memory
//   read barrier after the last read of a peripheral. Note that consecutive
//   accesses to the same peripheral don't require a memory barrier in between.
//   There are two places where memory barriers may be placed: in the user code
//   and in this library. Putting the memory barriers in the user code may allow
//   for higher performance, since less memory barriers are required. For
//   example, if the user code calls serial_puts(const char *) twice without
//   accessing other peripherals in between, the user code may decide to only
//   put a write barrier before the first call and a read barrier after the
//   second call, but no memory barriers in between the calls. However, this
//   approach shifts the burden of ensuring correct memory ordering to the user
//   code, which may make the user code difficult to write. Therefore, for the
//   sake of ease of programming, the burden of ensuring correct memory ordering
//   is taken up by this library. Every public-facing function has the required
//   memory barriers. There are internal functions that bypass the memory
//   barriers, so that "composed I/O functions" don't need to issue excessive
//   memory barriers. For example, serial_fputs(const char *) doesn't call
//   serial_putc(char) but a similar internal function, so that it doesn't issue
//   a memory barrier before and after sending each character. See the
//   description on high-level I/O functions for more details.
//
// Some references that may be referenced later:
//
// [bcm2835-datasheet]:
//   https://datasheets.raspberrypi.com/bcm2835/bcm2835-peripherals.pdf
// [bcm2835-datasheet-errata]: https://elinux.org/BCM2835_datasheet_errata

#include "oscos/serial.h"

#include <stdbool.h>
#include <stddef.h>

#include "oscos/bcm2837/aux.h"
#include "oscos/bcm2837/gpio.h"
#include "oscos/bcm2837/l2ic.h"
#include "oscos/bcm2837/mini_uart.h"
#include "oscos/bcm2837/peripheral_memory_barrier.h"
#include "oscos/delay.h"
#include "oscos/spinlock.h"

#define READ_BUF_SZ 1024
#define WRITE_BUF_SZ 1024

static atomic_flag _serial_spinlock = ATOMIC_FLAG_INIT;
static SerialMode _serial_mode = SM_TEXT;
static volatile char _serial_read_buf[READ_BUF_SZ];
static volatile size_t _serial_read_buf_start = 0, _serial_read_buf_len = 0;
static volatile char _serial_write_buf[WRITE_BUF_SZ];
static volatile size_t _serial_write_buf_start = 0, _serial_write_buf_len = 0;
// Whether or not the current execution context is in one of the synchronous I/O
// functions. This variable is meant to save a few memory barriers.
static volatile bool _serial_is_in_sync = false;

void serial_init(void) {
  // The initialization procedure is taken from
  // https://oscapstone.github.io/labs/hardware/uart.html.

  PERIPHERAL_WRITE_BARRIER();

  // Set GPIO pin 14 & 15 to use alternate function 5 ({T,R}XD1).
  GP->FSEL[1] = (GP->FSEL[1] & ~(GPFSEL_FSEL4_MASK | GPFSEL_FSEL5_MASK)) |
                (GPFSEL_FSEL4_ALT5 | GPFSEL_FSEL5_ALT5);

  // Disable the GPIO pull up/down on pin 14 (TXD1).
  //
  // We leave the pull down enabled on pin 15 (RXD1) (note: this is the default)
  // to ensure that mini UART doesn't read in garbage data when the pin is not
  // connected.
  //
  // The delay period of 1 μs is calculated by dividing the required delay
  // period of 150 clock cycles (as specified in [bcm2835-datasheet]) by 150
  // MHz, the nominal core frequency mentioned in [bcm2835-datasheet], pp. 34.
  // We believe 150 MHz should be used instead of the actual core frequency of
  // 250 MHz because the setup/hold time of a digital circuit is usually
  // specified in terms of real time (e.g. nanoseconds) instead of in clock
  // cycles. The specified delay period of 150 clock cycles might have been
  // derived by multiplying the actual required delay period of 1 μs by the
  // nominal core frequency of 150 MHz.

  GP->PUD = GPPUD_PUD_OFF;
  delay_ns(1000);
  GP->PUDCLK[0] = 1 << 14;
  delay_ns(1000);
  GP->PUD = 0;
  GP->PUDCLK[0] = 0;

  PERIPHERAL_READ_BARRIER();

  PERIPHERAL_WRITE_BARRIER();

  // Enable mini UART.
  AUX->ENB |= AUXENB_MINI_UART_ENABLE;
  // Disable TX and RX.
  AUX_MU->CNTL_REG = 0;
  // Disable interrupt.
  AUX_MU->IER_REG = 0;
  // Set the data size to 8 bits.
  // N. B. The datasheet [bcm2835-datasheet] incorrectly indicates that only bit
  // 0 needs to be set. In fact, bits [1:0] need to be set to 3. See
  // [bcm2835-datasheet-errata], #p14.
  AUX_MU->LCR_REG = 3;
  // Disable auto flow control.
  AUX_MU->MCR_REG = 0;
  // Set baud rate to 115200.
  AUX_MU->BAUD = 270;
  // Clear the transmit and receive FIFOs.
  AUX_MU->IIR_REG = 6;
  // Enable TX and RX.
  AUX_MU->CNTL_REG = 3;

  // Workaround for the issue of the serial console reading in an extraneous
  // 0xf8 upon bootstrapping. Delay for the time to receive 2 characters (i.e.,
  // 20 bauds) and clear the FIFOs. The delay period is determined by heuristic
  // and not backed by exact science.
  delay_ns(20 * NS_PER_SEC / 115200);
  AUX_MU->IIR_REG = 6;

  // Enable receive interrupt.
  AUX_MU->IER_REG |= AUX_MU_IER_REG_ENABLE_RECEIVE_INTERRUPT;

  PERIPHERAL_READ_BARRIER();

  PERIPHERAL_WRITE_BARRIER();

  // Enable AUX interrupt on the L2 interrupt controller.
  L2IC->enable_irqs[0] |= INT_SRC_L2_AUX;

  PERIPHERAL_READ_BARRIER();
}

static void _serial_flush(void) {
  while (_serial_write_buf_len != 0) {
    __asm__ __volatile__("wfi");
  }
  while (!(AUX_MU->LSR_REG & AUX_MU_LSR_REG_TRANSMITTER_IDLE))
    ;
}

void serial_deinit(void) {
  PERIPHERAL_WRITE_BARRIER();

  _serial_flush();

  // Disable TX and RX.
  AUX_MU->CNTL_REG = 0;
  // Clear the transmit and receive FIFOs.
  AUX_MU->IIR_REG = 6;
  // Reset registers.
  AUX_MU->IER_REG = 0;
  AUX_MU->LCR_REG = 0;
  AUX_MU->MCR_REG = 0;
  AUX_MU->BAUD = 0;
  // Disable mini UART.
  AUX->ENB &= ~AUXENB_MINI_UART_ENABLE;

  PERIPHERAL_READ_BARRIER();

  PERIPHERAL_WRITE_BARRIER();

  // Set GPIO pin 14 & 15 back to input mode.
  GP->FSEL[1] = (GP->FSEL[1] & ~(GPFSEL_FSEL4_MASK | GPFSEL_FSEL5_MASK)) |
                (GPFSEL_FSEL4_INPUT | GPFSEL_FSEL5_INPUT);

  // Re-enable the GPIO pull down on pin 14 (TXD1).
  // See the comments in serial_init(void) for notes on the delay period.

  GP->PUD = GPPUD_PUD_PULL_DOWN;
  delay_ns(1000);
  GP->PUDCLK[0] = 1 << 14;
  delay_ns(1000);
  GP->PUD = 0;
  GP->PUDCLK[0] = 0;

  PERIPHERAL_READ_BARRIER();
}

void serial_lock(void) { SPIN_LOCK(&_serial_spinlock); }

void serial_unlock(void) { SPIN_UNLOCK(&_serial_spinlock); }

void serial_set_mode(const SerialMode mode) {
  // ? Should we check the value of `mode`?
  // * FIXME: If the check is implemented, update the documentation in
  // * `include/oscos/serial.h`.
  _serial_mode = mode;
}

void serial_flush(void) {
  _serial_flush();
  PERIPHERAL_READ_BARRIER();
}

void mini_uart_irq_handler(void) {
  if (!_serial_is_in_sync) {
    PERIPHERAL_WRITE_BARRIER();
  }

  // Read.

  // Read until the read buffer is full or there are nothing to read.
  while (_serial_read_buf_len != READ_BUF_SZ &&
         (AUX_MU->LSR_REG & AUX_MU_LSR_REG_DATA_READY)) {
    _serial_read_buf[(_serial_read_buf_start + _serial_read_buf_len++) %
                     READ_BUF_SZ] = AUX_MU->IO_REG;
  }

  // Disable the receive interrupt if the read buffer is full. Otherwise, the
  // interrupt will fire again and again after exception return, blocking the
  // main code from executing.
  if (_serial_read_buf_len == READ_BUF_SZ) {
    AUX_MU->IER_REG &= ~AUX_MU_IER_REG_ENABLE_RECEIVE_INTERRUPT;
  }

  // Write.

  while (_serial_write_buf_len != 0 &&
         (AUX_MU->LSR_REG & AUX_MU_LSR_REG_TRANSMITTER_EMPTY)) {
    AUX_MU->IO_REG = _serial_write_buf[_serial_write_buf_start];
    _serial_write_buf_start = (_serial_write_buf_start + 1) % WRITE_BUF_SZ;
    _serial_write_buf_len--;
  }

  // Disable the transmit interrupt if the write buffer is full. Otherwise, the
  // interrupt will fire again and again after exception return, blocking the
  // main code from executing.
  if (_serial_write_buf_len == 0) {
    AUX_MU->IER_REG &= ~AUX_MU_IER_REG_ENABLE_TRANSMIT_INTERRUPT;
  }

  if (!_serial_is_in_sync) {
    PERIPHERAL_READ_BARRIER();
  }
}

// Raw I/O functions.

static int _serial_read_byte_nonblock(void) {
  if (_serial_read_buf_len == 0) {
    return -1;
  }

  // Enter critical section; disable receive interrupt.
  AUX_MU->IER_REG &= ~AUX_MU_IER_REG_ENABLE_RECEIVE_INTERRUPT;

  const char result = _serial_read_buf[_serial_read_buf_start];
  _serial_read_buf_start = (_serial_read_buf_start + 1) % READ_BUF_SZ;
  _serial_read_buf_len--;

  // Exit critical section; re-enable receive interrupt.
  // Note that the receive interrupt might already have been disabled before
  // entering this function due to the read buffer being full. It's safe to
  // enable the receive interrupt anyway, because there is at least one space in
  // the read buffer.
  AUX_MU->IER_REG |= AUX_MU_IER_REG_ENABLE_RECEIVE_INTERRUPT;

  return result;
}

static char _serial_read_byte(void) {
  int result;
  while ((result = _serial_read_byte_nonblock()) < 0) {
    __asm__ __volatile__("wfi");
  }

  return result;
}

static void _serial_write_byte(const char c) {
  // Wait for the write buffer to clear.
  while (_serial_write_buf_len == WRITE_BUF_SZ) {
    __asm__ __volatile__("wfi");
  }

  // Enter critical section; disable transmit interrupt.
  AUX_MU->IER_REG &= ~AUX_MU_IER_REG_ENABLE_TRANSMIT_INTERRUPT;

  _serial_write_buf[(_serial_write_buf_start + _serial_write_buf_len++) %
                    WRITE_BUF_SZ] = c;

  // Exit critical section; re-enable transmit interrupt.
  // Note that the transmit interrupt might already have been disabled before
  // entering this function due to the write buffer being empty. It's safe to
  // enable the transmit interrupt anyway, because there is at least one
  // character in the write buffer.
  AUX_MU->IER_REG |= AUX_MU_IER_REG_ENABLE_TRANSMIT_INTERRUPT;
}

// High-level I/O functions.
//
// High-level I/O functions are named after the counterparts in the `stdio.h`
// header file. Each of the family of high-level I/O functions consist of four
// functions: `_serial_abc_text_mode`, `_serial_abc_binary_mode`, `_serial_abc`,
// and `serial_abc`. Only the last one of them is public-facing. The
// `_serial_abc_text_mode` function contains the implementation used in text
// mode, while the `_serial_abc_binary_mode` function contains the
// implementation used in binary mode. The `_serial_abc` function calls either
// `_serial_abc_text_mode` or `_serial_abc_binary_mode` depending on the current
// mode setting. The `serial_abc` function issues the memory barriers and calls
// `_serial_abc`.
//
// Some high-level I/O functions are "composed". I.e., they are implemented by
// combining other I/O functions. The easiest example is
// serial_fputs(const char *), which, conceptually, repeatedly calls
// serial_putc(char) to send each character. However, if
// serial_fputs(const char *) is actually implemented in this way, then it will
// unnecessarily issue memory barriers and check the mode for each character in
// the string. (The latter may be optimized away by the compiler, but the former
// cannot.) In the actual implementation, the `serial_fputs` family has its own
// memory barrier in serial_fputs(const char *) and mode switch in
// _serial_fputs(const char *), while _serial_fputs_text_mode(const char *)
// calls _serial_putc_text_mode(char) and
// _serial_fputs_binary_mode(const char *) calls _serial_putc_binary_mode(char).
// This way, memory barriers are issued only once (one memory write barrier and
// one memory read barrier) and the mode is only checked once.
//
// (In retrospect, this sort of performance gain may not matter after all, since
// sending/receiving a single character already takes *thousands* of clock
// cycles to complete.)
//
// Since the definitions of the `_serial_abc` function and the `serial_abc`
// function across families are almost identical, we provide macros to generate
// the definitions of all of these functions. Given a specification of the
// function signature for all functions in the family, the
// #FN_DECL_TEXT_MODE(SPEC) macro generates the function signature of the
// `_serial_abc_text_mode` function, which can be used to define the said
// function. The same goes for the #FN_DECL_BINARY_MODE(SPEC) macro and the
// `_serial_abc_binary_mode` function. The #FN_DEFN(SPEC) function generates the
// definitions of both the `_serial_abc` function and the `serial_abc` function.
// See the definition of serial_putc(char) for an example.
//
// Furthermore, for "composed I/O functions", since the definition of the
// `_serial_abc_text_mode` function and the `_serial_abc_binary_mode` function
// within the same family are almost identical, we provide macros to generate
// the definitions of both functions from a common template. Given a
// specification of the function signature of all functions in the family and
// the said template, the #FN_DEFN_COMPOSED(SPEC, IMPL) macro generates the
// definition of every function in the family. See the definition of
// serial_fputs(const char *) for an example.

// Function generators.

#define NOTHING
#define NOTHING1(X)
#define ID(X) X
#define COMMA ,
#define CONST_RETURN(X) return
#define CONST_RETURN_RESULT(X) return result;

#define RESULT_DECL_NON_VOID(T) const T result =

#define ARG_DECL(ARG_TY, ARG_NAME_) ARG_TY ARG_NAME_
#define ARG_NAME(ARG_TY, ARG_NAME_) ARG_NAME_

#define RET_TY_NOMINAL(RET_TY_, NAME, ARGS) RET_TY_(ID, void)
#define RETURN_EXPR_IF_NON_VOID(RET_TY_, NAME, ARGS)                           \
  RET_TY_(CONST_RETURN, NOTHING)
#define RETURN_IF_VOID(RET_TY_, NAME, ARGS) RET_TY_(NOTHING1, return;)
#define DECL_RESULT_IF_NON_VOID(RET_TY_, NAME, ARGS)                           \
  RET_TY_(RESULT_DECL_NON_VOID, NOTHING)
#define RETURN_RESULT_IF_NON_VOID(RET_TY_, NAME, ARGS)                         \
  RET_TY_(CONST_RETURN_RESULT, NOTHING)

#define NAME_TEXT_MODE(RET_TY_, NAME, ARGS) _serial_##NAME##_text_mode
#define NAME_BINARY_MODE(RET_TY_, NAME, ARGS) _serial_##NAME##_binary_mode
#define NAME_PRIVATE(RET_TY_, NAME, ARGS) _serial_##NAME
#define NAME_PUBLIC(RET_TY_, NAME, ARGS) serial_##NAME

#define ARGS_DECL(RET_TY_, NAME, ARGS) ARGS(ARG_DECL, COMMA)
#define ARGS_INVOKE(RET_TY_, NAME, ARGS) ARGS(ARG_NAME, COMMA)

#define FN_DECL(SPEC, QUALIFIERS, NAME)                                        \
  QUALIFIERS SPEC(RET_TY_NOMINAL) SPEC(NAME)(SPEC(ARGS_DECL))
#define FN_DECL_TEXT_MODE(SPEC) FN_DECL(SPEC, static, NAME_TEXT_MODE)
#define FN_DECL_BINARY_MODE(SPEC) FN_DECL(SPEC, static, NAME_BINARY_MODE)
#define FN_DECL_PRIVATE(SPEC) FN_DECL(SPEC, static, NAME_PRIVATE)
#define FN_DECL_PUBLIC(SPEC) FN_DECL(SPEC, NOTHING, NAME_PUBLIC)

#define INVOKE(SPEC, NAME) SPEC(NAME)(SPEC(ARGS_INVOKE))
#define INVOKE_PRIVATE(SPEC) INVOKE(SPEC, NAME_PRIVATE)

#define RETURN_INVOKE(SPEC, NAME)                                              \
  SPEC(RETURN_EXPR_IF_NON_VOID) INVOKE(SPEC, NAME);                            \
  SPEC(RETURN_IF_VOID)
#define RETURN_INVOKE_TEXT_MODE(SPEC) RETURN_INVOKE(SPEC, NAME_TEXT_MODE)
#define RETURN_INVOKE_BINARY_MODE(SPEC) RETURN_INVOKE(SPEC, NAME_BINARY_MODE)

#define FN_DEFN_PRIVATE(SPEC)                                                  \
  FN_DECL_PRIVATE(SPEC) {                                                      \
    switch (_serial_mode) {                                                    \
    case SM_TEXT:                                                              \
      RETURN_INVOKE_TEXT_MODE(SPEC);                                           \
                                                                               \
    case SM_BINARY:                                                            \
      RETURN_INVOKE_BINARY_MODE(SPEC);                                         \
                                                                               \
    default:                                                                   \
      /* Unreachable. The mode can only be set via the                         \
         serial_set_mode(SerialMode) function, and it is assumed that the      \
         supplied mode is valid. */                                            \
      __builtin_unreachable();                                                 \
    }                                                                          \
  }

#define FN_DEFN_PUBLIC(SPEC)                                                   \
  FN_DECL_PUBLIC(SPEC) {                                                       \
    PERIPHERAL_WRITE_BARRIER();                                                \
    _serial_is_in_sync = true;                                                 \
    SPEC(DECL_RESULT_IF_NON_VOID) INVOKE_PRIVATE(SPEC);                        \
    PERIPHERAL_READ_BARRIER();                                                 \
    _serial_is_in_sync = false;                                                \
    SPEC(RETURN_RESULT_IF_NON_VOID)                                            \
  }

#define FN_DEFN(SPEC) FN_DEFN_PRIVATE(SPEC) FN_DEFN_PUBLIC(SPEC)

#define FN_DEFN_COMPOSED_TEXT_MODE(SPEC, IMPL)                                 \
  FN_DECL_TEXT_MODE(SPEC) IMPL(text_mode)
#define FN_DEFN_COMPOSED_BINARY_MODE(SPEC, IMPL)                               \
  FN_DECL_BINARY_MODE(SPEC) IMPL(binary_mode)
#define FN_DEFN_COMPOSED(SPEC, IMPL)                                           \
  FN_DEFN_COMPOSED_TEXT_MODE(SPEC, IMPL)                                       \
  FN_DEFN_COMPOSED_BINARY_MODE(SPEC, IMPL) FN_DEFN(SPEC)

// Definitions of high-level I/O functions.

// char getc(void)

#define GETC_RET_TY(X, V) X(char)
#define GETC_ARGS(X, D) X(void, NOTHING)
#define GETC_SPEC(X) X(GETC_RET_TY, getc, GETC_ARGS)

FN_DECL_TEXT_MODE(GETC_SPEC) {
  const char c = _serial_read_byte();
  return c == '\r' ? '\n' : c;
}

FN_DECL_BINARY_MODE(GETC_SPEC) { return _serial_read_byte(); }

FN_DEFN(GETC_SPEC)

// int getc_nonblock(void)

#define GETC_NONBLOCK_RET_TY(X, V) X(int)
#define GETC_NONBLOCK_ARGS(X, D) X(void, NOTHING)
#define GETC_NONBLOCK_SPEC(X)                                                  \
  X(GETC_NONBLOCK_RET_TY, getc_nonblock, GETC_NONBLOCK_ARGS)

FN_DECL_TEXT_MODE(GETC_NONBLOCK_SPEC) {
  const int c = _serial_read_byte_nonblock();
  if (c < 0)
    return c;
  return c == '\r' ? '\n' : c;
}

FN_DECL_BINARY_MODE(GETC_NONBLOCK_SPEC) { return _serial_read_byte_nonblock(); }

FN_DEFN(GETC_NONBLOCK_SPEC)

// char putc(char c)

#define PUTC_RET_TY(X, V) X(char)
#define PUTC_ARGS(X, D) X(const char, c)
#define PUTC_SPEC(X) X(PUTC_RET_TY, putc, PUTC_ARGS)

FN_DECL_TEXT_MODE(PUTC_SPEC) {
  if (c == '\n') {
    _serial_write_byte('\r');
    _serial_write_byte('\n');
  } else {
    _serial_write_byte(c);
  }

  return c;
}

FN_DECL_BINARY_MODE(PUTC_SPEC) {
  _serial_write_byte(c);
  return c;
}

FN_DEFN(PUTC_SPEC)

// void write(const char *buf, size_t count)

#define WRITE_RET_TY(X, V) V
#define WRITE_ARGS(X, D) X(const char *const, buf) D X(const size_t, count)
#define WRITE_SPEC(X) X(WRITE_RET_TY, write, WRITE_ARGS)

#define WRITE_IMPL(MODE)                                                       \
  {                                                                            \
    for (size_t i = 0; i < count; i++) {                                       \
      _serial_putc_##MODE(buf[i]);                                             \
    }                                                                          \
  }

FN_DEFN_COMPOSED(WRITE_SPEC, WRITE_IMPL)

// void fputs(const char *s)

#define FPUTS_RET_TY(X, V) V
#define FPUTS_ARGS(X, D) X(const char *const, s)
#define FPUTS_SPEC(X) X(FPUTS_RET_TY, fputs, FPUTS_ARGS)

#define FPUTS_IMPL(MODE)                                                       \
  {                                                                            \
    for (const char *c = s; *c; c++) {                                         \
      _serial_putc_##MODE(*c);                                                 \
    }                                                                          \
  }

FN_DEFN_COMPOSED(FPUTS_SPEC, FPUTS_IMPL)

// void puts(const char *s)

#define PUTS_RET_TY(X, V) V
#define PUTS_ARGS(X, D) X(const char *const, s)
#define PUTS_SPEC(X) X(PUTS_RET_TY, puts, PUTS_ARGS)

#define PUTS_IMPL(MODE)                                                        \
  {                                                                            \
    _serial_fputs_##MODE(s);                                                   \
    _serial_putc_##MODE('\n');                                                 \
  }

FN_DEFN_COMPOSED(PUTS_SPEC, PUTS_IMPL)

// void serial_print_hex(uint32_t x)

#define PRINT_HEX_RET_TY(X, V) V
#define PRINT_HEX_ARGS(X, D) X(const uint32_t, x)
#define PRINT_HEX_SPEC(X) X(PRINT_HEX_RET_TY, print_hex, PRINT_HEX_ARGS)

#define PRINT_HEX_IMPL(MODE)                                                   \
  {                                                                            \
    static const char HEX_DIGITS[16] = "0123456789abcdef";                     \
                                                                               \
    for (int shamt = 28; shamt >= 0; shamt -= 4) {                             \
      const uint32_t digit = (x >> shamt) & 0xf;                               \
      _serial_putc_##MODE(HEX_DIGITS[digit]);                                  \
    }                                                                          \
  }

FN_DEFN_COMPOSED(PRINT_HEX_SPEC, PRINT_HEX_IMPL)

// void serial_print_hex_u64(uint64_t x)

#define PRINT_HEX_U64_RET_TY(X, V) V
#define PRINT_HEX_U64_ARGS(X, D) X(const uint64_t, x)
#define PRINT_HEX_U64_SPEC(X)                                                  \
  X(PRINT_HEX_U64_RET_TY, print_hex_u64, PRINT_HEX_U64_ARGS)

#define PRINT_HEX_U64_IMPL(MODE)                                               \
  {                                                                            \
    static const char HEX_DIGITS[16] = "0123456789abcdef";                     \
                                                                               \
    for (int shamt = 60; shamt >= 0; shamt -= 4) {                             \
      const uint32_t digit = (x >> shamt) & 0xf;                               \
      _serial_putc_##MODE(HEX_DIGITS[digit]);                                  \
    }                                                                          \
  }

FN_DEFN_COMPOSED(PRINT_HEX_U64_SPEC, PRINT_HEX_U64_IMPL)
