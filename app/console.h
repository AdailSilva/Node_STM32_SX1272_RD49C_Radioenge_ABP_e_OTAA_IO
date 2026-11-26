/*
 * console.h — console de comandos na UART do módulo (115200 8N1).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>

void ConsoleInit( void );
void ConsoleProcess( void );
bool ConsoleHasInput( void );

#endif /* CONSOLE_H */
