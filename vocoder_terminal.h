// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

// ============================================================================
// Terminal Raw Mode
// ============================================================================

static struct termios gOrigTermios;
static bool gTermiosRestored = true;
static volatile sig_atomic_t gTerminalResized = 0;

static void sigwinchHandler(int /*sig*/) {
    gTerminalResized = 1;
}

static void restoreTerminal() {
    if (!gTermiosRestored) {
        tcsetattr(STDIN_FILENO, TCSANOW, &gOrigTermios);
        gTermiosRestored = true;
        // Show cursor, reset colors — write() is async-signal-safe
        static const char kResetSeq[] = "\033[?25h\033[0m\n";
        write(STDOUT_FILENO, kResetSeq, sizeof(kResetSeq) - 1);
    }
}

static void signalHandler(int /*sig*/) {
    restoreTerminal();
    _exit(0);
}

static bool enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &gOrigTermios) < 0) {
        fprintf(stderr, "ERROR: Cannot get terminal attributes\n");
        return false;
    }

    struct termios raw = gOrigTermios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        fprintf(stderr, "ERROR: Cannot set terminal raw mode\n");
        return false;
    }

    gTermiosRestored = false;

    // Install signal handlers for clean exit and terminal resize
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGWINCH, sigwinchHandler);

    // Hide cursor
    printf("\033[?25l");
    fflush(stdout);

    return true;
}

// ============================================================================
// Keyboard Input (non-blocking) — returns raw char, dispatched per-page
// ============================================================================

static constexpr int kKeyUp    = 256;
static constexpr int kKeyDown  = 257;
static constexpr int kKeyRight = 258;
static constexpr int kKeyLeft  = 259;

static int readRawKey() {
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;

    // Check for escape sequences
    if (c == '\033') {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return 27; // Bare Esc
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return 27;
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return kKeyUp;    // Up arrow
                case 'B': return kKeyDown;  // Down arrow
                case 'C': return kKeyRight; // Right arrow
                case 'D': return kKeyLeft;  // Left arrow
                default: break;
            }
        }
        return 0;
    }

    return static_cast<int>(static_cast<unsigned char>(c));
}
