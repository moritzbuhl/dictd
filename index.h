/* index.h -- 
 * Created: Sat Mar 15 18:10:58 2003 by Aleksey Cheusov <vle@gmx.net>
 * Copyright 1994-2003 Rickard E. Faith (faith@dict.org)
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
 * $Id: index.h,v 1.1 2003/03/19 16:43:27 cheusov Exp $
 */

#ifndef _INDEX_H_
#define _INDEX_H_

#include "dictP.h"
#include "defs.h"

/* initialize .index file */
extern dictIndex  *dict_index_open(
   const char *filename,
   int init_flags, int flag_utf8, int flag_allchars );
/* */
extern void dict_index_close (dictIndex *i);

//extern const char *dict_index_search (
//   const char *word,
//   dictIndex *idx);
extern int dict_search_database_ (
   lst_List l,
   const char *const word,
   const dictDatabase *database,
   int strategy );
extern int dict_search (
   lst_List l,
   const char *word,
   const dictDatabase *database, int strategy,
   int *extra_result,                  /* may be NULL */
   const dictPluginData **extra_data,  /* may be NULL */
   int *extra_data_size);              /* may be NULL */
extern int dict_search_databases (
   lst_List *l,
   lst_Position db_pos,
   const char *databaseName, const char *word, int strategy,
   int *db_found);

extern int        utf8_mode;
extern int        bit8_mode;

#endif /* _INDEX_H_ */