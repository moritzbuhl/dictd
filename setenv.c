#include "dictP.h"

int setenv(const char *name, const char *value, int overwrite)
{
#if HAVE_PUTENV

   if (!overwrite && getenv (name)){
      return 0;
   }else{
      char *p = (char *) malloc (strlen (name) + strlen (value) + 2);
      strcpy (p, name);
      strcat (p, "=");
      strcat (p, value);
      fprintf (stderr, "%s\n", p);
      return putenv (p);
   }
#else
   abort();
#endif
}
