/**
 * @file console_cmds.h
 * @brief Settings commands on the USB-Serial-JTAG console (list/get/set/reset).
 */

#ifndef CONSOLE_CMDS_H
#define CONSOLE_CMDS_H

#ifdef __cplusplus
extern "C" {
#endif

/** Start the REPL task. Call once, after settings_init(). */
void console_cmds_start(void);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_CMDS_H */
