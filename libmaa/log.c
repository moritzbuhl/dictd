/* log.c -- Logging routines, for a single, program-wide logging facility
 * Created: Mon Mar 10 09:37:21 1997 by faith@dict.org
 * Revised: Sun Mar 31 11:54:19 2002 by faith@dict.org
 * Copyright 1997-1999, 2001-2002 Rickard E. Faith (faith@dict.org)
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Library General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 * 
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * 
 * 
 * $Id: log.c,v 1.13 2002/08/05 11:16:53 cheusov Exp $
 * 
 */

#include "maaP.h"
#ifdef HAVE_SYSLOG_NAMES
#define SYSLOG_NAMES
#endif
#include <syslog.h>
#include <fcntl.h>
#include <sys/stat.h>

#if HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 64
#endif

static int        logFd = -1;
static FILE       *logUserStream;
static int        logSyslog;
static int        inhibitFull = 0;

static int        logOpen;
static int        logFacility = LOG_USER;

static const char *logIdent;
static const char *logFilenameOrig;
static char       *logFilename;
static char       *logFilenameTmp;
static int        logFilenameLen;
static char       logHostname[MAXHOSTNAMELEN];

#ifndef HAVE_SYSLOG_NAMES
typedef struct _code {
    const char *c_name;
    int        c_val;
} CODE;
CODE facilitynames[] = {
#if LOG_AUTH
    { "auth",     LOG_AUTH },
#endif
#if LOG_AUTHPRIV
    { "authpriv", LOG_AUTHPRIV },
#endif
#if LOG_CRON
    { "cron",     LOG_CRON },
#endif
#if LOG_DAEMON
    { "daemon",   LOG_DAEMON },
#endif
#if LOG_FTP
    { "ftp",      LOG_FTP },
#endif
#if LOG_KERN
    { "kern",     LOG_KERN },
#endif
#if LOG_LPR
    { "lpr",      LOG_LPR },
#endif
#if LOG_MAIL
    { "mail",     LOG_MAIL },
#endif
#if LOG_NEWS
    { "news",     LOG_NEWS },
#endif
#if LOG_SYSLOG
    { "syslog",   LOG_SYSLOG },
#endif
#if LOG_USER
    { "user",     LOG_USER },
#endif
#if LOG_UUCP
    { "uucp",     LOG_UUCP },
#endif
#if LOG_LOCAL0
    { "local0",   LOG_LOCAL0 },
#endif
#if LOG_LOCAL1
    { "local1",   LOG_LOCAL1 },
#endif
#if LOG_LOCAL2
    { "local2",   LOG_LOCAL2 },
#endif
#if LOG_LOCAL3
    { "local3",   LOG_LOCAL3 },
#endif
#if LOG_LOCAL4
    { "local4",   LOG_LOCAL4 },
#endif
#if LOG_LOCAL5
    { "local5",   LOG_LOCAL5 },
#endif
#if LOG_LOCAL6
    { "local6",   LOG_LOCAL6 },
#endif
#if LOG_LOCAL7
    { "local7",   LOG_LOCAL7 },
#endif
    { NULL,       -1 },
};
#endif

static void _log_set_hostname( void )
{
   static int hostnameSet = 0;
   char       *pt;

   if (!hostnameSet) {
      memset( logHostname, 0, sizeof(logHostname) );
      gethostname( logHostname, sizeof(logHostname)-1 );
      if ((pt = strchr(logHostname, '.'))) *pt = '\0';
      ++hostnameSet;
   }
}

void log_set_facility(const char *facility)
{
    CODE *pt;

    for (pt = facilitynames; pt->c_name; pt++) {
        if (!strcmp(pt->c_name, facility)) {
            logFacility = pt->c_val;
            return;
        }
    }
    err_fatal(__FUNCTION__, "%s is not a valid facility name\n", facility);
}

const char *log_get_facility(void)
{
    CODE *pt;

    for (pt = facilitynames; pt->c_name; pt++)
        if (pt->c_val == logFacility) return pt->c_name;
    return NULL;
}

void log_option( int option )
{
   if (option == LOG_OPTION_NO_FULL) inhibitFull = 1;
   else                              inhibitFull = 0;
}

void log_syslog( const char *ident )
{
   if (logSyslog)
      err_internal( __FUNCTION__, "Syslog facility already open\n" );
   
   openlog( ident, LOG_PID|LOG_NOWAIT, logFacility );
   ++logOpen;
   ++logSyslog;
}

static void log_mkpath(const char *filename)
{
    char *tmp = alloca(strlen(filename) + 1);
    char *pt;
    
    strcpy(tmp, filename);
    for (pt = tmp; *pt; pt++) {
        if (*pt == '/' && pt != tmp) {
            *pt = '\0';
            mkdir(tmp, 0755);
            *pt = '/';
        }
    }
}

static void _log_check_filename(void)
{
   time_t    t;
   struct tm *tm;
   
   if (!logFilename || !logFilenameTmp || !logFilenameLen) return;

   time(&t);
   tm = localtime(&t);
   
   strftime(logFilenameTmp, logFilenameLen, logFilenameOrig, tm);
   if (strcmp(logFilenameTmp, logFilename)) {
       strcpy(logFilename, logFilenameTmp);
       if (logFd >= 0) close(logFd);
       log_mkpath(logFilename);
       if ((logFd = open( logFilename, O_WRONLY|O_CREAT|O_APPEND, 0644 )) < 0)
           err_fatal_errno( __FUNCTION__,
                            "Cannot open \"%s\" for append\n", logFilename );
   }
}

void log_file( const char *ident, const char *filename )
{
   if (logFd >= 0)
      err_internal( __FUNCTION__,
		    "Log file \"%s\" open when trying to open \"%s\"\n",
		    logFilename, filename );

   logIdent        = str_find( ident );
   logFilenameOrig = str_find(filename);
   logFilenameLen  = strlen(filename)*3+1024;
   logFilename     = xmalloc(logFilenameLen + 1);
   logFilenameTmp  = xmalloc(logFilenameLen + 1);
   logFilename[0]  = '\0';
   _log_check_filename();
   
   _log_set_hostname();
   ++logOpen;
}

void log_stream( const char *ident, FILE *stream )
{
   if (logUserStream)
      err_internal( __FUNCTION__, "User stream already open\n" );

   logUserStream = stream;
   logIdent      = str_find( ident );

   _log_set_hostname();
   ++logOpen;
}

void log_close( void )
{
   if (logFd >= 0)    close( logFd );
   if (logUserStream != stdout && logUserStream != stderr && logUserStream != NULL)
       fclose( logUserStream );
   if (logSyslog)     closelog();
   if (logFilename)   xfree(logFilename);
   if (logFilenameTmp) xfree(logFilenameTmp);

   logFilename    = 0;
   logFilenameLen = 0;
   
   logOpen       = 0;
   logFd         = -1;
   logUserStream = NULL;
   logSyslog     = 0;
}

void log_error_va( const char *routine, const char *format, va_list ap )
{
   time_t t;
   char   buf[4096];
   char   *pt;
   
   if (!logOpen) return;
   
   time(&t);
   
   if (logFd >= 0 || logUserStream) {
      if (inhibitFull) {
         pt = buf;
      } else {
         sprintf( buf,
                  "%24.24s %s %s[%ld]: ",
                  ctime(&t),
                  logHostname,
                  logIdent,
                  (long int)getpid() );
         pt = buf + strlen( buf );
      }
      if (routine) sprintf( pt, "(%s) ", routine );
      pt = buf + strlen( buf );
      vsprintf( pt, format, ap );
      
      if (logFd >= 0) {
          _log_check_filename();
          write( logFd, buf, strlen(buf) );
      }
      if (logUserStream) {
         fseek( logUserStream, 0L, SEEK_END ); /* might help if luser didn't
                                                  open stream with "a" */
         fprintf( logUserStream, "%s", buf );
         fflush( logUserStream );
      }
   }
   
#if !defined(__DGUX__) && !defined(__hpux__) && !defined(__CYGWIN__)
#if !defined(__osf__)
   if (logSyslog) {
      vsyslog( LOG_ERR, format, ap );
   }
#endif
#endif
}

void log_error( const char *routine, const char *format, ... )
{
   va_list ap;

   va_start( ap, format );
   log_error_va( routine, format, ap );
   va_end( ap );
}

void log_info_va( const char *format, va_list ap )
{
   time_t t;
   char   buf[4096];
   char   *pt;
   
   if (!logOpen) return;
   
   time(&t);
   
   if (logFd >= 0 || logUserStream) {
      if (inhibitFull) {
         pt = buf;
      } else {
         sprintf( buf,
                  "%24.24s %s %s[%ld]: ",
                  ctime(&t),
                  logHostname,
                  logIdent,
                  (long int)getpid() );
         pt = buf + strlen( buf );
      }
      vsprintf( pt, format, ap );
      
      if (logFd >= 0) {
          _log_check_filename();
          write( logFd, buf, strlen(buf) );
      }
      if (logUserStream) {
         fseek( logUserStream, 0L, SEEK_END ); /* might help if luser didn't
                                                  open stream with "a" */
         fprintf( logUserStream, "%s", buf );
         fflush( logUserStream );
      }
   }
   
#if !defined(__DGUX__) && !defined(__hpux__) && !defined(__CYGWIN__)
#if !defined(__osf__)
   if (logSyslog) {
      vsyslog( LOG_INFO, format, ap );
   }
#endif
#endif
}

void log_info( const char *format, ... )
{
   va_list ap;

   va_start( ap, format );
   log_info_va( format, ap );
   va_end( ap );
}
