/* dictd.c -- 
 * Created: Fri Feb 21 20:09:09 1997 by faith@dict.org
 * Revised: Sat May  4 21:58:16 2002 by faith@dict.org
 * Copyright 1997-2000, 2002 Rickard E. Faith (faith@dict.org)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 1, or (at your option) any
 * later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 * 
 * $Id: dictd.c,v 1.49 2002/09/12 13:08:06 cheusov Exp $
 * 
 */

#include "dictd.h"
#include "servparse.h"
#include "utf8_ucs4.h"

#include <grp.h>                /* initgroups */
#include <pwd.h>                /* getpwuid */
#include <locale.h>             /* setlocale */

#define MAXPROCTITLE 2048       /* Maximum amount of proc title we'll use. */
#undef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#ifndef UID_NOBODY
#define UID_NOBODY  65534
#endif
#ifndef GID_NOGROUP
#define GID_NOGROUP 65534
#endif

extern int        yy_flex_debug;

extern int        utf8_mode;
extern int        mmap_mode;

static int        _dict_daemon;
static int        _dict_reaps;
static int        _dict_daemon_limit        = DICT_DAEMON_LIMIT;
static int        _dict_markTime;
static char       *_dict_argvstart;
static int        _dict_argvlen;

       int        _dict_forks;
       dictConfig *DictConfig;


void dict_initsetproctitle( int argc, char **argv, char **envp )
{
   int i;

   _dict_argvstart = argv[0];
   
   for (i = 0; envp[i]; i++) continue;
   if (i)
      _dict_argvlen = envp[i-1] + strlen(envp[i-1]) - _dict_argvstart;
   else
      _dict_argvlen = argv[argc-1] + strlen(argv[argc-1]) - _dict_argvstart;
   argv[1] = NULL;
}

void dict_setproctitle( const char *format, ... )
{
   va_list ap;
   int     len;
   char    buf[MAXPROCTITLE];

   va_start( ap, format );
   vsprintf( buf, format, ap );
   va_end( ap );
   if ((len = strlen(buf)) > MAXPROCTITLE-1)
      err_fatal( __FUNCTION__, "buffer overflow (%d)\n", len );
   buf[ MIN(_dict_argvlen,MAXPROCTITLE) - 1 ] = '\0';
   strcpy( _dict_argvstart, buf );
   memset( _dict_argvstart+len, 0, _dict_argvlen-len );
}

const char *dict_format_time( double t )
{
   static int  current = 0;
   static char buf[10][128];	/* Rotate 10 buffers */
   static char *this;
   long int    s, m, h, d;

   this = buf[current];
   if (++current >= 10) current = 0;

   if (t < 600) {
      sprintf( this, "%0.3f", t );
   } else {
      s = (long int)t;
      d = s / (3600*24);
      s -= d * 3600 * 24;
      h = s / 3600;
      s -= h * 3600;
      m = s / 60;
      s -= m * 60;

      if (d)
	 sprintf( this, "%ld+%02ld:%02ld:%02ld", d, h, m, s );
      else if (h)
	 sprintf( this, "%02ld:%02ld:%02ld", h, m, s );
      else
	 sprintf( this, "%02ld:%02ld", m, s );
   }

   return this;
}

static void reaper( int dummy )
{
#if defined(__osf__) || (defined(__sparc) && defined(__SVR4))
   int        status;
#else
   union wait status;
#endif
   pid_t      pid;

   while ((pid = wait3(&status, WNOHANG, NULL)) > 0) {
      ++_dict_reaps;
      
      if (flg_test(LOG_SERVER))
         log_info( ":I: Reaped %d%s\n",
                   pid,
                   _dict_daemon ? " IN CHILD": "" );
   }
}

static int start_daemon( void )
{
   pid_t pid;
   
   ++_dict_forks;
   switch ((pid = fork())) {
   case 0:
      ++_dict_daemon;
      break;
   case -1:
      log_info( ":E: Unable to fork daemon\n" );
      alarm(10);		/* Can't use sleep() here */
      pause();
      break;
   default:
      if (flg_test(LOG_SERVER)) log_info( ":I: Forked %d\n", pid );
      break;
   }
   return pid;
}

static void handler( int sig )
{
   const char *name = NULL;
   time_t     t;
   
   switch (sig) {
   case SIGHUP:  name = "SIGHUP";  break;
   case SIGINT:  name = "SIGINT";  break;
   case SIGQUIT: name = "SIGQUIT"; break;
   case SIGILL:  name = "SIGILL";  break;
   case SIGTRAP: name = "SIGTRAP"; break;
   case SIGTERM: name = "SIGTERM"; break;
   case SIGPIPE: name = "SIGPIPE"; break;
   case SIGALRM: name = "SIGALRM"; break;
   }

   if (_dict_daemon) {
      daemon_terminate( sig, name );
   } else {
      tim_stop( "dictd" );
      if (sig == SIGALRM && _dict_markTime > 0) {
	 time(&t);
	 log_info( ":T: %24.24s; %d/%d %sr %su %ss\n",
		   ctime(&t),
		   _dict_forks - _dict_reaps,
		   _dict_forks,
		   dict_format_time( tim_get_real( "dictd" ) ),
		   dict_format_time( tim_get_user( "dictd" ) ),
		   dict_format_time( tim_get_system( "dictd" ) ) );
	 alarm(_dict_markTime);
	 return;
      }
      if (name) {
	 log_info( ":I: %s: c/f = %d/%d; %sr %su %ss\n",
		   name,
		   _dict_comparisons,
		   _dict_forks,
		   dict_format_time( tim_get_real( "dictd" ) ),
		   dict_format_time( tim_get_user( "dictd" ) ),
		   dict_format_time( tim_get_system( "dictd" ) ) );
      } else {
	 log_info( ":I: Signal %d: c/f = %d/%d; %sr %su %ss\n",
		   sig,
		   _dict_comparisons,
		   _dict_forks,
		   dict_format_time( tim_get_real( "dictd" ) ),
		   dict_format_time( tim_get_user( "dictd" ) ),
		   dict_format_time( tim_get_system( "dictd" ) ) );
      }
   }
   if (!dbg_test(DBG_NOFORK) || sig != SIGALRM)
      exit(sig+128);
}

static void setsig( int sig, void (*f)(int), int sa_flags )
{
   struct sigaction   sa;

   sa.sa_handler = f;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = sa_flags;
   sigaction(sig, &sa, NULL);
}

struct access_print_struct {
   FILE *s;
   int  offset;
};

static int access_print( const void *datum, void *arg )
{
   dictAccess                 *a     = (dictAccess *)datum;
   struct access_print_struct *aps   = (struct access_print_struct *)arg;
   FILE                       *s     = aps->s;
   int                        offset = aps->offset;
   int                        i;
   const char                 *desc;

   for (i = 0; i < offset; i++) fputc( ' ', s );
   switch (a->type) {
   case DICT_DENY:     desc = "deny";     break;
   case DICT_ALLOW:    desc = "allow";    break;
   case DICT_AUTHONLY: desc = "authonly"; break;
   case DICT_USER:     desc = "user";     break;
   case DICT_GROUP:    desc = "group";    break; /* Not implemented. */
   default:            desc = "unknown";  break;
   }
   fprintf( s, "%s %s\n", desc, a->spec );
   return 0;
}

static void acl_print( FILE *s, lst_List l, int offset)
{
   struct access_print_struct aps;
   int                        i;

   aps.s      = s;
   aps.offset = offset + 3;
   
   for (i = 0; i < offset; i++) fputc( ' ', s );
   fprintf( s, "access {\n" );
   lst_iterate_arg( l, access_print, &aps );
   for (i = 0; i < offset; i++) fputc( ' ', s );
   fprintf( s, "}\n" );
}

static int user_print( const void *key, const void *datum, void *arg )
{
   const char *username = (const char *)key;
   const char *secret   = (const char *)datum;
   FILE       *s        = (FILE *)arg;

   if (dbg_test(DBG_AUTH))
      fprintf( s, "user %s %s\n", username, secret );
   else
      fprintf( s, "user %s *\n", username );
   return 0;
}

static int config_print( const void *datum, void *arg )
{
   dictDatabase *db = (dictDatabase *)datum;
   FILE         *s  = (FILE *)arg;

   fprintf( s, "database %s {\n", db->databaseName );
   if (db->dataFilename)
      fprintf( s, "   data       %s\n", db->dataFilename );
   if (db->indexFilename)
      fprintf( s, "   index      %s\n", db->indexFilename );
   if (db->indexsuffixFilename)
      fprintf( s, "   index_suffix      %s\n", db->indexsuffixFilename );
   if (db->filter)
      fprintf( s, "   filter     %s\n", db->filter ); /* Not implemented. */
   if (db->prefilter)
      fprintf( s, "   prefilter  %s\n", db->prefilter );
   if (db->postfilter)
      fprintf( s, "   postfilter %s\n", db->postfilter );
   if (db->databaseShort)
      fprintf( s, "   name       %s\n", db->databaseShort );
   if (db->acl) acl_print( s, db->acl, 3 );
   fprintf( s, "}\n" );
   return 0;
}

static void dict_config_print( FILE *stream, dictConfig *c )
{
   FILE *s = stream ? stream : stderr;

   if (c->acl) acl_print( s, c->acl, 0 );
   lst_iterate_arg( c->dbl, config_print, s );
   if (c->usl) hsh_iterate_arg( c->usl, user_print, s );
}

static const char *get_entry_info( dictDatabase *db, const char *entryName )
{
   dictWord *dw;
   lst_List list = lst_create();
   char     *pt, *buf;
   
   if (!dict_search ( list, entryName, db, DICT_EXACT )) {
      lst_destroy( list );
      return NULL;
   }

   dw = lst_nth_get( list, 1 );

   buf = pt = dict_data_obtain( db, dw );

   while (*pt != '\n')
      ++pt;

   ++pt;

   while (*pt == ' ' || *pt == '\t')
      ++pt;

   pt[ strlen(pt) - 1 ] = '\0';

   dict_destroy_list( list );

   pt = xstrdup (pt);

   xfree (buf);

   return pt;
}

static int init_database( const void *datum )
{
   dictDatabase *db = (dictDatabase *)datum;

   PRINTF (DBG_INIT, (":I: Initializing '%s'\n", db->databaseName));
   PRINTF (DBG_INIT, (":I:   Opening indices\n"));
   db->index        = dict_index_open( db->indexFilename, 1, 0, 0 );
   db->index_suffix = dict_index_open(
       db->indexsuffixFilename,
       0, db->index->flag_utf8, db->index->flag_allchars);

   if (db->index_suffix){
      db->index_suffix->flag_utf8     = db->index->flag_utf8;
      db->index_suffix->flag_allchars = db->index->flag_allchars;
   }

   PRINTF (DBG_INIT, (":I:   Opening data\n"));
   db->data         = dict_data_open( db->dataFilename, 0 );

   if (!db->databaseShort)
      db->databaseShort = get_entry_info( db, DICT_SHORT_ENTRY_NAME );
   else if (*db->databaseShort == '@')
      db->databaseShort = get_entry_info( db, db->databaseShort + 1 );
   if (!db->databaseShort)
      db->databaseShort = xstrdup (db->databaseName);

   dict_plugin_open (db->index, db);

   PRINTF(DBG_INIT,
	  (":I: %s \"%s\" initialized\n",db->databaseName,db->databaseShort));

   return 0;
}

static int close_database (const void *datum)
{
   dictDatabase  *db = (dictDatabase *)datum;

   dict_plugin_close (db -> index);

   dict_index_close (db->index);
   dict_index_close (db->index_suffix);

   dict_data_close (db->data);

   xfree ((void *) db -> databaseShort);

   return 0;
}

static int log_database_info( const void *datum )
{
   dictDatabase  *db = (dictDatabase *)datum;
   const char    *pt;
   unsigned long headwords = 0;

   for (pt = db->index->start; pt < db->index->end; pt++)
      if (*pt == '\n') ++headwords;
   db->index->headwords = headwords;
   
   log_info( ":I: %-12.12s %12lu %12lu %12lu %12lu\n",
	     db->databaseName, headwords,
	     db->index->size, db->data->size, db->data->length );

   return 0;
}

static void dict_ltdl_init ()
{
   if (lt_dlinit ())
      err_fatal( __FUNCTION__, "Can not initialize 'ltdl' library\n" );
}

static void dict_ltdl_close ()
{
   if (lt_dlexit ())
      err_fatal( __FUNCTION__, "Can not deinitialize 'ltdl' library\n" );
}

static void dict_init_databases( dictConfig *c )
{
   dict_ltdl_init ();

   lst_iterate( c->dbl, init_database );
   lst_iterate( c->dbl, log_database_info );
}

static void dict_close_databases (dictConfig *c)
{
   dictDatabase *db;
   dictAccess   *acl;

   while (lst_length (c -> dbl) > 0){
      db = (dictDatabase *) lst_pop (c -> dbl);
      close_database (db);
      xfree (db);
   }
   lst_destroy (c -> dbl);

   while (lst_length (c -> acl) > 0){
      acl = (dictAccess *) lst_pop (c->acl);
      xfree (acl);
   }
   lst_destroy (c -> acl);

   xfree (c);

   dict_ltdl_close ();
}

static int dump_def( const void *datum, void *arg )
{
   char         *buf;
   dictWord     *dw = (dictWord *)datum;
   dictDatabase *db = (dictDatabase *)arg;

   buf = dict_data_obtain( db, dw );

   printf( "%s\n", buf );
   xfree( buf );
   return 0;
}

static void dict_dump_defs( lst_List list, dictDatabase *db )
{
   lst_iterate_arg( list, dump_def, db );

   if (db -> index -> plugin)
      db -> index -> plugin -> dictdb_free (db -> index -> plugin -> data);
}

static const char *id_string( const char *id )
{
   static char buffer[BUFFERSIZE];

   sprintf( buffer, "%s", DICT_VERSION );
   
   return buffer;
}

const char *dict_get_banner( int shortFlag )
{
   static char    *shortBuffer = NULL;
   static char    *longBuffer = NULL;
   const char     *id = "$Id: dictd.c,v 1.49 2002/09/12 13:08:06 cheusov Exp $";
   struct utsname uts;
   
   if (shortFlag && shortBuffer) return shortBuffer;
   if (!shortFlag && longBuffer) return longBuffer;
   
   uname( &uts );
   
   shortBuffer = xmalloc(256);
   sprintf( shortBuffer, "%s %s", err_program_name(), id_string( id ) );
   
   longBuffer = xmalloc(256);
   sprintf( longBuffer,
	    "%s %s/rf on %s %s", err_program_name(), id_string( id ),
	    uts.sysname,
	    uts.release );
   
   if (shortFlag) return shortBuffer;
   return longBuffer;
}

static void banner( void )
{
   printf( "%s\n", dict_get_banner(0) );
   printf( "Copyright 1997-2002 Rickard E. Faith (faith@dict.org)\n" );
}

static void license( void )
{
   static const char *license_msg[] = {
     "",
     "This program is free software; you can redistribute it and/or modify it",
     "under the terms of the GNU General Public License as published by the",
     "Free Software Foundation; either version 1, or (at your option) any",
     "later version.",
     "",
     "This program is distributed in the hope that it will be useful, but",
     "WITHOUT ANY WARRANTY; without even the implied warranty of",
     "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU",
     "General Public License for more details.",
     "",
     "You should have received a copy of the GNU General Public License along",
     "with this program; if not, write to the Free Software Foundation, Inc.,",
     "675 Mass Ave, Cambridge, MA 02139, USA.",
   0 };
   const char        **p = license_msg;
   
   banner();
   while (*p) printf( "   %s\n", *p++ );
}

static void help( void )
{
   static const char *help_msg[] = {
      "-h --help             give this help",
      "   --license          display software license",
      "-v --verbose          verbose mode",
      "-V --version          display version number",
      "-p --port <port>      port number",
      "   --delay <seconds>  client timeout in seconds",
      "   --depth <length>   TCP/IP queue depth",
      "   --limit <children> maximum simultaneous children",
      "-c --config <file>    configuration file",
      "-l --log <option>     select logging option",
      "-s --syslog           log via syslog(3)",
      "-L --logfile <file>   log via specified file",
      "-m --mark <minutes>   how often should a timestamp be logged",
      "   --facility <fac>   set syslog logging facility",
      "-d --debug <option>   select debug option",
      "-t --test <word>      self test -- lookup word",
      "   --ftest <file>     self test -- lookup all words in file",
"   --test-strategy <strategy>   search strategy for --test and --ftest.\n\
                                the default is 'exact'",
"-f --force            force startup even if daemon running",
"   --locale <locale>  specifies the locale used for searching.\n\
                      if no locale is specified, the \"C\" locale is used.\n",
"   --no-mmap          do not use mmap() function and read entire files\n\
                      into memory instead.\n",
      0 };
   const char        **p = help_msg;

   banner();
   while (*p) printf( "%s\n", *p++ );
}

static void set_minimal( void )
{
   flg_set(flg_name(LOG_FOUND));
   flg_set(flg_name(LOG_NOTFOUND));
   flg_set(flg_name(LOG_STATS));
   flg_set(flg_name(LOG_CLIENT));
   flg_set(flg_name(LOG_AUTH));
   flg_set("-min");
}

static void release_root_privileges( void )
/* At the spring 1999 Linux Expo in Raleigh, Rik Faith told me that he
 * did not want dictd to be allowed to run as root for any reason.
 * This patch irrevocably releases root privileges.  -- Kirk Hilliard
 *
 * Updated to set the user to `dictd' if that user exists on the
 * system; if user dictd doesn't exist, it sets the user to `nobody'.
 * -- Bob Hilliard
 */
{
   if (geteuid() == 0) {
      struct passwd *pwd;
      if ((pwd = getpwnam("dictd"))) {
         setgid(pwd->pw_gid);
         initgroups("dictd",pwd->pw_gid);
         setuid(pwd->pw_uid);
      } else if ((pwd = getpwnam("nobody"))) {
         setgid(pwd->pw_gid);
         initgroups("nobody",pwd->pw_gid);
         setuid(pwd->pw_uid);
      } else {
         setgid(GID_NOGROUP);
         initgroups("nobody", GID_NOGROUP);
         setuid(UID_NOBODY);
      }
   }
}

/* Perform sanity checks that are often problems for people trying to
 * get dictd running.  Do this early, before we detach from the
 * console. */
static void sanity(const char *configFile)
{
   int           fail = 0;
   struct passwd *pw = NULL;
   struct group  *gr = NULL;

   if (access(configFile,R_OK)) {
      log_info(":E: %s is not readable (config file)\n", configFile);
      ++fail;
   }
   if (!DictConfig->dbl) {
      log_info(":E: no databases have been defined\n");
      log_info(":E: check %s or use -c\n", configFile);
      ++fail;
   }
   if (DictConfig->dbl) {
      lst_Position p;
      dictDatabase *e;
      LST_ITERATE(DictConfig->dbl, p, e) {
           if (access(e->indexFilename, R_OK)) {
              log_info(":E: %s is not readable (index file)\n",
                       e->indexFilename);
              ++fail;
           }
           if (access(e->dataFilename, R_OK)) {
              log_info(":E: %s is not readable (data file)\n",
                       e->dataFilename);
              ++fail;
           }
       }
   }
   if (fail) {
      pw = getpwuid (geteuid ());
      gr = getgrgid (getegid ());

      log_info(":E: for security, this program will not run as root.\n");
      log_info(":E: if started as root, this program will change"
               " to \"dictd\" or \"nobody\".\n");
      log_info(":E: currently running as user %d/%s, group %d/%s\n",
               geteuid(), pw && pw->pw_name ? pw->pw_name : "?",
               getegid(), gr && gr->gr_name ? gr->gr_name : "?");
      log_info(":E: config and db files must be readable by that user\n");
      err_fatal(__FUNCTION__, ":E: terminating due to errors\n");
   }
}

static void set_utf8_mode (const char *locale)
{
   char *locale_copy;
   locale_copy = strdup (locale);
   strlwr_8bit (locale_copy);

   utf8_mode =
       strstr (locale_copy, "utf-8") ||
       strstr (locale_copy, "utf8");

   free (locale_copy);
}

static void init (const char *fn)
{
   maa_init (fn);
}

static void deinit ()
{
   /*
     tim_stop ("dictd");
     maa_shutdown ();
   */
   src_destroy ();
   str_destroy ();
}

int main( int argc, char **argv, char **envp )
{
   int                childSocket;
   int                masterSocket;
   struct sockaddr_in csin;
   int                c;
   time_t             startTime;
   int                word_len;
   int                alen         = sizeof(csin);
   const char         *service     = DICT_DEFAULT_SERVICE;
   const char         *configFile  = DICT_CONFIG_PATH DICTD_CONFIG_NAME;
   int                detach       = 1;
   const char         *testWord    = NULL;
   const char         *testFile    = NULL;
   const char         *logFile     = NULL;
   int                delay        = DICT_DEFAULT_DELAY;
   int                depth        = DICT_QUEUE_DEPTH;
   int                useSyslog    = 0;
   int                logOptions   = 0;
   int                forceStartup = 0;
   const char         *locale      = "C";
   int                i;

   const char         *strategy_arg= "exact";
   int                 strategy;

   struct option      longopts[]   = {
      { "verbose",  0, 0, 'v' },
      { "version",  0, 0, 'V' },
      { "debug",    1, 0, 'd' },
      { "port",     1, 0, 'p' },
      { "config",   1, 0, 'c' },
      { "help",     0, 0, 'h' },
      { "license",  0, 0, 500 },
      { "test",     1, 0, 't' },
      { "ftest",    1, 0, 501 },
      { "log",      1, 0, 'l' },
      { "logfile",  1, 0, 'L' },
      { "syslog",   0, 0, 's' },
      { "mark",     1, 0, 'm' },
      { "delay",    1, 0, 502 },
      { "depth",    1, 0, 503 },
      { "limit",    1, 0, 504 },
      { "facility", 1, 0, 505 },
      { "force",    1, 0, 'f' },
      { "locale",           1, 0, 506 },
      { "test-strategy",    1, 0, 507 },
      { "no-mmap",          0, 0, 508 },
      { 0,                  0, 0, 0  }
   };

   release_root_privileges();
   init(argv[0]);

   flg_register( LOG_SERVER,    "server" );
   flg_register( LOG_CONNECT,   "connect" );
   flg_register( LOG_STATS,     "stats" );
   flg_register( LOG_COMMAND,   "command" );
   flg_register( LOG_FOUND,     "found" );
   flg_register( LOG_NOTFOUND,  "notfound" );
   flg_register( LOG_CLIENT,    "client" );
   flg_register( LOG_HOST,      "host" );
   flg_register( LOG_TIMESTAMP, "timestamp" );
   flg_register( LOG_MIN,       "min" );
   flg_register( LOG_AUTH,      "auth" );

   dbg_register( DBG_VERBOSE,  "verbose" );
   dbg_register( DBG_UNZIP,    "unzip" );
   dbg_register( DBG_SCAN,     "scan" );
   dbg_register( DBG_PARSE,    "parse" );
   dbg_register( DBG_SEARCH,   "search" );
   dbg_register( DBG_INIT,     "init" );
   dbg_register( DBG_PORT,     "port" );
   dbg_register( DBG_LEV,      "lev" );
   dbg_register( DBG_AUTH,     "auth" );
   dbg_register( DBG_NODETACH, "nodetach" );
   dbg_register( DBG_NOFORK,   "nofork" );
   dbg_register( DBG_ALT,      "alt" );

   while ((c = getopt_long( argc, argv,
			    "vVd:p:c:hL:t:l:sm:f", longopts, NULL )) != EOF)
      switch (c) {
                                /* Remember to copy optarg since we're
                                   going to destroy argv soon... */
      case 'v': dbg_set( "verbose" );                     break;
      case 'V': banner(); exit(1);                        break;
      case 'd': dbg_set( optarg );                        break;
      case 'p': service = str_copy(optarg);               break;
      case 'c': configFile = str_copy(optarg);            break;
      case 't': testWord = str_copy(optarg);              break;
      case 'L': logFile = str_copy(optarg);               break;
      case 's': ++useSyslog;                              break;
      case 'm': _dict_markTime = 60*atoi(optarg);         break;
      case 'f': ++forceStartup;                           break;
      case 'l':
	 ++logOptions;
	 flg_set( optarg );
	 if (flg_test(LOG_MIN)) set_minimal();
	 break;
      case 500: license(); exit(1);                       break;
      case 501: testFile = str_copy(optarg);              break;
      case 502: delay = atoi(optarg);                     break;
      case 503: depth = atoi(optarg);                     break;
      case 504: _dict_daemon_limit = atoi(optarg);        break;
      case 505: ++useSyslog; log_set_facility(optarg);    break;
      case 506: locale = optarg;                          break;
      case 507: strategy_arg = optarg;                    break;
      case 508: mmap_mode = 0;                            break;
      case 'h':
      default:  help(); exit(0);                          break;
      }

   strategy = lookup_strategy (strategy_arg);
   if (-1 == strategy){
      fprintf (stderr, "'%s' is invalid strategy\n", strategy_arg);
      fprintf (stderr, "available ones are:\n");
      for (i = 0; i < get_strategies_count (); ++i){
	  fprintf (
	      stderr, "  %15s : %s\n",
	      get_strategies () [i].name, get_strategies () [i].description);
      }
      exit (1);
   }

   if (dbg_test(DBG_NOFORK))    dbg_set_flag( DBG_NODETACH);
   if (dbg_test(DBG_NODETACH))  detach = 0;
   if (dbg_test(DBG_PARSE))     prs_set_debug(1);
   if (dbg_test(DBG_SCAN))      yy_flex_debug = 1;
   else                         yy_flex_debug = 0;
   if (flg_test(LOG_TIMESTAMP)) log_option( LOG_OPTION_FULL );
   else                         log_option( LOG_OPTION_NO_FULL );

   set_utf8_mode (locale);
   if (!setlocale(LC_ALL, locale)){
	   fprintf (stderr, "ivalid locale '%s'\n", locale);
	   exit (2);
   }

   time(&startTime);
   tim_start( "dictd" );
   alarm(_dict_markTime);

   DictConfig = xmalloc(sizeof(struct dictConfig));
   memset( DictConfig, 0, sizeof( struct dictConfig ) );
   if (!access(configFile,R_OK))
      prs_file_nocpp( configFile );


                                /* Open up logs for sanity check */
   if (logFile)   log_file( "dictd", logFile );
   if (useSyslog) log_syslog( "dictd" );
   log_stream( "dictd", stderr );
   sanity(configFile);
   log_close();

   
   if (testWord) {		/* stand-alone test mode */
      lst_List list = lst_create();

      dict_config_print( NULL, DictConfig );
      dict_init_databases( DictConfig );
      if (dict_search (
	 list,
	 testWord,
	 lst_nth_get( DictConfig->dbl, 1 ),
	 strategy ))
      {
	 if (dbg_test(DBG_VERBOSE))
	    dict_dump_list( list );

	 dict_dump_defs( list, lst_nth_get( DictConfig->dbl, 1 ) );
	 dict_destroy_list( list );
      } else {
	 printf( "No match\n" );
      }
      fprintf( stderr, "%d comparisons\n", _dict_comparisons );

      dict_close_databases (DictConfig);

      deinit ();

      exit( 0 );
   }

   if (testFile) {
      FILE         *str;
      char         buf[1024], *pt;
      dictDatabase *db = lst_nth_get(DictConfig->dbl, 1);
      int          words = 0;

      if (!(str = fopen(testFile,"r")))
	 err_fatal_errno( "Cannot open \"%s\" for read\n", testFile );

      dict_config_print( NULL, DictConfig );
      dict_init_databases( DictConfig );
      while (fgets(buf,1024,str)) {
         lst_List list = lst_create();


         word_len = strlen( buf );
         if (word_len > 0){
            if ('\n' == buf [word_len - 1]){
               buf [word_len - 1] = '\0';
            }
         }

         if ((pt = strchr(buf, '\t')))
	    *pt = '\0'; /* stop at tab */

         if (buf[0]){
	    ++words;
/*	    fprintf( stderr, "searching for word: \"%s\" size: %i\n", buf, strlen( buf ));*/
            if (dict_search ( list, buf, db, strategy )) {
	       if (dbg_test(DBG_VERBOSE))
		  dict_dump_list( list );

	       dict_dump_defs( list, db );
            } else {
               fprintf( stderr, "*************** No match for \"%s\"\n", buf );
            }
         }
         dict_destroy_list( list );
         if (words && !(words % 1000))
            fprintf( stderr,
                     "%d comparisons, %d words\n", _dict_comparisons, words );
      }

      fprintf( stderr,
	       "%d comparisons, %d words\n", _dict_comparisons, words );
      fclose( str);

      dict_close_databases (DictConfig);

      deinit ();

      exit(0);
      /* Comparisons:
	 P5/133
	 1878064 comparisons, 113955 words
	 39:18.72u 1.480s 55:20.27 71%
	 */

	
   }

   setsig(SIGCHLD, reaper, SA_RESTART);
   setsig(SIGHUP,  handler, 0);
   if (!dbg_test(DBG_NOFORK))
      setsig(SIGINT,  handler, 0);
   setsig(SIGQUIT, handler, 0);
   setsig(SIGILL,  handler, 0);
   setsig(SIGTRAP, handler, 0);
   setsig(SIGTERM, handler, 0);
   setsig(SIGPIPE, handler, 0);
   setsig(SIGALRM, handler, SA_RESTART);

   fflush(stdout);
   fflush(stderr);

   if (detach) net_detach();

                                /* Re-open logs for logging */
   if (logFile)   log_file( "dictd", logFile );
   if (useSyslog) log_syslog( "dictd" );
   if (!detach)   log_stream( "dictd", stderr );
   if ((logFile || useSyslog || !detach) && !logOptions) set_minimal();

   log_info(":I: %d starting %s %24.24s\n",
	    getpid(), dict_get_banner(0), ctime(&startTime));
   if (strcmp(locale, "C")) log_info(":I: using locale \"%s\"\n", locale);

   masterSocket = net_open_tcp( service, depth );
   
   if (dbg_test(DBG_VERBOSE)) dict_config_print( NULL, DictConfig );
   dict_init_databases( DictConfig );

   
   dict_initsetproctitle(argc, argv, envp);

   for (;;) {
      dict_setproctitle( "%s: %d/%d",
			 dict_get_banner(1),
			 _dict_forks - _dict_reaps,
			 _dict_forks );
      if (flg_test(LOG_SERVER))
         log_info( ":I: %d accepting on %s\n", getpid(), service );
      if ((childSocket = accept(masterSocket,
				(struct sockaddr *)&csin, &alen)) < 0) {
	 if (errno == EINTR) continue;
#ifdef __linux__
				/* Linux seems to return more types of
                                   errors than other OSs. */
	 if (errno == ETIMEDOUT
	     || errno == ECONNRESET
	     || errno == EHOSTUNREACH
	     || errno == ENETUNREACH) continue;
	 log_info( ":E: can't accept: errno = %d: %s\n",
		   errno, strerror(errno) );
#else
	 err_fatal_errno( __FUNCTION__, ":E: can't accept" );
#endif
      }

      if (_dict_daemon || dbg_test(DBG_NOFORK)) {
	 dict_daemon(childSocket,&csin,&argv,delay,0);
      } else {
	 if (_dict_forks - _dict_reaps < _dict_daemon_limit) {
	    if (!start_daemon()) { /* child */
	       alarm(0);
	       dict_daemon(childSocket,&csin,&argv,delay,0);
	       exit(0);
	    } else {		   /* parent */
	       close(childSocket);
	    }
	 } else {
	    dict_daemon(childSocket,&csin,&argv,delay,1);
	 }
      }
   }

   dict_close_databases (DictConfig);

   deinit ();
}
