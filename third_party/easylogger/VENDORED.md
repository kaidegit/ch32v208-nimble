# Vendored EasyLogger

Upstream: https://github.com/armink/EasyLogger
Commit:   806328e131836662fa83dd43364a53f699fd76ac (master, v2.2.99-dev,
          2.2.0-63-g806328e), fetched 2026-10-30.
License:  MIT (see LICENSE; each source file also carries the notice).
Scope:    easylogger/{inc,src,port} only - demo/, docs/ and plugins/ are
          not vendored. Only src/elog.c and src/elog_utils.c are compiled
          (see CMakeLists.txt); elog_async.c (POSIX pthread) and elog_buf.c
          are unused: async decoupling is done by this project's own USART1
          DMA ring (port/console_uart.c).
Config:   elog_cfg.h is NOT taken from upstream; config/elog_cfg.h in this
          project shadows easylogger/inc/elog_cfg.h via include order.
Local modifications: NONE (files are byte-identical to the commit above).
