#include "oscos/shell.h"

#include <stdnoreturn.h>

#include "oscos/libc/string.h"
#include "oscos/reset.h"
#include "oscos/serial.h"

#define MAX_CMD_LEN 78

static void _shell_print_prompt(void) { serial_fputs("# "); }

static size_t _shell_read_cmd(char *const buf, const size_t n) {
  size_t cmd_len = 0;

  for (;;) {
    const char c = serial_getc();
    serial_putc(c);

    if (c == '\n')
      break;

    if (cmd_len < n - 1) {
      buf[cmd_len++] = c;
    }
  }

  buf[cmd_len] = '\0';
  return cmd_len;
}

static void _shell_do_cmd_help(void) {
  serial_puts("help   : print this help menu\n"
              "hello  : print Hello World!\n"
              "reboot : reboot the device");
}

static void _shell_do_cmd_hello(void) { serial_puts("Hello World!"); }

noreturn static void _shell_do_cmd_reboot(void) { reboot(); }

static void _shell_cmd_not_found(const char *const cmd) {
  serial_fputs("oscsh: ");
  serial_fputs(cmd);
  serial_puts(": command not found");
}

void run_shell(void) {
  for (;;) {
    _shell_print_prompt();

    char cmd_buf[MAX_CMD_LEN + 1];
    _shell_read_cmd(cmd_buf, MAX_CMD_LEN + 1);

    if (strcmp(cmd_buf, "help") == 0) {
      _shell_do_cmd_help();
    } else if (strcmp(cmd_buf, "hello") == 0) {
      _shell_do_cmd_hello();
    } else if (strcmp(cmd_buf, "reboot") == 0) {
      _shell_do_cmd_reboot();
    } else {
      _shell_cmd_not_found(cmd_buf);
    }
  }
}
