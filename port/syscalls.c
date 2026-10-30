/*
 * port/syscalls.c - newlib-nano syscall stubs for the CH32V208 bring-up.
 *
 *  - console I/O goes through the project log backend (USART1 DMA ring,
 *    port/console_uart.c): _write -> log ring, _read -> EOF (PA10 is wired
 *    as USART1 RX but nothing consumes it - no shell by design)
 *  - _sbrk grows the newlib heap between the ld/Link.ld symbols
 *    _heap_start / _heap_end (located above .bss, below the 2 KB
 *    reset stack)
 *  - heap locking: newlib's malloc can be re-entered from different
 *    FreeRTOS tasks, so __malloc_lock/__malloc_unlock suspend/resume
 *    the scheduler.  (Interrupt-context allocation is not expected;
 *    NimBLE allocates from tasks only.)
 */

#include <sys/stat.h>
#include <sys/reent.h>
#include <sys/time.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"
#include "wch_log.h"

#undef errno
extern int errno;

int _kill( int pid, int sig );
int _getpid( void );

/* Heap boundaries from ld/Link.ld. */
extern char _heap_start[];
extern char _heap_end[];

static char * heap_ptr = NULL;

/*--------------------------------------------------------------------*/
/* Memory                                                             */
/*--------------------------------------------------------------------*/

void * _sbrk( ptrdiff_t incr )
{
    char * prev;
    char * next;

    if( heap_ptr == NULL )
    {
        heap_ptr = _heap_start;
    }

    prev = heap_ptr;
    /* 8-byte alignment keeps ABI-friendly allocations. */
    next = ( char * ) ( ( ( uintptr_t ) prev + incr + 7u ) & ~( uintptr_t ) 7u );

    if( next > _heap_end )   /* also catches overflow of the addition */
    {
        errno = ENOMEM;
        return ( void * ) -1;
    }

    heap_ptr = next;
    return prev;
}

void __malloc_lock( struct _reent * r )
{
    ( void ) r;
    vTaskSuspendAll();
}

void __malloc_unlock( struct _reent * r )
{
    ( void ) r;
    xTaskResumeAll();
}

/*--------------------------------------------------------------------*/
/* Console: USART1 DMA log ring                                       */
/*--------------------------------------------------------------------*/

ssize_t _write( int fd, const void * buf, size_t len )
{
    if( ( buf == NULL ) || ( fd < 0 ) )
    {
        errno = EBADF;
        return -1;
    }
    /* Everything (stdout and stderr) is funnelled into the log ring.  The
     * ring may drop under burst (counted in wch_log stats); report the
     * requested length so newlib never retries in a loop. */
    wch_log_write( buf, len );
    return ( ssize_t ) len;
}

ssize_t _read( int fd, void * buf, size_t len )
{
    ( void ) fd;
    ( void ) buf;
    ( void ) len;
    /* PA10 is configured as USART1 RX, but no receive consumer exists
     * (no shell by design): reads see EOF. */
    return 0;
}

/*--------------------------------------------------------------------*/
/* Character-device plumbing (RTT behaves like a TTY)                 */
/*--------------------------------------------------------------------*/

int _close( int fd )
{
    ( void ) fd;
    errno = EBADF;
    return -1;
}

int _fstat( int fd, struct stat * st )
{
    ( void ) fd;
    if( st == NULL )
    {
        errno = EBADF;
        return -1;
    }
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty( int fd )
{
    ( void ) fd;
    return 1;
}

off_t _lseek( int fd, off_t pos, int whence )
{
    ( void ) fd;
    ( void ) pos;
    ( void ) whence;
    errno = ESPIPE;
    return ( off_t ) -1;
}

/*--------------------------------------------------------------------*/
/* Re-entrant syscall family (_r variants).                            */
/*                                                                     */
/* newlib's libc calls the _r versions internally (malloc -> _sbrk_r,  */
/* stdio -> _write_r, ...).  Without these, libc links its OWN default */
/* wrappers, which on riscv are LINUX SYSTEM CALLS (li a7,SYS_x; ecall)*/
/* - on bare metal every first heap allocation or stdio use raised an  */
/* ecall exception that landed in a weak-vector spin loop: the entire  */
/* "silent hang after np:8" class of bring-up crashes.                  */
/*--------------------------------------------------------------------*/

void * _sbrk_r( struct _reent * r, ptrdiff_t incr )
{
    char * ret;

    ret = _sbrk( incr );
    if( ret == ( char * ) -1 )
    {
        r->_errno = ENOMEM;
    }
    return ret;
}

ssize_t _write_r( struct _reent * r, int fd, const void * buf, size_t len )
{
    ( void ) r;
    return _write( fd, buf, len );
}

ssize_t _read_r( struct _reent * r, int fd, void * buf, size_t len )
{
    ( void ) r;
    return _read( fd, buf, len );
}

int _open_r( struct _reent * r, const char * path, int flags, int mode )
{
    ( void ) r; ( void ) path; ( void ) flags; ( void ) mode;
    errno = ENOSYS;
    return -1;
}

int _close_r( struct _reent * r, int fd )
{
    ( void ) r;
    return _close( fd );
}

int _fstat_r( struct _reent * r, int fd, struct stat * st )
{
    ( void ) r;
    return _fstat( fd, st );
}

int _isatty_r( struct _reent * r, int fd )
{
    ( void ) r;
    return _isatty( fd );
}

off_t _lseek_r( struct _reent * r, int fd, off_t pos, int whence )
{
    ( void ) r;
    return _lseek( fd, pos, whence );
}

int _kill_r( struct _reent * r, int pid, int sig )
{
    ( void ) r;
    return _kill( pid, sig );
}

int _getpid_r( struct _reent * r )
{
    ( void ) r;
    return _getpid();
}

/*--------------------------------------------------------------------*/
/* Process / OS stubs                                                 */
/*--------------------------------------------------------------------*/

void _exit( int status )
{
    ( void ) status;
    taskDISABLE_INTERRUPTS();
    wch_log_panic( "\r\n[_exit] status=%d - halting\r\n", status );
    for( ;; )
    {
    }
}

int _kill( int pid, int sig )
{
    ( void ) pid;
    ( void ) sig;
    errno = EINVAL;
    return -1;
}

int _getpid( void )
{
    return 1;
}

int _gettimeofday( struct timeval * tv, void * tz )
{
    ( void ) tz;
    /* No wall clock in the bring-up image; report the epoch so callers
     * that only check the return value keep working. */
    if( tv != NULL )
    {
        tv->tv_sec = 0;
        tv->tv_usec = 0;
    }
    return 0;
}

char * __env[ 1 ] = { 0 };
char ** environ = __env;
