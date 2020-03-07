/* index.c -- 
 * Created: Wed Oct  9 14:52:23 1996 by faith@dict.org
 * Revised: Tue Apr 23 09:14:43 2002 by faith@dict.org
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
 * $Id: index.c,v 1.32 2002/09/12 13:08:06 cheusov Exp $
 * 
 */

#include "dictzip.h"
#include "regex.h"
#include "utf8_ucs4.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <ctype.h>
#include <ltdl.h>
#include <stdio.h>

#define FIND_NEXT(pt,end) while (pt < end && *pt++ != '\n');
#define OPTSTART        1	/* Optimize search range for constant start */
#define MAXWORDLEN    512
#define BMH_THRESHOLD   3	/* When to start using Boyer-Moore-Hoorspool */

int utf8_mode;     /* dictd uses UTF-8 dictionaries */
int mmap_mode = 1; /* dictd uses mmap() function (the default) */

int _dict_comparisons;
static int isspacealnumtab[UCHAR_MAX + 1];
static int isspacealnumtab_allchars[UCHAR_MAX + 1];
static int char2indextab[UCHAR_MAX + 2];
static int index2chartab[UCHAR_MAX + 2];
static int chartab[UCHAR_MAX + 1];
static int charcount;
/* #define isspacealnum(x) (isspacealnumtab[(unsigned char)(x)]) */
#define c2i(x) (char2indextab[(unsigned char)(x)])
#define i2c(x) (index2chartab[(unsigned char)(x)])
#define c(x)   (((x) < charcount) ? chartab[(unsigned char)(x)] : 0)
#define altcompare(a,b,c) (1)

/*
  compares two 8bit strings (containing one character)
  according to locate
*/
static int dict_table_init_compare_strcoll (const void *a, const void *b)
{
    return strcoll(*(const char **)a, *(const char **)b);
}

/*
  compares two strings (containing one character)
  converting ASCII character to lower case.
*/
static int dict_table_init_compare_utf8 (const void *a, const void *b)
{
    int c1 = ** (const unsigned char **) a;
    int c2 = ** (const unsigned char **) b;

    if (c1 <= CHAR_MAX)
	c1 = tolower (c1);
    if (c2 <= CHAR_MAX)
	c2 = tolower (c2);

    return c1 - c2;
}

/*
  Copies alphanumeric and space characters converting them to lower case.
  Strings are represented in 8-bit character set.
*/
static int tolower_alnumspace_8bit (
   const char *src, char *dest,
   int allchars_mode)
{
   int c;

   for (; *src; ++src) {
      c = * (const unsigned char *) src;

      if (isspace( c )) {
         *dest++ = ' ';
      }else if (allchars_mode || isalnum( c )){
	 *dest++ = tolower (c);
      }
   }

   *dest = '\0';
   return 1;
}

/*
  Copies alphanumeric and space characters converting them to lower case.
  Strings are represented in UTF-8 character set.
*/
static int tolower_alnumspace_utf8 (
   const char *src, char *dest,
   int allchars_mode)
{
    wint_t      ucs4_char;

    while (src && src [0]){
	src = utf8_to_ucs4 (src, &ucs4_char);
	if (src){
	    if (iswspace (ucs4_char)){
		*dest++ = ' ';
	    }else if (allchars_mode || iswalnum (ucs4_char)){
		if (!ucs4_to_utf8 (towlower (ucs4_char), dest))
		    return 0;

		dest += strlen (dest);
	    }
	}
    }

    *dest = 0;

    return (src != NULL);
}

static void dict_table_init(void)
{
    int      i;
    unsigned char s[2 * UCHAR_MAX + 2];
    unsigned char *p[UCHAR_MAX + 1];

    for (i = 0; i <= UCHAR_MAX; i++) {
	if (isspace(i) || isalnum(i) || (utf8_mode && i >= 0x80)){
	    isspacealnumtab [i] = 1;
	}else{
	    isspacealnumtab [i] = 0;
	}
	isspacealnumtab_allchars [i] = 1;
    }
    isspacealnumtab['\t'] = isspacealnumtab['\n'] = 0; /* special */
    isspacealnumtab_allchars['\t'] = isspacealnumtab_allchars['\n'] = 0; /* special */

    for (i = 0; i <= UCHAR_MAX; i++){
	if (islower (i) || (utf8_mode && i >= 0xC0))
	    chartab[charcount++] = i;
    }

                                /* Populate an array with length-1 strings */
    for (i = 0; i <= UCHAR_MAX; i++) {
	if (!isupper (i)){
	    s[2 * i] = i;
	}else{
	    s[2 * i] = 0;
	}

	s[2 * i + 1] = '\0';
	p[i]         = &s[2 * i];
    }
                                /* Sort those strings in the locale */
    if (utf8_mode)
	qsort(p, UCHAR_MAX + 1, sizeof(p[0]), dict_table_init_compare_utf8);
    else
	qsort(p, UCHAR_MAX + 1, sizeof(p[0]), dict_table_init_compare_strcoll);

                                /* Extract our unordered arrays */
    for (i = 0; i <= UCHAR_MAX; i++) {
        char2indextab[*p[i]] = i;
        index2chartab[i]     = *p[i];
    }
    char2indextab[UCHAR_MAX + 1] = UCHAR_MAX;
    index2chartab[UCHAR_MAX + 1] = UCHAR_MAX; /* we may index here in  */

    if (dbg_test(DBG_SEARCH)) {
	for (i = 0; i <= UCHAR_MAX; ++i){
	    if (p [i][0] <= CHAR_MAX)
		printf ("sorted list: %s\n", p [i]);
	    else
		printf ("sorted list: %i\n", (unsigned char) p [i] [0]);
	}
    }

    if (dbg_test(DBG_SEARCH)) {
        for (i = 0; i < charcount; i++)
            printf("%03d %d ('%c')\n", i, c(i), c(i));
        for (i = 0; i <= UCHAR_MAX; i++)
            printf("c2i(%d/'%c') = %d; i2c(%d) = %d/'%c'\n",
                   i, (char) isgraph(i) ? i : '.',
		   c2i(i), c2i(i),
		   i2c(c2i(i)), (char) i2c(c2i(i)) ? i2c(c2i(i)) : '.');
    }
}

static int compare_allchars(
    const char *word,
    const char *start, const char *end )
{
   int c1, c2;
   int result;

   /* FIXME.  Optimize this inner loop. */
   while (*word && start < end && *start != '\t') {
//      if (!isspacealnum(*start)) {
//	 ++start;
//	 continue;
//      }
#if 0
      if (isspace( *start )) c2 = ' ';
      else                   c2 = * (unsigned char *) start;
      if (isspace( *word )) c1 = ' ';
      else                  c1 = * (unsigned char *) word;
#else
      c2 = * (unsigned char *) start;
      c1 = * (unsigned char *) word;
#endif
      if (c1 != c2) {
	    if (c1 < c2){
	       result = -2;
	    }else{
	       result = 1;
	    }
	 if (dbg_test(DBG_SEARCH)){
	    printf("   result = %d (%i != %i) \n", result, c1, c2);
	 }
         return result;
      }
      ++word;
      ++start;
   }

   PRINTF(DBG_SEARCH,("   result = %d\n",
		      *word ? 1 : ((*start != '\t') ? -1 : 0)));
   return  *word ? 1 : ((*start != '\t') ? -1 : 0);
}

/*
static int compare_utf8(
    const char *word,
    const char *start,
    const char *end )
{
   wint_t        c1, c2;
   int           result;
   char          s1 [7], s2 [7];

   while (*word && start < end && *start != '\t') {
      start = utf8_to_ucs4 (start, &c2);
      if (!start){
	 PRINTF(DBG_SEARCH,("   result = ERROR!!!\n"));
	 return 2;
      }
      if (!iswspace (c2) && !iswalnum (c2))
	 continue;
      c2 = towlower (c2);

      word = utf8_to_ucs4 (word, &c1);
      if (!word){
	 PRINTF(DBG_SEARCH,("   result = ERROR!!!\n"));
	 return 2;
      }
      c1 = towlower (c1);

      if (c1 != c2) {
	 ucs4_to_utf8 (c1, s1);
	 ucs4_to_utf8 (c2, s2);
	 result = strcmp (s1, s2) < 0 ? -2 : 1;
	 PRINTF(DBG_SEARCH,("   result = %d (%s != %s)\n", result, s1, s2));
         return result;
      }
   }

   if (start == end){
      PRINTF(DBG_SEARCH,("   result = ERROR!!!\n"));
      return 2;
   }

   if (*word){
       PRINTF(DBG_SEARCH,("   result = 1\n"));
       return 1;
   }

   while (*start != '\t'){
      start = utf8_to_ucs4 (start, &c2);
      if (!start){
	 PRINTF(DBG_SEARCH,("   result = ERROR!!!\n"));
	 return 2;
      }

      if (iswalnum (c2) || iswspace (c2)){
	 PRINTF(DBG_SEARCH,("   result = -1\n"));
	 return -1;
      }
   }

   PRINTF(DBG_SEARCH,("   result = 0\n"));
   return 0;
}
*/

static int compare_alnumspace(
    const char *word,
    const dictIndex *dbindex,
    const char *start, const char *end )
{
   int c1, c2;
   int result;

   assert (dbindex);

   /* FIXME.  Optimize this inner loop. */
   while (*word && start < end && *start != '\t') {
      if (!dbindex -> isspacealnum[* (const unsigned char *) start]) {
	 ++start;
	 continue;
      }
#if 0
      if (isspace( *start )) c2 = ' ';
      else                   c2 = tolower(* (unsigned char *) start);
      if (isspace( *word )) c1 = ' ';
      else                  c1 = tolower(* (unsigned char *) word);
#else
      c2 = tolower(* (unsigned char *) start);
      c1 = tolower(* (unsigned char *) word);
#endif
      if (c1 != c2) {
	 if (utf8_mode){
	    if (
	       (c1 <= CHAR_MAX ? c2i (c1) : c1) <
	       (c2 <= CHAR_MAX ? c2i (c2) : c2))
	    {
	       result = -2;
	    }else{
	       result = 1;
	    }
	 }else{
	    result = (c2i (c1) < c2i (c2) ? -2 : 1);
	 }
	 if (dbg_test(DBG_SEARCH)){
	    if (utf8_mode)
	       printf("   result = %d (%i != %i) \n", result, c1, c2);
	    else
	       printf("   result = %d ('%c' != '%c') \n", result, c1, c2);
	 }
         return result;
      }
      ++word;
      ++start;
   }

   while (
       *start != '\t' &&
       !dbindex -> isspacealnum[* (const unsigned char *) start])
   {
      ++start;
   }

   PRINTF(DBG_SEARCH,("   result = %d\n",
		      *word ? 1 : ((*start != '\t') ? -1 : 0)));
   return  *word ? 1 : ((*start != '\t') ? -1 : 0);
}

/* Compare:
   
   Return -2 if word <  word-pointed-to-by-start
          -1 if word prefix word-pointed-to-by-start
           0 if word == word-pointed-to-by-start
	   1 if word >  word-pointed-to-by-start
	   2 if some kind of error happened

   The comparison must be the same as "sort -df" phone directory order:
   ignore all characters except letters, digits, and blanks; fold upper
   case into the equivalent lowercase.

   word already has all of the illegal characters removed
*/

static int compare(
   const char *word,
   const dictIndex *dbindex,
   const char *start, const char *end )
{
   char       buf[80], *d;
   const char *s;

   if (dbg_test(DBG_SEARCH)) {
      for (
	 d = buf, s = start;
	 d - buf < sizeof (buf)-1 && s < end && *s != '\t';)
      {
	 *d++ = *s++;
      }

      *d = '\0';
      printf( "compare \"%s\" with \"%s\" (sizes: %i and %i)\n",
         word, buf, strlen( word ), strlen( buf ) );
   }

   ++_dict_comparisons;		/* counter for profiling */

   if (dbindex && (dbindex -> flag_allchars || dbindex -> flag_utf8)){
      return compare_allchars( word, start, end );
   }else{
      return compare_alnumspace( word, dbindex, start, end );
   }
}

static const char *binary_search(
    const char *word,
    const dictIndex *dbindex,
    const char *start, const char *end )
{
   const char *pt;

   PRINTF(DBG_SEARCH,("%s %p %p\n",word,start,end));

   pt = start + (end-start)/2;
   FIND_NEXT(pt,end);
   while (pt < end) {
      switch (compare( word, dbindex, pt, end )){
	 case -2: case -1: case 0:
	    end = pt;
	    break;
	 case 1:
	    start = pt;
	    break;
	 case  2:
	    return end;     /* ERROR!!! */
	 default:
	    assert (0);
      }
      PRINTF(DBG_SEARCH,("%s %p %p\n",word,start,end));
      pt = start + (end-start)/2;
      FIND_NEXT(pt,end);
   }

   return start;
}

static const char *binary_search_8bit(
    const char *word,
    const dictIndex *dbindex,
    const char *start, const char *end )
{
   char       buf[80], *d;
   const char *s;
   const char *pt;
   int cmp;

   PRINTF(DBG_SEARCH,("%s %p %p\n",word,start,end));

   pt = start + (end-start)/2;
   FIND_NEXT(pt,end);
   while (pt < end) {
      if (dbg_test(DBG_SEARCH)) {
         for (d = buf, s = pt; s < end && *s != '\t';) *d++ = *s++;
         *d = '\0';
         printf( "compare \"%s\" with \"%s\" (sizes: %i and %i)\n",
            word, buf, strlen( word ), strlen( buf ) );
      }

      if (
	 dbindex &&
	 (dbindex -> flag_utf8 || dbindex -> flag_allchars))
      {
	 cmp = compare_allchars ( word, pt, end );
      }else{
	 cmp = compare_alnumspace ( word, dbindex, pt, end );
      }

      switch (cmp){
	 case -2: case -1: case 0:
	    end = pt;
	    break;
	 case 1:
	    start = pt;
	    break;
	 case  2:
	    return end;     /* ERROR!!! */
	 default:
	    assert (0);
      }
      PRINTF(DBG_SEARCH,("%s %p %p\n",word,start,end));
      pt = start + (end-start)/2;
      FIND_NEXT(pt,end);
   }

   return start;
}

static const char *linear_search(
    const char *word,
    const dictIndex *dbindex,
    const char *start, const char *end )
{
   const char *pt;

   for (pt = start; pt < end;) {
      switch (compare( word, dbindex, pt, end )) {
      case -2: return NULL;	/* less than and not prefix */
      case -1:			/* prefix */
      case  0: return pt;	/* exact */
      case  1: break;           /* greater than */
      case  2: return NULL;     /* ERROR!!! */
      }
      FIND_NEXT(pt,end);
   }
   return NULL;
}

const char *dict_index_search( const char *word, dictIndex *idx )
{
   const char    *start;
   const char    *end;
#if OPTSTART
   int first, last;
#endif

   if (!idx)
      err_internal( __FUNCTION__, "No information on index file\n" );

   /* With optStart:
      17071 comparisons, 1000 words
      33946 comparisons, 2000 words

      Without optStart:
      20889 comparisons, 1000 words
      41668 comparisons, 2000 words

      Linear:
       594527 comparisons, 1000 words
      2097035 comparisons, 2000 words
   */

#if OPTSTART
   first   = * (const unsigned char *) word;
   last    = i2c(c2i(first)+1);

   if (dbg_test(DBG_SEARCH)) {
      if (!utf8_mode || (last <= CHAR_MAX && first <= CHAR_MAX))
	 printf("binary_search from %c to %c\n", first, last);
      else
	 printf("binary_search from %i to %i\n", first, last);
   }

   end   = idx->optStart [last];
   start = idx->optStart [first];
#else
   start = idx->start;
   end   = idx->end;
#endif
   if (end < start) end = idx->end;

   start = binary_search( word, idx, start, end );

   PRINTF(DBG_SEARCH,("binary_search returns %p\n",start));

   start = linear_search( word, idx, start, idx->end );

   PRINTF(DBG_SEARCH,("linear_search returns %p\n",start));

   return start;
}

static dictWord *dict_word_create(
    const char *entry,
    dictDatabase *database,
    dictIndex *dbindex)
{
   int        firstTab  = 0;
   int        secondTab = 0;
   int        newline   = 0;
   dictWord   *dw       = xmalloc( sizeof( struct dictWord ) );
   char       *buf;
   int        offset    = 0;
   int        state     = 0;
   const char *pt       = entry;
   char       *s, *d;
   
   for (;pt < dbindex->end && *pt != '\n'; pt++, offset++) {
      if (*pt == '\t') {
	 switch (++state) {
	 case 1: firstTab = offset;  break;
	 case 2: secondTab = offset; break;
	 default:
	    err_internal( __FUNCTION__,
			  "Too many tabs in index entry \"%*.*s\"\n",
			  secondTab, secondTab, entry );
	 }
      }
   }
   newline = offset;
   
   if (state != 2)
      err_internal( __FUNCTION__,
		    "Too few tabs in index entry \"%20.20s\"\n", entry );

   buf = alloca( newline + 1 );
   strncpy( buf, entry, newline );
   buf[firstTab] = buf[secondTab] = buf[newline] = '\0';

   dw->start    = b64_decode( buf + firstTab + 1 );
   dw->end      = b64_decode( buf + secondTab + 1 );
   dw->def      = NULL;
   dw->def_size = 0;
   dw->database = database;

				/* Apply quoting to word */
   dw->word     = xmalloc(strlen(buf) * 2 + 1);
   for (s = buf, d = (char *)dw->word; *s;) {
       switch (*s) {
       case '"':
       case '\\':
	   *d++ = '\\';
       default:
	   *d++ = *s++;
       }
   }
   *d = '\0';

   return dw;
}

static int dict_dump_datum( const void *datum )
{
   dictWord *dw = (dictWord *)datum;
   printf(
      "\"%s\" %lu/%lu %p/%i\n",
      dw->word, dw->start, dw->end,
      dw->def, dw->def_size );

   return 0;
}

void dict_dump_list( lst_List list )
{
   lst_iterate( list, dict_dump_datum );
}

int dict_destroy_datum( const void *datum )
{
   dictWord *dw = (dictWord *)datum;
		 
   if (dw->word) xfree( (char *)dw->word );
   dw->word     = NULL;
   dw->start    = 0;
   dw->end      = 0;
   dw->database = NULL;
   xfree( dw );
   return 0;
}

void dict_destroy_list( lst_List list )
{
   lst_iterate( list, dict_destroy_datum );
   lst_destroy( list );
}

static int dict_search_exact( lst_List l,
			      const char *word,
			      dictDatabase *database,
			      dictIndex *dbindex)
{
   const char *pt   = NULL;
   int        count = 0;
   dictWord   *datum;
   const char *previous = NULL;

   pt   = dict_index_search( word, dbindex );

   while (pt && pt < dbindex->end) {
      if (!compare( word, dbindex, pt, dbindex->end )) {
	 if (!previous || altcompare(previous, pt, dbindex->end)) {
	    ++count;
	    if (l){
	       datum = dict_word_create( previous = pt, database, dbindex );
	       lst_append( l, datum );
	    }
	 }
      } else break;
      FIND_NEXT( pt, dbindex->end );
   }

   return count;
}

static int dict_search_prefix( lst_List l,
			       const char *word,
			       dictDatabase *database,
			       dictIndex *dbindex)
{
   const char *pt   = dict_index_search( word, dbindex );
   int        count = 0;
   dictWord   *datum;
   const char *previous = NULL;

   while (pt && pt < dbindex->end) {
      switch (compare( word, dbindex, pt, dbindex->end )) {
	 case -2:
	    return count;
	 case -1:
	 case 0:
	    if (!previous || altcompare(previous, pt, dbindex->end)) {
	       ++count;
	       datum = dict_word_create( previous = pt, database, dbindex );
	       lst_append( l, datum );
	    }
	    break;
	 case 1:
	    return count;
	 case 2:
	    return count; /* ERROR!!! */
	 default:
	    assert (0);
      }
      FIND_NEXT( pt, dbindex->end );
   }

   return count;
}

static int dict_search_brute( lst_List l,
			      const unsigned char *word,
			      dictDatabase *database,
			      dictIndex *dbindex,
			      int suffix,
			      int patlen )
{
   const unsigned char *start = dbindex->start;
   const unsigned char *end   = dbindex->end;
   const unsigned char *p, *pt;
   int        count = 0;
   dictWord   *datum;
   int        result;
   const char *previous = NULL;

   assert (dbindex);

   p = start;
   while (p < end && !dbindex -> isspacealnum[*p]) ++p;
   for (; p < end; ++p) {
      if (*p == '\t') {
	 while (p < end && *p != '\n') ++p;
	 ++p;
	 while (p < end && !dbindex -> isspacealnum[*p]) ++p;
      }
      if (tolower(*p) == *word) {
	 result = compare( word, dbindex, p, end );
	 if (result == -1 || result == 0) {
	    if (suffix && result) continue;
	    for (pt = p; pt >= start && *pt != '\n'; --pt)
	       if (*pt == '\t') goto continue2;
	    if (!previous || altcompare(previous, pt + 1, end)) {
	       ++count;
	       datum = dict_word_create( previous = pt + 1, database, dbindex );
#if 0
	       fprintf( stderr, "Adding %d %s\n",
			compare( word, dbindex, p, end ),
			datum->word);
#endif
	       lst_append( l, datum );
	    }
	    FIND_NEXT(p,end);
	    --p;
	 }
      }
 continue2:
      ;
   }
   
   return count;
}

/* dict_search_bmh implements a version of the Boyer-Moore-Horspool text
   searching algorithm, as described in G. H. Gonnet and R. Baeza-Yates,
   HANDBOOK OF ALGORITHMS AND DATA STRUCTURES: IN PASCAL AND C (2nd ed).
   Addison-Wesley Publishing Co., 1991.  Pages 258-9. */

static int dict_search_bmh( lst_List l,
			    const unsigned char *word,
			    dictDatabase *database,
			    dictIndex *dbindex,
			    int suffix )
{
   const unsigned char *start = dbindex->start;
   const unsigned char *end   = dbindex->end;
   int        patlen = strlen( word );
   int        skip[UCHAR_MAX + 1];
   int        i;
   int        j;
   const unsigned char *p, *pt;
   int        count = 0;
   const unsigned char *f = NULL; /* Boolean flag, but has to be a pointer */
   dictWord   *datum;
   const unsigned char *wpt;
   const unsigned char *previous = NULL;

   if (patlen < BMH_THRESHOLD)
      return dict_search_brute( l, word, database, dbindex, suffix, patlen );

   for (i = 0; i <= UCHAR_MAX; i++) {
      if (dbindex -> isspacealnum[i])
	 skip[i] = patlen;
      else
	 skip[i] = 1;
   }
   for (i = 0; i < patlen-1; i++) skip[(unsigned char)word[i]] = patlen-i-1;

   for (p = start+patlen-1; p < end; f ? (f=NULL) : (p += skip[tolower(*p)])) {
      while (*p == '\t') {
	 FIND_NEXT(p,end);
	 p += patlen-1;
	 if (p > end) return count;
      }
      ++_dict_comparisons;		/* counter for profiling */
      
				/* FIXME.  Optimize this inner loop. */
      for (j = patlen - 1, pt = p, wpt = word + patlen - 1; j >= 0; j--) {
	 if (pt < start) break;
 	 while (pt >= start && !dbindex -> isspacealnum[*pt]) {
	    if (*pt == '\n' || *pt == '\t') goto continue2;
	    --pt;
	 }
	 if (tolower(*pt--) != *wpt--) break;
      }
      
      if (j == -1) {
	 if (suffix && p[1] != '\t') continue;
	 for (; pt > start && *pt != '\n'; --pt)
	    if (*pt == '\t') goto continue2;
	 if (pt > start) ++pt;
	 if (!previous || altcompare(previous, pt, dbindex->end)) {
	    ++count;
	    datum = dict_word_create( previous = pt, database, dbindex );
#if 0
	    fprintf( stderr, "Adding %d %s, word = %s\n",
		     compare( word, dbindex, p, dbindex->end ),
		     datum->word,
		     word );
#endif
	    if (l)
	       lst_append( l, datum );
	 }
	 FIND_NEXT(p,end);
	 f = p += patlen-1;	/* Set boolean flag to non-NULL value */
	 if (p > end) return count;
      }
continue2:
      ;
   }

   return count;
}

static int dict_search_substring( lst_List l,
				  const char *word,
				  dictDatabase *database,
				  dictIndex *dbindex)
{
   return dict_search_bmh( l, word, database, dbindex, 0 );
}

static int dict_search_suffix( lst_List l,
			       const char *word,
			       dictDatabase *database,
			       dictIndex *dbindex)
{
   return dict_search_bmh( l, word, database, dbindex, 1 );
}

static int dict_search_regexpr( lst_List l,
				const char *word,
				dictDatabase *database,
				dictIndex *dbindex,
				int type )
{
   const char    *start = dbindex->start;
   const char    *end   = dbindex->end;
   const char    *p, *pt;
   int           count = 0;
   dictWord      *datum;
   regex_t       re;
   char          erbuf[100];
   int           err;
   regmatch_t    subs[1];
   unsigned char first;
   const char    *previous = NULL;

#if OPTSTART
   if (*word == '^' && dbindex -> isspacealnum [(unsigned char) word[1]]) {
      first = word[1];
      end   = dbindex->optStart[i2c(c2i(first)+1)];
      start = dbindex->optStart[first];
      if (end < start) end = dbindex->end;
   }
#endif

   if ((err = regcomp(&re, word, REG_ICASE|REG_NOSUB|type))) {
      regerror(err, &re, erbuf, sizeof(erbuf));
      log_info( "regcomp(%s): %s\n", word, erbuf );
      return 0;
   }

   pt = start;
   while (pt && pt < end) {
      for (p = pt; *p != '\t' && p < end; ++p);
      subs[0].rm_so = 0;
      subs[0].rm_eo = p - pt;
      ++_dict_comparisons;
      if (!regexec(&re, pt, 1, subs, REG_STARTEND)) {
	 if (!previous || altcompare(previous, pt, end)) {
	    ++count;
	    datum = dict_word_create( previous = pt, database, dbindex );
#if 0
	    fprintf( stderr, "Adding %d %s\n",
		     compare( word, dbindex, pt, end ),
		     datum->word);
#endif
	    lst_append( l, datum );
	 }
      }
      pt = p + 1;
      FIND_NEXT( pt, end );
   }

   regfree(&re);
   
   return count;
}

static int dict_search_re( lst_List l,
			   const char *word,
			   dictDatabase *database,
			   dictIndex    *dbindex)
{
   return dict_search_regexpr( l, word, database, dbindex, REG_EXTENDED );
}

static int dict_search_regexp( lst_List l,
			       const char *word,
			       dictDatabase *database,
			       dictIndex    *dbindex)
{
   return dict_search_regexpr( l, word, database, dbindex, REG_BASIC );
}

static int dict_search_soundex( lst_List l,
				const char *word,
				dictDatabase *database,
				dictIndex    *dbindex)
{
   const char *pt;
   const char *end;
   int        count = 0;
   dictWord   *datum;
   char       soundex[10];
   char       buffer[MAXWORDLEN];
   char       *d;
   const unsigned char *s;
   int        i;
   int        c = (unsigned char)*word;
   const char *previous = NULL;

#if OPTSTART
   pt  = dbindex->optStart[ c ];
   end = dbindex->optStart[ i2c(c2i(c)+1) ];
   if (end < pt) end = dbindex->end;
#else
   pt  = dbindex->start;
   end = dbindex->end;
#endif

   strcpy( soundex, txt_soundex( word ) );
   
   while (pt && pt < end) {
      for (i = 0, s = pt, d = buffer; i < MAXWORDLEN - 1; i++, ++s) {
	 if (*s == '\t') break;
	 if (!dbindex -> isspacealnum [*s]) continue;
	 *d++ = *s;
      }
      *d = '\0';
      if (!strcmp(soundex, txt_soundex(buffer))) {
	 if (!previous || altcompare(previous, pt, end)) {
	    datum = dict_word_create( previous = pt, database, dbindex );
	    lst_append( l, datum );
	    ++count;
	 }
      }
      FIND_NEXT(pt,end);
   }

   return count;
}

/* I was unable to locate V. I. Levenshtein, "Binary codes capable of
   correcting deletions, insertions, and reversals,"
   Sov. Phys.-Dokt. 10(8): Feb. 1966, 707-710.

   So, I used Joseph J. Pollock and Antonio Zamora, "Automatic spelling
   correction in scientific and scholarly text," CACM, 27(4): Apr. 1985,
   358-368.  They point out (p. 363) that the precedence of these tests,
   when used for spelling correction, is OMISSION = TRANSPOSITION >
   INSERTION > SUBSTITUTION.  If list is not sorted, then this order should
   be used.

   In this routine, we only consider corrections with a Levenshtein
   distance of 1.

*/

static int dict_search_levenshtein( lst_List l,
				    const char *word,
				    dictDatabase *database,
				    dictIndex *dbindex)
{
   int        len   = strlen(word);
   char       *buf  = alloca(len+2);
   char       *p    = buf;
   int        count = 0;
   set_Set    s     = set_create(NULL,NULL);
   int        i, j, k;
   const char *pt;
   char       tmp;
   dictWord   *datum;

#define CHECK                                         \
   if ((pt = dict_index_search(buf, dbindex))           \
       && !compare(buf, dbindex, pt, dbindex->end)) {            \
      if (!set_member(s,buf)) {                       \
	 ++count;                                     \
	 set_insert(s,str_find(buf));                 \
	 datum = dict_word_create(pt, database, dbindex);\
	 lst_append(l, datum);                        \
         PRINTF(DBG_LEV,("  %s added\n",buf));        \
      }                                               \
   }

				/* Deletions */
   for (i = 0; i < len; i++) {
      p = buf;
      for (j = 0; j < len; j++)
	 if (i != j) *p++ = word[j];
      *p = '\0';
      CHECK;
   }
                                /* Transpositions */
   for (i = 1; i < len; i++) {
      strcpy( buf, word );
      tmp = buf[i-1];
      buf[i-1] = buf[i];
      buf[i] = tmp;
      CHECK;
   }

				/* Insertions */
   for (i = 0; i < len; i++) {
      for (k = 0; k < charcount; k++) {
	 p = buf;
         for (j = 0; j < len; j++) {
            *p++ = word[j];
            if (i == j) *p++ = c(k);
         }
         *p = '\0';
	 CHECK;
      }
   }
                                /* Insertions at the end */
   strcpy( buf, word );
   buf[ len + 1 ] = '\0';
   for (k = 0; k < charcount; k++) {
      buf[ len ] = c(k);
      CHECK;
   }

   
                                  /* Substitutions */
   for (i = 0; i < len; i++) {
      strcpy( buf, word );
      for (j = 0; j < charcount; j++) {
         buf[i] = c(j);
	 CHECK;
      }
   }

   PRINTF(DBG_LEV,("  Got %d matches\n",count));
   set_destroy(s);
   
   return count;
}

/*
  makes anagram of the 8-bit string 'str'
  if length == -1 then str is 0-terminated string
*/
static void stranagram_8bit (char *str, int length)
{
   char* i = str;
   char* j;
   char v;

   if (length == -1)
       length = strlen (str);

   j = str + length - 1;

   while (i < j){
       v = *i;
       *i = *j;
       *j = v;

       ++i;
       --j;
   }
}

/*
  makes anagram of the utf-8 string 'str'
  Returns non-zero if success, 0 otherwise
*/
static int stranagram_utf8 (char *str)
{
   size_t len;
   char *p;

   for (p = str; *p; ){
      len = charlen_utf8 (p);
      if (len == (size_t) -1)
	 return 0; /* not a UTF-8 string */

      if (len > 1)
	  stranagram_8bit (p, len);

      p += len;
   }

   stranagram_8bit (str, -1);
   return 1;
}

/* makes anagram of utf-8 string 'str' */
static int stranagram (char *str, int utf8_string)
{
   if (utf8_string){
      return stranagram_utf8 (str);
   }else{
      stranagram_8bit (str, -1);
      return 1;
   }
}

static int dict_search_plugin (
   lst_List l,
   const char *const word,
   dictDatabase *database,
   int strategy )
{
   int                  ret = 0;
   int                  failed = 0;
   const char * const * defs;
   const int          * defs_sizes;
   int                  defs_count;
   const char         * err_msg;
   int                  i;
   dictWord           * def;
   int                  len;

   PRINTF (DBG_SEARCH, (":S:     searching\n"));
   sigaction (SIGCHLD, NULL, NULL);
   failed = database -> index -> plugin -> dictdb_search (
      database -> index -> plugin -> data,
      word, -1,
      strategy,
      &ret,
      NULL, NULL,
      &defs, &defs_sizes, &defs_count);

   if (failed){
      err_msg = database -> index -> plugin -> dictdb_error (
	 database -> index -> plugin -> data);

      PRINTF (DBG_SEARCH, (":E: Plugin failed: %s\n", err_msg ? err_msg : ""));
   }else{
      switch (ret){
      case PLUGIN_RESULT_FOUND:
	 PRINTF (DBG_SEARCH, (":S:     found %i definitions\n", defs_count));
	 break;
      case PLUGIN_RESULT_NOTFOUND:
	 PRINTF (DBG_SEARCH, (":S:     not found\n"));
	 return 0;
      default:
	 err_fatal (__FUNCTION__, "invalid pligin's exit status\n");
      }

      for (i = 0; i < defs_count; ++i){
	 def = xmalloc (sizeof (dictWord));

	 def -> database = database;
	 def -> start    = def -> end = 0;

	 len = defs_sizes [i];
	 if (-1 == len)
	    len = strlen (defs [i]);

	 if (strategy & DICT_MATCH_MASK){
	    def -> word     = xstrdup (defs [i]);
	    def -> def      = def -> word;
	    def -> def_size = len;
	 }else{
	    def -> word     = xstrdup (word);
	    def -> def      = defs [i];
	    def -> def_size = len;
	 }

	 lst_append (l, def);
      }

      return defs_count;
   }

   return 0;
}

static int dict_search_database_ (
   lst_List l,
   const char *const word,
   dictDatabase *database,
   int strategy )
{
   char       *buf = NULL;

   buf = alloca( strlen( word ) + 1 );

   if (utf8_mode){
      if (!tolower_alnumspace_utf8 (
	  word, buf, database -> index -> flag_allchars))
      {
	 PRINTF(DBG_SEARCH, ("tolower_... ERROR!!!\n"));
	 return 0;
      }
   }else{
      tolower_alnumspace_8bit (
	  word, buf, database -> index -> flag_allchars);
   }

   if (!buf [0] && word [0]){
      /*
        This may happen because of invalid --locale specified.
	Without following line entire dictionary will be returned
          for non-ASCII words.
      */
      return 0;
   }

/*
   if (!database->index)
      database->index =
	  dict_index_open( database->indexFilename, 1, 0, 0 );
   if (!database->index_suffix && database->indexsuffixFilename)
      database->index_suffix =
	  dict_index_open(
	      database->indexsuffixFilename,
	      0, database->index->flag_utf8, database->index->flag_allchars );
*/

   switch (strategy) {
   case DICT_EXACT:
      return dict_search_exact( l, buf, database, database->index );

   case DICT_PREFIX:
      return dict_search_prefix( l, buf, database, database->index );

   case DICT_SUBSTRING:
      return dict_search_substring( l, buf, database, database->index );

   case DICT_SUFFIX:
      if (database->index_suffix){
	 PRINTF(DBG_SEARCH, ("anagram: '%s' ==> ", buf));
	 if (!stranagram (buf, utf8_mode)){
	    PRINTF(DBG_SEARCH, ("failed building anagram\n"));
	    return 0; /* invalid utf8 string */
	 }

	 PRINTF(DBG_SEARCH, ("'%s'\n", buf));
	 return dict_search_prefix ( l, buf, database, database->index_suffix);
      }else{
	 return dict_search_suffix( l, buf, database, database->index );
      }
   case DICT_RE:
      return dict_search_re( l, word, database, database->index );

   case DICT_REGEXP:
      return dict_search_regexp( l, word, database, database->index );

   case DICT_SOUNDEX:
      return dict_search_soundex( l, buf, database, database->index );

   case DICT_LEVENSHTEIN:
      return dict_search_levenshtein( l, buf, database, database->index);

   default:
      err_internal( __FUNCTION__, "Search strategy %d unknown\n", strategy );
   }
}

int dict_search (
   lst_List l,
   const char *const word,
   dictDatabase *database,
   int strategy )
{
   int ret;

   PRINTF (DBG_SEARCH, (":S: Searching in '%s'\n", database -> databaseName));

#if 0
   fprintf (stderr, "STRATEGY: %x\n", strategy);
#endif

   if (database -> index -> plugin){
      PRINTF (DBG_SEARCH, (":S:   plugin search\n"));
      ret = dict_search_plugin (l, word, database, strategy);

      if (ret)
	 return ret;
   }

   strategy &= ~DICT_MATCH_MASK;

   PRINTF (DBG_SEARCH, (":S:   database search\n"));
   return dict_search_database_ (l, word, database, strategy);
}

/* Reads plugin's file name from .dict file */
/* do not free() returned value*/
static char *dict_plugin_filename (dictDatabase *db, dictWord *dw)
{
   static char filename [FILENAME_MAX];

   char *buf = dict_data_obtain (db, dw);
   char *p = buf;
   int len;

   if (!strncmp (p, DICT_ENTRY_PLUGIN, strlen (DICT_ENTRY_PLUGIN))){
      while (*p != '\n')
	 ++p;
   }

   while (*p == '\n' || isspace (*p))
      ++p;

   len = strlen (p);

   while (len > 0 && (p [len - 1] == '\n' || isspace (p [len - 1])))
      --len;

   p [len] = 0;

   strncpy (filename, p, sizeof (filename) - 1);
   filename [sizeof (filename) - 1] = 0;

   xfree (buf);

   return filename;
}

/* reads data without headword 00-... */
static char *dict_plugin_data (dictDatabase *db, dictWord *dw)
{
   char *buf = dict_data_obtain (db, dw);
   char *p = buf;
   int len;

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

dictIndex *dict_index_open(
   const char *filename,
   int init_flags, int flag_utf8, int flag_allchars)
{
   struct stat sb;
   static int  tabInit = 0;
   dictIndex   *i;
   dictDatabase db;
#if OPTSTART
   int         j;
   char        buf[2];
#endif

   if (!filename)
      return NULL;

   i = xmalloc( sizeof( struct dictIndex ) );

   if (!tabInit) dict_table_init();
   tabInit = 1;

   memset( i, 0, sizeof( struct dictIndex ) );

   if ((i->fd = open( filename, O_RDONLY )) < 0)
      err_fatal_errno( __FUNCTION__,
		       "Cannot open index file \"%s\"\n", filename );
   if (fstat( i->fd, &sb ))
      err_fatal_errno( __FUNCTION__,
		       "Cannot stat index file \"%s\"\n", filename );
   i->size = sb.st_size;

   if (mmap_mode){
      i->start = mmap( NULL, i->size, PROT_READ, MAP_SHARED, i->fd, 0 );
      if ((void *)i->start == (void *)(-1))
	 err_fatal_errno (
	    __FUNCTION__,
	    "Cannot mmap index file \"%s\"\b", filename );
   }else{
      i->start = xmalloc (i->size);
      if (-1 == read (i->fd, (char *) i->start, i->size))
	 err_fatal_errno (
	    __FUNCTION__,
	    "Cannot read index file \"%s\"\b", filename );

      close (i -> fd);
      i -> fd = 0;
   }

   i->end = i->start + i->size;

   i->flag_utf8     = flag_utf8;
   i->flag_allchars = flag_allchars;
   i->isspacealnum  = isspacealnumtab;

   i->plugin        = NULL;

#if OPTSTART
   for (j = 0; j <= UCHAR_MAX; j++)
      i->optStart[j] = i->start;
#endif

   if (init_flags){
      memset (&db, 0, sizeof (db));
      db.index = i;

      i->flag_allchars = 1;
      i->isspacealnum = isspacealnumtab_allchars;

      i->flag_allchars =
	 0 != dict_search_database_ (NULL, DICT_FLAG_ALLCHARS, &db, DICT_EXACT);
      PRINTF(DBG_INIT, (":I:     \"%s\": flag_allchars=%i\n", filename, i->flag_allchars));

      if (!i -> flag_allchars)
	 i -> isspacealnum = isspacealnumtab;

      i->flag_utf8 =
	 0 != dict_search_database_ (NULL, DICT_FLAG_UTF8, &db, DICT_EXACT);
      PRINTF(DBG_INIT, (":I:     \"%s\": flag_utf8=%i\n", filename, i->flag_utf8));
   }

#if OPTSTART
   buf[0] = ' ';
   buf[1] = '\0';
   i->optStart[ ' ' ] = binary_search_8bit( buf, i, i->start, i->end );

   for (j = 0; j < charcount; j++) {
      buf[0] = c(j);
      buf[1] = '\0';
      i->optStart[toupper(c(j))]
	 = i->optStart[c(j)]
	 = binary_search_8bit( buf, i, i->start, i->end );
      if (dbg_test (DBG_SEARCH)){
	 if (!utf8_mode || c(j) <= CHAR_MAX)
	    printf ("optStart [%c] = %p\n", c(j), i->optStart[c(j)]);
	 else
	    printf ("optStart [%i] = %p\n", c(j), i->optStart[c(j)]);
      }
   }

   for (j = '0'; j <= '9'; j++) {
      buf[0] = j;
      buf[1] = '\0';
      i->optStart[j] = binary_search_8bit( buf, i, i->start, i->end );
   }

   i->optStart[UCHAR_MAX]   = i->end;
   i->optStart[UCHAR_MAX+1] = i->end;
#endif

   return i;
}

void dict_index_close( dictIndex *i )
{
   if (!i)
      return;

   if (mmap_mode){
      if (i->fd >= 0) {
	 munmap( (void *)i->start, i->size );
	 close( i->fd );
	 i->fd = 0;
      }
   }else{
      if (i -> start)
	 xfree ((char *) i -> start);
   }

   i->start = i->end = NULL;
   i->flag_utf8      = 0;
   i->flag_allchars  = 0;
   i->isspacealnum   = NULL;

   xfree (i);
}

int dict_plugin_open (dictIndex *i, dictDatabase *db)
{
   int ret = 0;
   lst_List list;
   char *plugin_filename;
   char *plugin_data;
   dictWord *dw;
   dictPluginInitData init_data;
   int version;

   list = lst_create();

   ret = dict_search_database_ (list, DICT_ENTRY_PLUGIN, db, DICT_EXACT);
   switch (ret){
   case 1: case 2:
      dw = (dictWord *) lst_pop (list);

      plugin_filename = dict_plugin_filename (db, dw);
      PRINTF(DBG_INIT, (":I:   Initinalizing plugin '%s'\n", plugin_filename));

      dict_destroy_datum (dw);
      if (2 == ret)
	 dict_destroy_datum (lst_pop (list));

      i -> plugin = xmalloc (sizeof (dictPlugin));
      memset (i -> plugin, 0, sizeof (dictPlugin));

      PRINTF(DBG_INIT, (":I:     opening plugin\n"));
      i -> plugin -> handle = lt_dlopen (plugin_filename);
      if (!i -> plugin -> handle){
	 PRINTF(DBG_INIT, (":I:     faild\n"));
	 exit (1);
      }

      PRINTF(DBG_INIT, (":I:     getting functions addresses\n"));
      i -> plugin -> dictdb_open   =
	 lt_dlsym (i -> plugin -> handle, DICT_PLUGINFUN_OPEN);
      i -> plugin -> dictdb_free   =
	 lt_dlsym (i -> plugin -> handle, DICT_PLUGINFUN_FREE);
      i -> plugin -> dictdb_search =
	 lt_dlsym (i -> plugin -> handle, DICT_PLUGINFUN_SEARCH);
      i -> plugin -> dictdb_close  =
	 lt_dlsym (i -> plugin -> handle, DICT_PLUGINFUN_CLOSE);
      i -> plugin -> dictdb_error  =
	 lt_dlsym (i -> plugin -> handle, DICT_PLUGINFUN_ERROR);

      if (!i -> plugin -> dictdb_open || !i -> plugin -> dictdb_search ||
	  !i -> plugin -> dictdb_free ||
	  !i -> plugin -> dictdb_error || !i -> plugin -> dictdb_close)
      {
	 PRINTF(DBG_INIT, (":I:     faild\n"));
	 exit (1);
      }

      ret = dict_search_database_ (list, DICT_ENTRY_PLUGIN_DATA, db, DICT_EXACT);

      switch (ret){
      case 0:
	 PRINTF(DBG_INIT, (":I:     initializing plugin\n"));
	 ret = i -> plugin -> dictdb_open (NULL, 0, &version, &i -> plugin -> data);
	 if (ret){
	    err_fatal (__FUNCTION__, "Cannot initialize plugin '%s'\n", plugin_filename);
	 }

	 switch (version){
	 case 0:
	    break;
	 default:
	    err_fatal (__FUNCTION__, "Invalid version returned\n");
	 }

	 break;

      case 1: case 2:
	 dw = (dictWord *) lst_pop (list);
	 plugin_data = dict_plugin_data (db, dw);

	 dict_destroy_datum (dw);
	 if (2 == ret)
	    dict_destroy_datum (lst_pop (list));

	 PRINTF(DBG_INIT, (":I:     initializing plugin\n"));

	 init_data.id   = PLUGIN_INIT_DATA_DICT;
	 init_data.data = plugin_data;
	 init_data.size = -1;

	 ret = i -> plugin -> dictdb_open (
	    &init_data, 1,
	    NULL, &i -> plugin -> data);

	 if (ret){
	    PRINTF(DBG_INIT, (":I:     faild\n"));
	    exit (1);
	 }

	 xfree (plugin_data);

	 break;

      default:
	 err_internal(
	    __FUNCTION__,
	    "more than one %s entry\n",
	    DICT_ENTRY_PLUGIN_DATA );

	 break;
      }

      break;

   case 0:
      break;

   default:
      err_internal( __FUNCTION__, "Corrupted .index file'\n" );
   }

   lst_destroy (list);
   return 0;
}

void dict_plugin_close ( dictIndex *i )
{
   int ret;

   if (!i -> plugin)
      return;

   if (i -> plugin -> dictdb_close){
      ret = i -> plugin -> dictdb_close (i -> plugin -> data);
      if (ret){
	 PRINTF(DBG_INIT, ("exiting plugin failed"));
	 exit (1);
      }
   }

   ret = lt_dlclose (i -> plugin -> handle);
   if (ret)
      PRINTF(DBG_INIT, ("%s", lt_dlerror ()));

   xfree (i -> plugin);
}
