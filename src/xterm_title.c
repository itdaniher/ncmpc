/* ncmpc (Ncurses MPD Client)
 * (c) 2004-2017 The Music Player Daemon Project
 * Project homepage: http://musicpd.org
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
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "xterm_title.h"
#include "options.h"

#include <stdio.h>
#include <stdlib.h>

void
set_xterm_title(const char *title)
{
	/* the current xterm title exists under the WM_NAME property */
	/* and can be retrieved with xprop -id $WINDOWID */

	if (!options.enable_xterm_title)
		return;

	if (getenv("WINDOWID") == NULL) {
		options.enable_xterm_title = FALSE;
		return;
	}

	printf("\033]0;%s\033\\", title);
	fflush(stdout);
}
