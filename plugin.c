/* index.c -- 
 * Created: Sat Mar 15 16:47:42 2003 by Aleksey Cheusov <vle@gmx.net>
 * Copyright 1996, 1997, 1998, 2000, 2002 Rickard E. Faith (faith@dict.org)
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
 * $Id: plugin.c,v 1.2 2003/04/10 18:52:32 cheusov Exp $
 * 
 */

#include "plugin.h"
#include "strategy.h"
#include "data.h"
#include "index.h"
#include "dictd.h"

#ifndef HAVE_DLFCN_H
#include <ltdl.h>
#else
#include <dlfcn.h>
#endif

#include <maa.h>
#include <stdlib.h>
#include <ctype.h>

int dict_search_plugin (
   lst_List l,
   const char *const word,
   const dictDatabase *database,
   int strategy,
   int *extra_result,
   const dictPluginData **extra_data,
   int *extra_data_size)
{
   int ret;
   int                  failed = 0;
   const char * const * defs;
   const int          * defs_sizes;
   int                  defs_count;
   const char         * err_msg;
   int                  i;
   dictWord           * def;
   int                  len;

   assert (database);
   assert (database -> plugin);

   PRINTF (DBG_SEARCH, (":S:     searching\n"));

   failed = database -> plugin -> dictdb_search (
      database -> plugin -> data,
      word, -1,
      strategy,
      &ret,
      extra_data, extra_data_size,
      &defs, &defs_sizes, &defs_count);

   if (extra_result)
      *extra_result = ret;

   if (failed){
      err_msg = database -> plugin -> dictdb_error (
	 database -> plugin -> data);

      PRINTF (DBG_SEARCH, (":E: Plugin failed: %s\n", err_msg ? err_msg : ""));
   }else{
      switch (ret){
      case DICT_PLUGIN_RESULT_FOUND:
	 PRINTF (DBG_SEARCH, (":S:     found %i definitions\n", defs_count));
	 break;
      case DICT_PLUGIN_RESULT_NOTFOUND:
	 PRINTF (DBG_SEARCH, (":S:     not found\n"));
	 return 0;
      case DICT_PLUGIN_RESULT_EXIT:
	 PRINTF (DBG_SEARCH, (":S:     exiting\n"));
	 return 0;
      case DICT_PLUGIN_RESULT_PREPROCESS:
	 PRINTF (DBG_SEARCH, (":S:     preprocessing\n"));
	 break;
      default:
	 err_fatal (__FUNCTION__, "invalid pligin's exit status\n");
      }

      for (i = 0; i < defs_count; ++i){
	 def = xmalloc (sizeof (dictWord));
	 memset (def, 0, sizeof (*def));

	 def -> database = database;
	 def -> start    = def -> end = 0;

	 len = defs_sizes [i];
	 if (-1 == len)
	    len = strlen (defs [i]);

	 if (
	    strategy & DICT_MATCH_MASK &&
	    ret != DICT_PLUGIN_RESULT_PREPROCESS)
	 {
	    def -> word     = xstrdup (defs [i]);
	    def -> def      = def -> word;
	    def -> def_size = -1;
	 }else{
	    def -> word     = xstrdup (word);
	    def -> def      = defs [i];
	    def -> def_size = len;
	 }

	 lst_push (l, def);
      }

      return defs_count;
   }

   return 0;
}

/* reads data without headword 00-... */
static char *dict_plugin_data (const dictDatabase *db, const dictWord *dw)
{
   char *buf = dict_data_obtain (db, dw);
   char *p = buf;
   int len;

   assert (db);
   assert (db -> index);

   if (!strncmp (p, DICT_ENTRY_PLUGIN_DATA, strlen (DICT_ENTRY_PLUGIN_DATA))){
      while (*p != '\n')
	 ++p;
   }

   while (*p == '\n')
      ++p;

   len = strlen (p);

   while (len > 0 && p [len - 1] == '\n')
      --len;

   p [len] = 0;

   p = xstrdup (p);
   xfree (buf);

   return p;
}

static int plugin_initdata_set_data (
   dictPluginData *data, int data_size,
   const dictDatabase *db)
{
   char *plugin_data;
   int ret = 0;
   lst_List list;
   dictWord *dw;

   if (!db -> index)
      return 0;

   if (data_size <= 0)
      err_fatal (__FUNCTION__, "invalid initial array size");

   list = lst_create ();

   ret = dict_search_database_ (
      list, DICT_ENTRY_PLUGIN_DATA, db, DICT_EXACT);

   if (0 == ret){
      lst_destroy (list);
      return 0;
   }

   dw = (dictWord *) lst_pop (list);
   plugin_data = dict_plugin_data (db, dw);

   dict_destroy_datum (dw);
   if (2 == ret)
      dict_destroy_datum (lst_pop (list));

   data -> id   = DICT_PLUGIN_INITDATA_DICT;
   data -> data = plugin_data;
   data -> size = -1;

   lst_destroy (list);

   return 1;
}

static int plugin_initdata_set_dbnames (dictPluginData *data, int data_size)
{
   const dictDatabase *db;
   int count;
   int i;

   if (data_size <= 0)
      err_fatal (__FUNCTION__, "too small initial array");

   count = lst_length (DictConfig -> dbl);
   if (count == 0)
      return 0;

   if (count > data_size)
      err_fatal (__FUNCTION__, "too small initial array");

   for (i = 1; i <= count; ++i){
      db = (const dictDatabase *)(lst_nth_get (DictConfig -> dbl, i));

      data -> id   = DICT_PLUGIN_INITDATA_DBNAME;
      if (db -> databaseName){
	 data -> size = strlen (db -> databaseName);
	 data -> data = xstrdup (db -> databaseName);
      }else{
	 data -> size = 0;
	 data -> data = NULL;
      }

      ++data;
   }

   return count;
}

static int plugin_initdata_set_stratnames (dictPluginData *data, int data_size)
{
   dictStrategy **strats;
   int count;
   int ret = 0;
   int i;
   dictPluginData_strategy datum;

   if (data_size <= 0)
      err_fatal (__FUNCTION__, "too small initial array");

   count = get_strategy_count ();
   assert (count > 0);

   strats = get_strategies ();

   for (i = 0; i < count; ++i){
      data -> id   = DICT_PLUGIN_INITDATA_STRATEGY;

      if (
	 strlen (strats [i] -> name) + 1 >
	 sizeof (datum.name))
      {
	 err_fatal (__FUNCTION__, "too small initial array");
      }

      datum.number = strats [i] -> number;
      strcpy (datum.name, strats [i] -> name);

      data -> size = sizeof (datum);
      data -> data = xmalloc (sizeof (datum));

      memcpy ((void *) data -> data, &datum, sizeof (datum));

      ++data;
      ++ret;
   }

   return ret;
}

/* all dict [i]->data are xmalloc'd*/
static int plugin_initdata_set (
   dictPluginData *data, int data_size,
   const dictDatabase *db)
{
   int count = 0;
   dictPluginData *p = data;

   count = plugin_initdata_set_dbnames (data, data_size);
   data      += count;
   data_size -= count;

   count = plugin_initdata_set_stratnames (data, data_size);
   data      += count;
   data_size -= count;

   count = plugin_initdata_set_data (data, data_size, db);
   data      += count;
   data_size -= count;

   return data - p;
}

static void plugin_init_data_free (
   dictPluginData *data, int data_size)
{
   int i=0;

   for (i = 0; i < data_size; ++i){
      if (data -> data)
	 xfree ((void *) data -> data);

      ++data;
   }
}

/* Reads plugin's file name from .dict file */
/* do not free() returned value*/
static char *dict_plugin_filename (
   const dictDatabase *db,
   const dictWord *dw)
{
   static char filename [FILENAME_MAX];

   char *buf = dict_data_obtain (db, dw);
   char *p = buf;
   int len;

   if (!strncmp (p, DICT_ENTRY_PLUGIN, strlen (DICT_ENTRY_PLUGIN))){
      while (*p != '\n')
	 ++p;
   }

   while (*p == '\n' || isspace ((unsigned char) *p))
      ++p;

   len = strlen (p);

   while (
      len > 0 &&
      (p [len - 1] == '\n' || isspace ((unsigned char) p [len - 1])))
   {
      --len;
   }

   p [len] = 0;

   if (p [0] != '.' && p [0] != '/'){
      if (sizeof (filename) < strlen (DICT_PLUGIN_PATH) + strlen (p) + 1)
	 err_fatal (__FUNCTION__, "too small initial array\n");

      strcpy (filename, DICT_PLUGIN_PATH);
      strcat (filename, p);
   }else{
      strncpy (filename, p, sizeof (filename) - 1);
   }

   filename [sizeof (filename) - 1] = 0;

   xfree (buf);

   return filename;
}


static void dict_plugin_test (dictPlugin *plugin, int version, int ret)
{
   const char *err_msg = NULL;

   if (ret){
      err_msg = plugin -> dictdb_error (
	 plugin -> data);

      if (err_msg){
	 err_fatal (
	    __FUNCTION__,
	    "%s\n",
	    plugin -> dictdb_error (plugin -> data));
      }else{
	 err_fatal (
	    __FUNCTION__,
	    "Error code %i\n", ret);
      }
   }

   switch (version){
   case 0:
      break;
/*
   case 1:
      if (!i -> plugin -> dictdb_set)
	 err_fatal (__FUNCTION__, "'%s' function is not found\n", DICT_PLUGINFUN_SET);
      break;
*/
   default:
      err_fatal (__FUNCTION__, "Invalid version returned by plugin\n");
   }
}

static void dict_plugin_dlsym (dictPlugin *plugin)
{
   PRINTF(DBG_INIT, (":I:     getting functions addresses\n"));

   plugin -> dictdb_open   =
      lt_dlsym (plugin -> handle, DICT_PLUGINFUN_OPEN);
   plugin -> dictdb_free   =
      lt_dlsym (plugin -> handle, DICT_PLUGINFUN_FREE);
   plugin -> dictdb_search =
      lt_dlsym (plugin -> handle, DICT_PLUGINFUN_SEARCH);
   plugin -> dictdb_close  =
      lt_dlsym (plugin -> handle, DICT_PLUGINFUN_CLOSE);
   plugin -> dictdb_error  =
      lt_dlsym (plugin -> handle, DICT_PLUGINFUN_ERROR);
   plugin -> dictdb_set   =
      lt_dlsym (plugin -> handle, DICT_PLUGINFUN_SET);

   if (!plugin -> dictdb_open ||
       !plugin -> dictdb_search ||
       !plugin -> dictdb_free ||
       !plugin -> dictdb_error ||
       !plugin -> dictdb_close)
   {
      PRINTF(DBG_INIT, (":I:     faild\n"));
      exit (1);
   }
}

static dictPlugin *create_plugin (
   const char *plugin_filename,
   const dictPluginData *plugin_init_data,
   int plugin_init_data_size)
{
   dictPlugin *plugin;
   int ret;
   int version;

   PRINTF(DBG_INIT, (":I:   Initializing plugin '%s'\n", plugin_filename));

   plugin = xmalloc (sizeof (dictPlugin));
   memset (plugin, 0, sizeof (dictPlugin));

   PRINTF(DBG_INIT, (":I:     opening plugin\n"));
   plugin -> handle = lt_dlopen (plugin_filename);
   if (!plugin -> handle){
      PRINTF(DBG_INIT, (":I:     faild\n"));
      exit (1);
   }

   dict_plugin_dlsym (plugin);

   PRINTF(DBG_INIT, (":I:     initializing plugin\n"));
   ret = plugin -> dictdb_open (
      plugin_init_data, plugin_init_data_size, &version, &plugin -> data);

   dict_plugin_test (plugin, version, ret);

   return plugin;
}

int dict_plugin_init (dictDatabase *db)
{
   int ret = 0;
   lst_List list;
   const char *plugin_filename;
   dictWord *dw;

   dictPluginData init_data [3000];
   int init_data_size;

   init_data_size = plugin_initdata_set (
      init_data, sizeof (init_data)/sizeof (init_data [0]),
      db);

   if (db -> plugin_db){
      init_data [init_data_size].id   = DICT_PLUGIN_INITDATA_DICT;
      init_data [init_data_size].data = db -> plugin_data;
      init_data [init_data_size].size = -1;

      db -> plugin = create_plugin (
	 db -> pluginFilename,
	 init_data, init_data_size + 1);
   }else{
      if (db -> index){
	 list = lst_create ();

	 ret = dict_search_database_ (list, DICT_ENTRY_PLUGIN, db, DICT_EXACT);
	 switch (ret){
	 case 1: case 2:
	    dw = (dictWord *) lst_pop (list);

	    plugin_filename = dict_plugin_filename (db, dw);

	    dict_destroy_datum (dw);
	    if (2 == ret)
	       dict_destroy_datum (lst_pop (list));

	    db -> plugin = create_plugin (
	       plugin_filename, init_data, init_data_size);

	    break;
	 case 0:
	    break;
	 default:
	    err_internal( __FUNCTION__, "Corrupted .index file'\n" );
	 }

	 lst_destroy (list);
      }
   }

   plugin_init_data_free (init_data, init_data_size);

   return 0;
}

void dict_plugin_destroy ( dictDatabase *db )
{
   int ret;

   if (!db)
      return;

   if (!db -> plugin)
      return;

   if (db -> plugin -> dictdb_close){
      ret = db -> plugin -> dictdb_close (db -> plugin -> data);
      if (ret){
	 PRINTF(DBG_INIT, ("exiting plugin failed"));
	 exit (1);
      }
   }

   ret = lt_dlclose (db -> plugin -> handle);
   if (ret)
      PRINTF(DBG_INIT, ("%s", lt_dlerror ()));

   xfree (db -> plugin);
   db -> plugin = NULL;
}

static int call_dictdb_free1 (const void *datum)
{
   const dictWord     *dw = (dictWord *)datum;
   const dictDatabase *db = dw -> database;

   if (db -> plugin)
      db -> plugin -> dictdb_free (db -> plugin -> data);

   return 0;
}

void call_dictdb_free (lst_List list)
{
   lst_iterate (list, call_dictdb_free1);
}
