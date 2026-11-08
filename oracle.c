/* ORACLE.C     (C) Copyright The Ferrite contributors, 2026         */
/*              Oracle shim for differential testing                 */
/*                                                                   */
/*   Released under "The Q Public License Version 1"                 */
/*   (http://www.hercules-390.org/herclic.html) as modifications to  */
/*   Hercules.                                                       */
/*-------------------------------------------------------------------*/
/* This loadable module turns Hercules into a reference "oracle"     */
/* for the Ferrite emulator's differential test harness. It serves   */
/* the oracle protocol (Ferrite repository: docs/oracle-protocol.md) */
/* on a localhost TCP port and lets a single client reset the        */
/* machine, load storage, set and read the state of CPU 0, and       */
/* execute an exact number of instructions.                          */
/*                                                                   */
/* Usage:   ldmod oracle        (port from FERRITE_ORACLE_PORT,      */
/*                               default 15370)                     */
/*                                                                   */
/* Instruction counting relies on a small patch in cpu.c: while      */
/* sysblk.oracle is set, process_trace() lets regs->oracle_steps     */
/* instructions execute and then stops the CPU quietly before the    */
/* next one.                                                         */
/*-------------------------------------------------------------------*/

#include "hstdinc.h"

#define _ORACLE_C_
#define _ORACLE_DLL_

#include "hercules.h"
#include "opcode.h"
#include "inline.h"

#if !defined( _MSVC_ )
  #include <netinet/in.h>
  #include <netinet/tcp.h>
#endif

/*-------------------------------------------------------------------*/
/*   ARCH_DEP section: compiled multiple times, once for each arch.  */
/*-------------------------------------------------------------------*/

#if !defined( COMPILE_THIS_ONLY_ONCE )
#define       COMPILE_THIS_ONLY_ONCE

#define ORACLE_STATE_SIZE   512         /* State block size          */

/* Wire values of the architecture modes */
#define ORACLE_ARCH_S370    0
#define ORACLE_ARCH_ESA390  1
#define ORACLE_ARCH_Z       2

#endif // COMPILE_THIS_ONLY_ONCE

/*-------------------------------------------------------------------*/
/* Invalidate everything the CPU caches about storage and its state  */
/*-------------------------------------------------------------------*/
static void ARCH_DEP( oracle_purge )( REGS* regs )
{
    INVALIDATE_AIA( regs );
    ARCH_DEP( purge_tlb )( regs );
#if defined( FEATURE_ACCESS_REGISTERS )
    ARCH_DEP( purge_alb )( regs );
#endif
}

/*-------------------------------------------------------------------*/
/* Store the architected state of a CPU into a state block           */
/*-------------------------------------------------------------------*/
static void ARCH_DEP( oracle_get_state )( REGS* regs, BYTE* b )
{
    int i;

    memset( b, 0, ORACLE_STATE_SIZE );

#if   ARCH_IDX == ARCH_370_IDX
    b[0] = ORACLE_ARCH_S370;
#elif ARCH_IDX == ARCH_390_IDX
    b[0] = ORACLE_ARCH_ESA390;
#else
    b[0] = ORACLE_ARCH_Z;
#endif
    STORE_HW( b + 2, regs->cpuad );

    /* PSW in architected format (8 bytes, or 16 for z/Arch) */
    ARCH_DEP( store_psw )( regs, b + 8 );

    for (i = 0; i < 16; i++)
    {
#if ARCH_IDX == ARCH_900_IDX
        STORE_DW( b +  24 + 8*i, regs->GR_G( i ));
        STORE_DW( b + 152 + 8*i, regs->CR_G( i ));
#else
        STORE_DW( b +  24 + 8*i, (U64) regs->GR_L( i ));
        STORE_DW( b + 152 + 8*i, (U64) regs->CR_L( i ));
#endif
#if defined( FEATURE_ACCESS_REGISTERS )
        STORE_FW( b + 280 + 4*i, regs->AR( i ));
#endif
    }

#if ARCH_IDX == ARCH_370_IDX
    /* S/370: floating-point registers 0, 2, 4 and 6 only */
    for (i = 0; i < 8; i += 2)
        STORE_DW( b + 344 + 8*i, regs->FPR_L( i ));
#else
    for (i = 0; i < 16; i++)
        STORE_DW( b + 344 + 8*i, regs->FPR_L( i ));
    STORE_FW( b + 472, regs->fpc );
#endif

    STORE_DW( b + 480, (U64) regs->PX );
    STORE_DW( b + 488, (U64) get_cpu_timer( regs ));
    STORE_DW( b + 496, (U64) regs->clkc );
}

/*-------------------------------------------------------------------*/
/* Replace the architected state of a CPU from a state block         */
/* Returns 0, or non-zero if the PSW is invalid                      */
/*-------------------------------------------------------------------*/
static int ARCH_DEP( oracle_set_state )( REGS* regs, BYTE* b )
{
    int  i;
    int  rc;
    U64  px;

    /* Sync psw.IA and drop all cached translations first */
    ARCH_DEP( oracle_purge )( regs );

    for (i = 0; i < 16; i++)
    {
#if ARCH_IDX == ARCH_900_IDX
        regs->GR_G( i ) = fetch_dw( b +  24 + 8*i );
        regs->CR_G( i ) = fetch_dw( b + 152 + 8*i );
#else
        regs->GR_L( i ) = (U32) fetch_dw( b +  24 + 8*i );
        regs->CR_L( i ) = (U32) fetch_dw( b + 152 + 8*i );
#endif
#if defined( FEATURE_ACCESS_REGISTERS )
        regs->AR( i ) = fetch_fw( b + 280 + 4*i );
#endif
    }

#if ARCH_IDX == ARCH_370_IDX
    for (i = 0; i < 8; i += 2)
        regs->FPR_L( i ) = fetch_dw( b + 344 + 8*i );
#else
    for (i = 0; i < 16; i++)
        regs->FPR_L( i ) = fetch_dw( b + 344 + 8*i );
    regs->fpc = fetch_fw( b + 472 );
#endif

    /* Prefix register */
    px = fetch_dw( b + 480 ) & PX_MASK;
    if (px > sysblk.mainsize - sizeof( PSA_3XX ))
        return 1;
    regs->PX  = px;
    regs->psa = (PSA_3XX*)(regs->mainstor + regs->PX);

    set_cpu_timer( regs, (S64) fetch_dw( b + 488 ));
    regs->clkc = fetch_dw( b + 496 );

    /* PSW last, so that its checks see the new control registers */
    if ((rc = ARCH_DEP( load_psw )( regs, b + 8 )) != 0)
        return rc;

    SET_IC_MASK( regs );
    TEST_SET_AEA_MODE( regs );
    ARCH_DEP( oracle_purge )( regs );
    return 0;
}

/*-------------------------------------------------------------------*/
/*          (delineates ARCH_DEP from non-arch_dep)                  */
/*-------------------------------------------------------------------*/

#if !defined( _GEN_ARCH )

  #if defined(              _ARCH_NUM_1 )
    #define   _GEN_ARCH     _ARCH_NUM_1
    #include "oracle.c"
  #endif

  #if defined(              _ARCH_NUM_2 )
    #undef    _GEN_ARCH
    #define   _GEN_ARCH     _ARCH_NUM_2
    #include "oracle.c"
  #endif

/*-------------------------------------------------------------------*/
/*  non-ARCH_DEP section: compiled only ONCE after last arch built   */
/*-------------------------------------------------------------------*/

#define ORACLE_VERSION      1           /* Protocol version          */
#define ORACLE_DEFAULT_PORT 15370       /* Default TCP port          */
#define ORACLE_MAX_MESSAGE  (16*1024*1024)
#define ORACLE_MAX_READ     (1024*1024)
#define ORACLE_CPU          0           /* The oracle drives CPU 0   */

/* Message types */
#define MSG_HELLO           0x0001
#define MSG_BYE             0x0002
#define MSG_RESET           0x0010
#define MSG_LOAD            0x0011
#define MSG_SET_KEYS        0x0012
#define MSG_READ_KEYS       0x0013
#define MSG_READ_STORAGE    0x0014
#define MSG_CHANGED_PAGES   0x0015
#define MSG_SET_STATE       0x0020
#define MSG_GET_STATE       0x0021
#define MSG_STEP            0x0030
#define MSG_REPLY           0x8000
#define MSG_ERROR           0xFFFF

/* Error codes */
#define ERR_VERSION         1
#define ERR_MALFORMED       2
#define ERR_RANGE           3
#define ERR_STATE           4
#define ERR_INTERNAL        5

/* STEP stop reasons */
#define STOP_COUNT          0
#define STOP_WAIT           1
#define STOP_STOPPED        2
#define STOP_TIMEOUT        3

/* Storage key bits exchanged on the wire (ACC F R C) */
#define ORACLE_KEY_MASK     (STORKEY_KEY | STORKEY_FETCH | STORKEY_REF | STORKEY_CHANGE)

static TID   oracle_tid;                /* Server thread             */
static int   oracle_port   = ORACLE_DEFAULT_PORT;
static int   oracle_listen = -1;        /* Listening socket          */
static bool  oracle_exit   = false;     /* Server shutdown requested */

/*-------------------------------------------------------------------*/
/* Socket helpers                                                    */
/*-------------------------------------------------------------------*/
static bool read_exact( int fd, BYTE* buf, size_t len )
{
    while (len)
    {
        ssize_t n = recv( fd, buf, len, 0 );
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        buf += n;
        len -= (size_t) n;
    }
    return true;
}

static bool write_all( int fd, const BYTE* buf, size_t len )
{
    while (len)
    {
        ssize_t n = send( fd, buf, len, 0 );
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        buf += n;
        len -= (size_t) n;
    }
    return true;
}

static bool send_reply( int fd, U16 type, const BYTE* payload, size_t len )
{
    BYTE hdr[8];
    STORE_FW( hdr + 0, (U32)(4 + len) );
    STORE_HW( hdr + 4, type );
    STORE_HW( hdr + 6, 0 );
    return write_all( fd, hdr, sizeof( hdr ))
        && (len == 0 || write_all( fd, payload, len ));
}

static bool send_error( int fd, U16 code, const char* msg )
{
    BYTE   buf[260];
    size_t n = strlen( msg );
    if (n > sizeof( buf ) - 4)
        n = sizeof( buf ) - 4;
    STORE_HW( buf + 0, code );
    STORE_HW( buf + 2, (U16) n );
    memcpy( buf + 4, msg, n );
    return send_reply( fd, MSG_ERROR, buf, 4 + n );
}

/*-------------------------------------------------------------------*/
/* Architecture dispatch helpers                                     */
/*-------------------------------------------------------------------*/
static int arch_idx_from_wire( BYTE arch )
{
    switch (arch)
    {
#if defined( _370 )
    case ORACLE_ARCH_S370:   return ARCH_370_IDX;
#endif
#if defined( _390 )
    case ORACLE_ARCH_ESA390: return ARCH_390_IDX;
#endif
#if defined( _900 )
    case ORACLE_ARCH_Z:      return ARCH_900_IDX;
#endif
    default:                 return -1;
    }
}

static void oracle_get_state( REGS* regs, BYTE* b )
{
    switch (sysblk.arch_mode)
    {
#if defined( _370 )
    case ARCH_370_IDX: s370_oracle_get_state( regs, b ); break;
#endif
#if defined( _390 )
    case ARCH_390_IDX: s390_oracle_get_state( regs, b ); break;
#endif
#if defined( _900 )
    case ARCH_900_IDX: z900_oracle_get_state( regs, b ); break;
#endif
    default: CRASH();
    }
}

static int oracle_set_state( REGS* regs, BYTE* b )
{
    switch (sysblk.arch_mode)
    {
#if defined( _370 )
    case ARCH_370_IDX: return s370_oracle_set_state( regs, b );
#endif
#if defined( _390 )
    case ARCH_390_IDX: return s390_oracle_set_state( regs, b );
#endif
#if defined( _900 )
    case ARCH_900_IDX: return z900_oracle_set_state( regs, b );
#endif
    default: CRASH();
    }
    UNREACHABLE_CODE( return -1 );
}

static void oracle_purge( REGS* regs )
{
    switch (sysblk.arch_mode)
    {
#if defined( _370 )
    case ARCH_370_IDX: s370_oracle_purge( regs ); break;
#endif
#if defined( _390 )
    case ARCH_390_IDX: s390_oracle_purge( regs ); break;
#endif
#if defined( _900 )
    case ARCH_900_IDX: z900_oracle_purge( regs ); break;
#endif
    default: CRASH();
    }
}

/*-------------------------------------------------------------------*/
/* CPU control                                                       */
/*-------------------------------------------------------------------*/

/* Bring CPU 0 into the regular stopped state. A CPU parked in the
   oracle stop inside process_trace() is released and stops again in
   the normal instruction-loop path. Returns false on timeout. */
static bool oracle_stop_cpu( void )
{
    int i;

    for (i = 0; i < 5000; i++)          /* about 5 seconds           */
    {
        REGS* regs;
        bool  done = false;

        OBTAIN_INTLOCK( NULL );
        {
            regs = sysblk.regs[ ORACLE_CPU ];
            if (regs->cpustate == CPUSTATE_STOPPED && !regs->stepwait)
                done = true;
            else if (regs->cpustate != CPUSTATE_STOPPING)
            {
                regs->cpustate = CPUSTATE_STOPPING;
                ON_IC_INTERRUPT( regs );
                WAKEUP_CPU( regs );
            }
        }
        RELEASE_INTLOCK( NULL );

        if (done)
            return true;
        USLEEP( 1000 );
    }
    return false;
}

/*-------------------------------------------------------------------*/
/* Request handlers. Each returns false if the connection is broken. */
/*-------------------------------------------------------------------*/

static bool do_hello( int fd, BYTE* p, U32 n )
{
    BYTE        buf[128];
    const char* name = "SDL Hercules " VERSION " oracle shim";
    size_t      len  = strlen( name );
    U32         archs = 0;

    if (n < 2)
        return send_error( fd, ERR_MALFORMED, "short HELLO" );
    if (fetch_hw( p ) != ORACLE_VERSION)
        return send_error( fd, ERR_VERSION, "unsupported protocol version" );

#if defined( _370 )
    archs |= 1;
#endif
#if defined( _390 )
    archs |= 2;
#endif
#if defined( _900 )
    archs |= 4;
#endif
    if (len > sizeof( buf ) - 18)
        len = sizeof( buf ) - 18;

    STORE_HW( buf + 0, ORACLE_VERSION );
    STORE_HW( buf + 2, 0 );
    STORE_FW( buf + 4, archs );
    STORE_DW( buf + 8, (U64) sysblk.mainsize );
    STORE_HW( buf + 16, (U16) len );
    memcpy( buf + 18, name, len );
    return send_reply( fd, MSG_HELLO | MSG_REPLY, buf, 18 + len );
}

static bool do_reset( int fd, BYTE* p, U32 n )
{
    int  target;
    bool clear;
    U64  mainsize;
    int  rc;

    if (n < 12)
        return send_error( fd, ERR_MALFORMED, "short RESET" );
    if ((target = arch_idx_from_wire( p[0] )) < 0)
        return send_error( fd, ERR_STATE, "architecture not supported" );
    clear    = (p[1] & 0x01) != 0;
    mainsize = fetch_dw( p + 4 );
    if (mainsize && mainsize != (U64) sysblk.mainsize)
        return send_error( fd, ERR_STATE, "main storage size cannot be changed" );

    if (!oracle_stop_cpu())
        return send_error( fd, ERR_INTERNAL, "CPU did not stop" );

    OBTAIN_INTLOCK( NULL );
    {
        rc = system_reset( target, clear, false, ORACLE_CPU );
        if (rc == 0)
        {
            REGS* regs = sysblk.regs[ ORACLE_CPU ];
            regs->oracle_steps = 0;
            regs->breakortrace = 1;
            sysblk.oracle      = 1;
        }
    }
    RELEASE_INTLOCK( NULL );

    if (rc != 0)
        return send_error( fd, ERR_INTERNAL, "system reset failed" );
    return send_reply( fd, MSG_RESET | MSG_REPLY, NULL, 0 );
}

static bool do_load( int fd, BYTE* p, U32 n )
{
    U64 addr;
    U32 len;

    if (n < 8)
        return send_error( fd, ERR_MALFORMED, "short LOAD" );
    addr = fetch_dw( p );
    len  = n - 8;
    if (addr > (U64) sysblk.mainsize || len > (U64) sysblk.mainsize - addr)
        return send_error( fd, ERR_RANGE, "LOAD outside main storage" );

    OBTAIN_INTLOCK( NULL );
    {
        memcpy( sysblk.mainstor + addr, p + 8, len );
        oracle_purge( sysblk.regs[ ORACLE_CPU ] );
    }
    RELEASE_INTLOCK( NULL );

    return send_reply( fd, MSG_LOAD | MSG_REPLY, NULL, 0 );
}

/* Key array index for an absolute address */
#define KEYIDX( abs )   ((abs) >> _STORKEY_ARRAY_SHIFTAMT)

static bool do_set_keys( int fd, BYTE* p, U32 n )
{
    U64 addr;
    U32 count, i;

    if (n < 8)
        return send_error( fd, ERR_MALFORMED, "short SET_KEYS" );
    addr  = fetch_dw( p );
    count = n - 8;
    if ((addr & (_2K - 1)) != 0)
        return send_error( fd, ERR_MALFORMED, "address not 2K aligned" );
    if (addr > (U64) sysblk.mainsize
        || (U64) count * _2K > (U64) sysblk.mainsize - addr)
        return send_error( fd, ERR_RANGE, "SET_KEYS outside main storage" );

    OBTAIN_INTLOCK( NULL );
    {
        for (i = 0; i < count; i++)
        {
            BYTE* k = &sysblk.storkeys[ KEYIDX( addr + (U64) i * _2K ) ];
            *k = (*k & ~ORACLE_KEY_MASK) | (p[8 + i] & ORACLE_KEY_MASK);
        }
        oracle_purge( sysblk.regs[ ORACLE_CPU ] );
    }
    RELEASE_INTLOCK( NULL );

    return send_reply( fd, MSG_SET_KEYS | MSG_REPLY, NULL, 0 );
}

static bool do_read_keys( int fd, BYTE* p, U32 n )
{
    U64   addr;
    U32   count, i;
    BYTE* out;
    bool  ok;

    if (n < 12)
        return send_error( fd, ERR_MALFORMED, "short READ_KEYS" );
    addr  = fetch_dw( p );
    count = fetch_fw( p + 8 );
    if ((addr & (_2K - 1)) != 0)
        return send_error( fd, ERR_MALFORMED, "address not 2K aligned" );
    if (count > ORACLE_MAX_READ || addr > (U64) sysblk.mainsize
        || (U64) count * _2K > (U64) sysblk.mainsize - addr)
        return send_error( fd, ERR_RANGE, "READ_KEYS outside main storage" );
    if (!(out = malloc( count ? count : 1 )))
        return send_error( fd, ERR_INTERNAL, "out of memory" );

    for (i = 0; i < count; i++)
        out[i] = sysblk.storkeys[ KEYIDX( addr + (U64) i * _2K ) ] & ORACLE_KEY_MASK;

    ok = send_reply( fd, MSG_READ_KEYS | MSG_REPLY, out, count );
    free( out );
    return ok;
}

static bool do_read_storage( int fd, BYTE* p, U32 n )
{
    U64 addr;
    U32 len;

    if (n < 12)
        return send_error( fd, ERR_MALFORMED, "short READ_STORAGE" );
    addr = fetch_dw( p );
    len  = fetch_fw( p + 8 );
    if (len > ORACLE_MAX_READ)
        return send_error( fd, ERR_RANGE, "READ_STORAGE longer than 1 MiB" );
    if (addr > (U64) sysblk.mainsize || len > (U64) sysblk.mainsize - addr)
        return send_error( fd, ERR_RANGE, "READ_STORAGE outside main storage" );

    return send_reply( fd, MSG_READ_STORAGE | MSG_REPLY, sysblk.mainstor + addr, len );
}

static bool do_changed_pages( int fd, BYTE* p, U32 n )
{
    U64   pages = (U64) sysblk.mainsize / _4K;
    U64   page;
    U32   count = 0;
    bool  reset;
    BYTE* out;
    bool  ok;

    if (n < 1)
        return send_error( fd, ERR_MALFORMED, "short CHANGED_PAGES" );
    reset = (p[0] & 0x01) != 0;
    if (!(out = malloc( 4 + pages * 8 )))
        return send_error( fd, ERR_INTERNAL, "out of memory" );

    OBTAIN_INTLOCK( NULL );
    {
        for (page = 0; page < pages; page++)
        {
            U64  abs     = page * _4K;
            U64  first   = KEYIDX( abs );
            U64  last    = KEYIDX( abs + _4K - 1 );
            U64  k;
            bool changed = false;

            for (k = first; k <= last; k++)
                if (sysblk.storkeys[k] & STORKEY_CHANGE)
                    changed = true;
            if (!changed)
                continue;

            STORE_DW( out + 4 + (U64) count * 8, abs );
            count++;
            if (reset)
                for (k = first; k <= last; k++)
                    sysblk.storkeys[k] &= ~STORKEY_CHANGE;
        }

        /* Hercules sets R/C bits only when a TLB entry is created,
           so the TLB must be purged for new stores to be noticed. */
        if (reset)
            oracle_purge( sysblk.regs[ ORACLE_CPU ] );
    }
    RELEASE_INTLOCK( NULL );

    STORE_FW( out, count );
    ok = send_reply( fd, MSG_CHANGED_PAGES | MSG_REPLY, out, 4 + (size_t) count * 8 );
    free( out );
    return ok;
}

static bool do_get_state( int fd )
{
    BYTE state[ ORACLE_STATE_SIZE ];

    OBTAIN_INTLOCK( NULL );
    oracle_get_state( sysblk.regs[ ORACLE_CPU ], state );
    RELEASE_INTLOCK( NULL );

    return send_reply( fd, MSG_GET_STATE | MSG_REPLY, state, sizeof( state ));
}

static bool do_set_state( int fd, BYTE* p, U32 n )
{
    int rc;

    if (n != ORACLE_STATE_SIZE)
        return send_error( fd, ERR_MALFORMED, "state block has wrong size" );
    if (arch_idx_from_wire( p[0] ) != sysblk.arch_mode)
        return send_error( fd, ERR_STATE, "state block architecture differs from current mode" );

    OBTAIN_INTLOCK( NULL );
    rc = oracle_set_state( sysblk.regs[ ORACLE_CPU ], p );
    RELEASE_INTLOCK( NULL );

    if (rc != 0)
        return send_error( fd, ERR_MALFORMED, "invalid PSW or prefix in state block" );
    return send_reply( fd, MSG_SET_STATE | MSG_REPLY, NULL, 0 );
}

static bool do_step( int fd, BYTE* p, U32 n )
{
    BYTE  out[ 12 + ORACLE_STATE_SIZE ];
    U64   count;
    U32   timeout_ms;
    U64   left  = 0;
    BYTE  why   = STOP_COUNT;
    U64   waited_us = 0;

    if (n < 12)
        return send_error( fd, ERR_MALFORMED, "short STEP" );
    count      = fetch_dw( p );
    timeout_ms = fetch_fw( p + 8 );

    if (!sysblk.oracle)
        return send_error( fd, ERR_STATE, "RESET required before STEP" );

    if (count > 0)
    {
        /* Start the CPU with the step budget */
        OBTAIN_INTLOCK( NULL );
        {
            REGS* regs = sysblk.regs[ ORACLE_CPU ];
            oracle_purge( regs );
            regs->oracle_steps = count;
            regs->breakortrace = 1;
            regs->opinterv     = 0;
            regs->checkstop    = 0;
            regs->cpustate     = CPUSTATE_STARTED;
            WAKEUP_CPU( regs );
        }
        RELEASE_INTLOCK( NULL );

        /* Wait until it stops, enters a wait state or times out */
        for (;;)
        {
            bool finished = false;

            OBTAIN_INTLOCK( NULL );
            {
                REGS* regs = sysblk.regs[ ORACLE_CPU ];
                if (regs->cpustate == CPUSTATE_STOPPED)
                {
                    finished = true;
                    left     = regs->oracle_steps;
                    if (regs->stepwait && left == 0)
                        why = STOP_COUNT;
                    else if (WAITSTATE( &regs->psw ))
                        why = STOP_WAIT;
                    else
                        why = STOP_STOPPED;
                }
                else if (regs->cpustate == CPUSTATE_STARTED
                      && WAITSTATE( &regs->psw ))
                {
                    finished = true;
                    left     = regs->oracle_steps;
                    why      = STOP_WAIT;
                }
            }
            RELEASE_INTLOCK( NULL );

            if (finished)
                break;
            if (waited_us >= (U64) timeout_ms * 1000)
            {
                why = STOP_TIMEOUT;
                break;
            }
            USLEEP( 50 );
            waited_us += 50;
        }

        /* Every request except STEP leaves the CPU stopped */
        if (why != STOP_COUNT)
        {
            if (!oracle_stop_cpu())
                return send_error( fd, ERR_INTERNAL, "CPU did not stop" );
            OBTAIN_INTLOCK( NULL );
            left = sysblk.regs[ ORACLE_CPU ]->oracle_steps;
            sysblk.regs[ ORACLE_CPU ]->oracle_steps = 0;
            RELEASE_INTLOCK( NULL );
        }
    }

    STORE_DW( out, count - left );
    out[8] = why;
    out[9] = out[10] = out[11] = 0;

    OBTAIN_INTLOCK( NULL );
    oracle_get_state( sysblk.regs[ ORACLE_CPU ], out + 12 );
    RELEASE_INTLOCK( NULL );

    return send_reply( fd, MSG_STEP | MSG_REPLY, out, sizeof( out ));
}

/*-------------------------------------------------------------------*/
/* Serve one client until it says BYE or disconnects                 */
/*-------------------------------------------------------------------*/
static void oracle_serve( int fd )
{
    BYTE  hdr[8];
    BYTE* buf = NULL;
    bool  ok  = true;

    while (ok && !oracle_exit)
    {
        U32 len, n;
        U16 type;

        if (!read_exact( fd, hdr, sizeof( hdr )))
            break;
        len  = fetch_fw( hdr );
        type = fetch_hw( hdr + 4 );
        if (len < 4 || len > ORACLE_MAX_MESSAGE)
            break;
        n = len - 4;

        free( buf );
        if (!(buf = malloc( n ? n : 1 )) || !read_exact( fd, buf, n ))
            break;

        switch (type)
        {
        case MSG_HELLO:         ok = do_hello( fd, buf, n );         break;
        case MSG_RESET:         ok = do_reset( fd, buf, n );         break;
        case MSG_LOAD:          ok = do_load( fd, buf, n );          break;
        case MSG_SET_KEYS:      ok = do_set_keys( fd, buf, n );      break;
        case MSG_READ_KEYS:     ok = do_read_keys( fd, buf, n );     break;
        case MSG_READ_STORAGE:  ok = do_read_storage( fd, buf, n );  break;
        case MSG_CHANGED_PAGES: ok = do_changed_pages( fd, buf, n ); break;
        case MSG_SET_STATE:     ok = do_set_state( fd, buf, n );     break;
        case MSG_GET_STATE:     ok = do_get_state( fd );             break;
        case MSG_STEP:          ok = do_step( fd, buf, n );          break;
        case MSG_BYE:
            send_reply( fd, MSG_BYE | MSG_REPLY, NULL, 0 );
            ok = false;
            break;
        default:
            ok = send_error( fd, ERR_MALFORMED, "unknown message type" );
            break;
        }
    }
    free( buf );
}

/*-------------------------------------------------------------------*/
/* Server thread                                                     */
/*-------------------------------------------------------------------*/
static void* oracle_thread( void* arg )
{
    struct sockaddr_in sa;
    int one = 1;

    UNREFERENCED( arg );

    if ((oracle_listen = socket( AF_INET, SOCK_STREAM, 0 )) < 0)
    {
        logmsg( "HHCOR001E oracle: socket() failed: %s\n", strerror( errno ));
        return NULL;
    }
    setsockopt( oracle_listen, SOL_SOCKET, SO_REUSEADDR, (void*) &one, sizeof( one ));

    memset( &sa, 0, sizeof( sa ));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons( (U16) oracle_port );
    sa.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    if (bind( oracle_listen, (struct sockaddr*) &sa, sizeof( sa )) < 0
        || listen( oracle_listen, 1 ) < 0)
    {
        logmsg( "HHCOR002E oracle: cannot listen on 127.0.0.1:%d: %s\n", oracle_port, strerror( errno ));
        close( oracle_listen );
        oracle_listen = -1;
        return NULL;
    }

    logmsg( "HHCOR003I oracle: listening on 127.0.0.1:%d\n", oracle_port );

    while (!oracle_exit)
    {
        int fd = accept( oracle_listen, NULL, NULL );
        if (fd < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (void*) &one, sizeof( one ));
        oracle_serve( fd );
        close( fd );
    }
    return NULL;
}

/*-------------------------------------------------------------------*/
/*                   HDL module definitions                          */
/*-------------------------------------------------------------------*/

HDL_DEPENDENCY_SECTION;
{
    HDL_DEPENDENCY( HERCULES );
    HDL_DEPENDENCY( REGS );
    HDL_DEPENDENCY( SYSBLK );
}
END_DEPENDENCY_SECTION;

HDL_REGISTER_SECTION;
{
    const char* port = getenv( "FERRITE_ORACLE_PORT" );

    UNREFERENCED( regsym );

    if (port && atoi( port ) > 0 && atoi( port ) < 65536)
        oracle_port = atoi( port );

    create_thread( &oracle_tid, DETACHED, oracle_thread, NULL, "oracle" );
}
END_REGISTER_SECTION;

HDL_FINAL_SECTION;
{
    oracle_exit   = true;
    sysblk.oracle = 0;
    if (oracle_listen >= 0)
    {
        shutdown( oracle_listen, SHUT_RDWR );
        close( oracle_listen );
        oracle_listen = -1;
    }
}
END_FINAL_SECTION;

#endif // !defined( _GEN_ARCH )
