/*
 * port/syscalls.c - newlib-nano syscall stubs for the CH32V208 bring-up.
 *
 *  - console I/O goes through SEGGER RTT channel 0
 *    (_write -> RTT up buffer, _read -> RTT down buffer)
 *  - _sbrk grows the newlib heap between the ld/Link.ld symbols
 *    _heap_start / _heap_end (located above .bss, below the 2 KB
 *    reset stack)
 *  - heap locking: newlib's malloc can be re-entered from different
 *    FreeRTOS tasks, so __malloc_lock/__malloc_unlock suspend/resume
 *    the scheduler.  (Interrupt-context allocation is not expected;
 *    NimBLE allocates from tasks only.)
 */

#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"
#include "SEGGER_RTT.h"

#undef errno
extern int errno;

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
/* Console: RTT channel 0                                             */
/*--------------------------------------------------------------------*/

ssize_t _write( int fd, const void * buf, size_t len )
{
    if( ( buf == NULL ) || ( fd < 0 ) )
    {
        errno = EBADF;
        return -1;
    }
    /* Everything (stdout and stderr) is funnelled into RTT ch 0. */
    return ( ssize_t ) SEGGER_RTT_Write( 0, buf, len );
}

ssize_t _read( int fd, void * buf, size_t len )
{
    if( ( buf == NULL ) || ( fd < 0 ) )
    {
        errno = EBADF;
        return -1;
    }
    if( len == 0 )
    {
        return 0;
    }
    return ( ssize_t ) SEGGER_RTT_Read( 0, buf, len );
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
/* Process / OS stubs                                                 */
/*--------------------------------------------------------------------*/

void _exit( int status )
{
    ( void ) status;
    taskDISABLE_INTERRUPTS();
    SEGGER_RTT_printf( 0, "\r\n[_exit] status=%d - halting\r\n", status );
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
