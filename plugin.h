#ifndef _PLUGIN_H_
#define _PLUGIN_H_

#define DICT_ENTRY_PLUGIN         "00-database-plugin"
#define DICT_ENTRY_PLUGIN_DATA    "00-database-plugin-data"

#define DICT_PLUGINFUN_OPEN      "dictdb_open"
#define DICT_PLUGINFUN_ERROR     "dictdb_error"
#define DICT_PLUGINFUN_FREE      "dictdb_free"
#define DICT_PLUGINFUN_SEARCH    "dictdb_search"
#define DICT_PLUGINFUN_CLOSE     "dictdb_close"

#define DICT_EXACT        1	/* Exact */
#define DICT_PREFIX       2	/* Prefix */
#define DICT_SUBSTRING    3	/* Substring */
#define DICT_SUFFIX       4	/* Suffix */
#define DICT_RE           5	/* POSIX 1003.2 (modern) regular expressions */
#define DICT_REGEXP       6	/* old (basic) regular expresions */
#define DICT_SOUNDEX      7	/* Soundex */
#define DICT_LEVENSHTEIN  8	/* Levenshtein */

/*
  The following mask is added to the search strategy
  and passed to plugin
  if search type is 'match'
 */
#define DICT_MATCH_MASK 0x8000

/*
  EACH FUNCTION RETURNS ZERO IF SUCCESS
*/

typedef struct dictPluginInitData {
   int id;           /* PLUGIN_DATA_XXX constant */
   int size;
   const void *data;
} dictPluginInitData;

/*
  Initializes dictionary and returns zero if success.
 */
typedef int (*dictdb_open_type)
   (
      const dictPluginInitData *init_data, /* in: data for initializing dictionary */
      int init_data_size, /* in: a number of dictPluginInitData strctures */
      int *version,       /* out: version of plugin's interface*/
      void ** dict_data   /* out: plugin's global data */
      );

enum {
   PLUGIN_INIT_DATA_DICT, /* data obtained from .dict file */
   PLUGIN_INIT_DATA_AUTH

   /* this list can be enlarged*/
};

/*
 * Returns error message or NULL if last operation returned success.
 */
typedef const char *(*dictdb_error_type) (
   void * dict_data       /* in: plugin's global data */
   );

/*
 * Deinitializes dictionary and returns zero if success.
 * This function MUST ALWAYS BE called after dictdb_init.
 */
typedef int (*dictdb_close_type) (
   void * dict_data       /* in: plugin's global data */
   );

enum {
   PLUGIN_RESULT_NOTFOUND,
   PLUGIN_RESULT_FOUND,         /* definitions/matches have been found */

   /* this list can be enlarged */
};

/*
 * Frees data allocated by dictdb_search
 */
typedef int (*dictdb_free_type) (
   void * dict_data       /* in: plugin's global data */
   );

/*
 *
 *
 */
typedef int (*dictdb_search_type) (
   void *dict_data,         /* in: plugin's global data */
   const char *word,        /* in: word */
   int word_size,           /* in: wordsize or -1 for 0-terminated string */
   int search_strategy,     /* in: search strategy */
   int *ret,                /* out: result type - plt_XXX */
   const char **result_extra,       /* out: extra information */
   int *result_extra_size,          /* out: extra information size */
   const char * const* *definitions,/* out: definitions */
   const int * *definitions_sizes,  /* out: sizes of definitions */
   int *definitions_count);         /* out: a number of found definitions */

#endif // _PLUGIN_H_
