/* 
 * $Id$
 *
 * (c) 2004 by Kalle Wallin (kaw@linux.se)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "config.h"
#include "ncmpc.h"
#include "support.h"

#define BUFSIZE 1024

extern void screen_status_printf(char *format, ...);

static gboolean noconvert = TRUE;

char *
trim(char *str)
{
  char *end;

  if( str==NULL )
    return NULL;

  while( IS_WHITESPACE(*str) )
    str++;

  end=str+strlen(str)-1;
  while( end>str && IS_WHITESPACE(*end) )
    {
      *end = '\0';
      end--;
    }
  return str;
}

char *
remove_trailing_slash(char *path)
{
  int len;

  if( path==NULL )
    return NULL;

  len=strlen(path);
  if( len>1 && path[len-1] == '/' )
    path[len-1] = '\0';

  return path;
}

char *
lowerstr(char *str)
{
  size_t i;
  size_t len = strlen(str);

  if( str==NULL )
    return NULL;

  i=0;
  while(i<len && str[i])
    {
      str[i] = tolower(str[i]);
      i++;
    }
  return str;
}


#ifndef HAVE_BASENAME
char *
basename(char *path)
{
  char *end;

  path = remove_trailing_slash(path);
  end = path + strlen(path);

  while( end>path && *end!='/' )
    end--;

  if( *end=='/' && end!=path )
    return end+1;

  return path;
}
#endif /* HAVE_BASENAME */


#ifndef HAVE_STRCASESTR
char *
strcasestr(const char *haystack, const char *needle)
{
  return strstr(lowerstr(haystack), lowerstr(needle));
}
#endif /* HAVE_STRCASESTR */

void
charset_init(gboolean disable)
{
  noconvert = disable;
}

char *
utf8_to_locale(char *utf8str)
{
  gchar *str;
  gsize rb, wb;
  GError *error;

  if( noconvert )
    return g_strdup(utf8str);

  rb = 0; /* bytes read */
  wb = 0; /* bytes written */
  error = NULL;
  str=g_locale_from_utf8(utf8str, 
			 strlen(utf8str),
			 &wb, &rb,
			 &error);
  if( error )
    {
      const char *charset;

      g_get_charset(&charset);
      screen_status_printf(_("Error: Unable to convert characters to %s"),
			   charset);
      D(g_printerr("utf8_to_locale(): %s\n", error->message));
      g_error_free(error);
      return g_strdup(utf8str);
    }
  
  return str;
}

char *
locale_to_utf8(char *localestr)
{
  gchar *str;
  gsize rb, wb;
  GError *error;

  if( noconvert )
    return g_strdup(localestr);

  rb = 0; /* bytes read */
  wb = 0; /* bytes written */
  error = NULL;
  str=g_locale_to_utf8(localestr, 
		       strlen(localestr), 
		       &wb, &rb,
		       &error);
  if( error )
    {
      screen_status_printf(_("Error: Unable to convert characters to UTF-8"));
      D(g_printerr("locale_to_utf8: %s\n", error->message));
      g_error_free(error);
      return g_strdup(localestr);
    }

  return str;
}


