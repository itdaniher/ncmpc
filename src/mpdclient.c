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

#include "mpdclient.h"
#include "callbacks.h"
#include "filelist.h"
#include "config.h"
#include "gidle.h"
#include "charset.h"

#ifdef ENABLE_ASYNC_CONNECT
#include "aconnect.h"
#endif

#include <mpd/client.h>

#include <assert.h>

static gboolean
mpdclient_enter_idle_callback(gpointer user_data)
{
	struct mpdclient *c = user_data;
	assert(c->enter_idle_source_id != 0);
	assert(c->source != NULL);
	assert(!c->idle);

	c->enter_idle_source_id = 0;
	c->idle = mpd_glib_enter(c->source);
	return false;
}

static void
mpdclient_schedule_enter_idle(struct mpdclient *c)
{
	assert(c != NULL);
	assert(c->source != NULL);

	if (c->enter_idle_source_id == 0)
		/* automatically re-enter MPD "idle" mode */
		c->enter_idle_source_id =
			g_idle_add(mpdclient_enter_idle_callback, c);
}

static void
mpdclient_cancel_enter_idle(struct mpdclient *c)
{
	if (c->enter_idle_source_id != 0) {
		g_source_remove(c->enter_idle_source_id);
		c->enter_idle_source_id = 0;
	}
}

static void
mpdclient_invoke_error_callback(enum mpd_error error,
				const char *message)
{
	char *allocated;
	if (error == MPD_ERROR_SERVER)
		/* server errors are UTF-8, the others are locale */
		message = allocated = utf8_to_locale(message);
	else
		allocated = NULL;

	mpdclient_error_callback(message);
	g_free(allocated);
}

static void
mpdclient_invoke_error_callback1(struct mpdclient *c)
{
	assert(c != NULL);
	assert(c->connection != NULL);

	struct mpd_connection *connection = c->connection;

	enum mpd_error error = mpd_connection_get_error(connection);
	assert(error != MPD_ERROR_SUCCESS);

	mpdclient_invoke_error_callback(error,
					mpd_connection_get_error_message(connection));
}

static void
mpdclient_gidle_callback(enum mpd_error error,
			 gcc_unused enum mpd_server_error server_error,
			 const char *message, enum mpd_idle events,
			 void *ctx)
{
	struct mpdclient *c = ctx;

	c->idle = false;

	assert(mpdclient_is_connected(c));

	if (error != MPD_ERROR_SUCCESS) {
		mpdclient_invoke_error_callback(error, message);
		mpdclient_disconnect(c);
		mpdclient_lost_callback();
		return;
	}

	c->events |= events;
	mpdclient_update(c);

	mpdclient_idle_callback(c->events);

	c->events = 0;

	if (c->source != NULL)
		mpdclient_schedule_enter_idle(c);
}

/****************************************************************************/
/*** mpdclient functions ****************************************************/
/****************************************************************************/

bool
mpdclient_handle_error(struct mpdclient *c)
{
	enum mpd_error error = mpd_connection_get_error(c->connection);

	assert(error != MPD_ERROR_SUCCESS);

	if (error == MPD_ERROR_SERVER &&
	    mpd_connection_get_server_error(c->connection) == MPD_SERVER_ERROR_PERMISSION &&
	    mpdclient_auth_callback(c))
		return true;

	mpdclient_invoke_error_callback(error,
					mpd_connection_get_error_message(c->connection));

	if (!mpd_connection_clear_error(c->connection)) {
		mpdclient_disconnect(c);
		mpdclient_lost_callback();
	}

	return false;
}

#ifdef ENABLE_ASYNC_CONNECT
#ifndef WIN32

static bool
is_local_socket(const char *host)
{
	return *host == '/' || *host == '@';
}

static bool
settings_is_local_socket(const struct mpd_settings *settings)
{
	const char *host = mpd_settings_get_host(settings);
	return host != NULL && is_local_socket(host);
}

#endif
#endif

struct mpdclient *
mpdclient_new(const gchar *host, unsigned port,
	      unsigned timeout_ms, const gchar *password)
{
	struct mpdclient *c = g_new0(struct mpdclient, 1);

#ifdef ENABLE_ASYNC_CONNECT
	c->settings = mpd_settings_new(host, port, timeout_ms,
				       NULL, NULL);
	if (c->settings == NULL)
		g_error("Out of memory");

#ifndef WIN32
	c->settings2 = host == NULL && port == 0 &&
		settings_is_local_socket(c->settings)
		? mpd_settings_new(host, 6600, timeout_ms, NULL, NULL)
		: NULL;
#endif

#else
	c->host = host;
	c->port = port;
#endif

	c->timeout_ms = timeout_ms;
	c->password = password;

	playlist_init(&c->playlist);
	c->volume = -1;
	c->events = 0;
	c->playing = false;

	return c;
}

void
mpdclient_free(struct mpdclient *c)
{
	mpdclient_disconnect(c);

	mpdclient_playlist_free(&c->playlist);

#ifdef ENABLE_ASYNC_CONNECT
	mpd_settings_free(c->settings);

#ifndef WIN32
	if (c->settings2 != NULL)
		mpd_settings_free(c->settings2);
#endif
#endif

	g_free(c);
}

static char *
settings_name(const struct mpd_settings *settings)
{
	assert(settings != NULL);

	const char *host = mpd_settings_get_host(settings);
	if (host == NULL)
		host = "unknown";

	if (host[0] == '/')
		return g_strdup(host);

	unsigned port = mpd_settings_get_port(settings);
	if (port == 0 || port == 6600)
		return g_strdup(host);

	return g_strdup_printf("%s:%u", host, port);
}

char *
mpdclient_settings_name(const struct mpdclient *c)
{
	assert(c != NULL);

#ifdef ENABLE_ASYNC_CONNECT
	return settings_name(c->settings);
#else
	struct mpd_settings *settings =
		mpd_settings_new(c->host, c->port, 0, NULL, NULL);
	if (settings == NULL)
		return g_strdup("unknown");

	char *name = settings_name(settings);
	mpd_settings_free(settings);
	return name;
#endif
}

static void
mpdclient_status_free(struct mpdclient *c)
{
	if (c->status == NULL)
		return;

	mpd_status_free(c->status);
	c->status = NULL;

	c->volume = -1;
	c->playing = false;
}

void
mpdclient_disconnect(struct mpdclient *c)
{
#ifdef ENABLE_ASYNC_CONNECT
	if (c->async_connect != NULL) {
		aconnect_cancel(c->async_connect);
		c->async_connect = NULL;
	}
#endif

	mpdclient_cancel_enter_idle(c);

	if (c->source != NULL) {
		mpd_glib_free(c->source);
		c->source = NULL;
		c->idle = false;
	}

	if (c->connection) {
		mpd_connection_free(c->connection);
		++c->connection_id;
	}
	c->connection = NULL;

	mpdclient_status_free(c);

	playlist_clear(&c->playlist);

	if (c->song)
		c->song = NULL;

	/* everything has changed after a disconnect */
	c->events |= MPD_IDLE_ALL;
}

static bool
mpdclient_connected(struct mpdclient *c,
		    struct mpd_connection *connection)
{
	c->connection = connection;

	if (mpd_connection_get_error(connection) != MPD_ERROR_SUCCESS) {
		mpdclient_invoke_error_callback1(c);
		mpdclient_disconnect(c);
		mpdclient_failed_callback();
		return false;
	}

#ifdef ENABLE_ASYNC_CONNECT
	if (c->timeout_ms > 0)
		mpd_connection_set_timeout(connection, c->timeout_ms);
#endif

	/* send password */
	if (c->password != NULL &&
	    !mpd_run_password(connection, c->password)) {
		mpdclient_invoke_error_callback1(c);
		mpdclient_disconnect(c);
		mpdclient_failed_callback();
		return false;
	}

	c->source = mpd_glib_new(connection,
				 mpdclient_gidle_callback, c);
	mpdclient_schedule_enter_idle(c);

	++c->connection_id;

	mpdclient_connected_callback();
	return true;
}

#ifdef ENABLE_ASYNC_CONNECT

static void
mpdclient_aconnect_start(struct mpdclient *c,
			 const struct mpd_settings *settings);

static void
mpdclient_connect_success(struct mpd_connection *connection, void *ctx)
{
	struct mpdclient *c = ctx;
	assert(c->async_connect != NULL);
	c->async_connect = NULL;

	mpdclient_connected(c, connection);
}

static void
mpdclient_connect_error(const char *message, void *ctx)
{
	struct mpdclient *c = ctx;
	assert(c->async_connect != NULL);
	c->async_connect = NULL;

#ifndef WIN32
	if (!c->connecting2 && c->settings2 != NULL) {
		c->connecting2 = true;
		mpdclient_aconnect_start(c, c->settings2);
		return;
	}
#endif

	mpdclient_error_callback(message);
	mpdclient_failed_callback();
}

static const struct aconnect_handler mpdclient_connect_handler = {
	.success = mpdclient_connect_success,
	.error = mpdclient_connect_error,
};

static void
mpdclient_aconnect_start(struct mpdclient *c,
			 const struct mpd_settings *settings)
{
	aconnect_start(&c->async_connect,
		       mpd_settings_get_host(settings),
		       mpd_settings_get_port(settings),
		       &mpdclient_connect_handler, c);
}

#endif

void
mpdclient_connect(struct mpdclient *c)
{
	/* close any open connection */
	mpdclient_disconnect(c);

#ifdef ENABLE_ASYNC_CONNECT
#ifndef WIN32
	c->connecting2 = false;
#endif
	mpdclient_aconnect_start(c, c->settings);
#else
	/* connect to MPD */
	struct mpd_connection *connection =
		mpd_connection_new(c->host, c->port, c->timeout_ms);
	if (connection == NULL)
		g_error("Out of memory");

	mpdclient_connected(c, connection);
#endif
}

bool
mpdclient_update(struct mpdclient *c)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);

	if (connection == NULL)
		return false;

	/* free the old status */
	mpdclient_status_free(c);

	/* retrieve new status */
	c->status = mpd_run_status(connection);
	if (c->status == NULL)
		return mpdclient_handle_error(c);

	c->volume = mpd_status_get_volume(c->status);
	c->playing = mpd_status_get_state(c->status) == MPD_STATE_PLAY;

	/* check if the playlist needs an update */
	if (c->playlist.version != mpd_status_get_queue_version(c->status)) {
		bool retval;

		if (!playlist_is_empty(&c->playlist))
			retval = mpdclient_playlist_update_changes(c);
		else
			retval = mpdclient_playlist_update(c);
		if (!retval)
			return false;
	}

	/* update the current song */
	if (!c->song || mpd_status_get_song_id(c->status) >= 0) {
		c->song = playlist_get_song(&c->playlist,
					    mpd_status_get_song_pos(c->status));
	}

	return true;
}

struct mpd_connection *
mpdclient_get_connection(struct mpdclient *c)
{
	if (c->source != NULL && c->idle) {
		c->idle = false;
		mpd_glib_leave(c->source);

		mpdclient_schedule_enter_idle(c);
	}

	return c->connection;
}

static struct mpd_status *
mpdclient_recv_status(struct mpdclient *c)
{
	assert(c->connection != NULL);

	struct mpd_status *status = mpd_recv_status(c->connection);
	if (status == NULL) {
		mpdclient_handle_error(c);
		return NULL;
	}

	if (c->status != NULL)
		mpd_status_free(c->status);
	return c->status = status;
}

/****************************************************************************/
/*** MPD Commands  **********************************************************/
/****************************************************************************/

bool
mpdclient_cmd_crop(struct mpdclient *c)
{
	if (!mpdclient_is_playing(c))
		return false;

	int length = mpd_status_get_queue_length(c->status);
	int current = mpd_status_get_song_pos(c->status);
	if (current < 0 || mpd_status_get_queue_length(c->status) < 2)
		return true;

	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	mpd_command_list_begin(connection, false);

	if (current < length - 1)
		mpd_send_delete_range(connection, current + 1, length);
	if (current > 0)
		mpd_send_delete_range(connection, 0, current);

	mpd_command_list_end(connection);

	return mpdclient_finish_command(c);
}

bool
mpdclient_cmd_clear(struct mpdclient *c)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	/* send "clear" and "status" */
	if (!mpd_command_list_begin(connection, false) ||
	    !mpd_send_clear(connection) ||
	    !mpd_send_status(connection) ||
	    !mpd_command_list_end(connection))
		return mpdclient_handle_error(c);

	/* receive the new status, store it in the mpdclient struct */

	struct mpd_status *status = mpdclient_recv_status(c);
	if (status == NULL)
		return false;

	if (!mpd_response_finish(connection))
		return mpdclient_handle_error(c);

	/* update mpdclient.playlist */

	if (mpd_status_get_queue_length(status) == 0) {
		/* after the "clear" command, the queue is really
		   empty - this means we can clear it locally,
		   reducing the UI latency */
		playlist_clear(&c->playlist);
		c->playlist.version = mpd_status_get_queue_version(status);
		c->song = NULL;
	}

	c->events |= MPD_IDLE_QUEUE;
	return true;
}

bool
mpdclient_cmd_volume(struct mpdclient *c, gint value)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	mpd_send_set_volume(connection, value);
	return mpdclient_finish_command(c);
}

bool
mpdclient_cmd_volume_up(struct mpdclient *c)
{
	if (c->volume < 0 || c->volume >= 100)
		return true;

	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	return mpdclient_cmd_volume(c, ++c->volume);
}

bool
mpdclient_cmd_volume_down(struct mpdclient *c)
{
	if (c->volume <= 0)
		return true;

	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	return mpdclient_cmd_volume(c, --c->volume);
}

bool
mpdclient_cmd_add_path(struct mpdclient *c, const gchar *path_utf8)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	return mpd_send_add(connection, path_utf8)?
		mpdclient_finish_command(c) : false;
}

bool
mpdclient_cmd_add(struct mpdclient *c, const struct mpd_song *song)
{
	assert(c != NULL);
	assert(song != NULL);

	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL || c->status == NULL)
		return false;

	/* send the add command to mpd; at the same time, get the new
	   status (to verify the new playlist id) and the last song
	   (we hope that's the song we just added) */

	if (!mpd_command_list_begin(connection, true) ||
	    !mpd_send_add(connection, mpd_song_get_uri(song)) ||
	    !mpd_send_status(connection) ||
	    !mpd_send_get_queue_song_pos(connection,
					 playlist_length(&c->playlist)) ||
	    !mpd_command_list_end(connection) ||
	    !mpd_response_next(connection))
		return mpdclient_handle_error(c);

	c->events |= MPD_IDLE_QUEUE;

	struct mpd_status *status = mpdclient_recv_status(c);
	if (status == NULL)
		return false;

	if (!mpd_response_next(connection))
		return mpdclient_handle_error(c);

	struct mpd_song *new_song = mpd_recv_song(connection);
	if (!mpd_response_finish(connection) || new_song == NULL) {
		if (new_song != NULL)
			mpd_song_free(new_song);

		return mpd_connection_clear_error(connection) ||
			mpdclient_handle_error(c);
	}

	if (mpd_status_get_queue_length(status) == playlist_length(&c->playlist) + 1 &&
	    mpd_status_get_queue_version(status) == c->playlist.version + 1) {
		/* the cheap route: match on the new playlist length
		   and its version, we can keep our local playlist
		   copy in sync */
		c->playlist.version = mpd_status_get_queue_version(status);

		/* the song we just received has the correct id;
		   append it to the local playlist */
		playlist_append(&c->playlist, new_song);
	}

	mpd_song_free(new_song);

	return true;
}

bool
mpdclient_cmd_delete(struct mpdclient *c, gint idx)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);

	if (connection == NULL || c->status == NULL)
		return false;

	if (idx < 0 || (guint)idx >= playlist_length(&c->playlist))
		return false;

	const struct mpd_song *song = playlist_get(&c->playlist, idx);

	/* send the delete command to mpd; at the same time, get the
	   new status (to verify the playlist id) */

	if (!mpd_command_list_begin(connection, false) ||
	    !mpd_send_delete_id(connection, mpd_song_get_id(song)) ||
	    !mpd_send_status(connection) ||
	    !mpd_command_list_end(connection))
		return mpdclient_handle_error(c);

	c->events |= MPD_IDLE_QUEUE;

	struct mpd_status *status = mpdclient_recv_status(c);
	if (status == NULL)
		return false;

	if (!mpd_response_finish(connection))
		return mpdclient_handle_error(c);

	if (mpd_status_get_queue_length(status) == playlist_length(&c->playlist) - 1 &&
	    mpd_status_get_queue_version(status) == c->playlist.version + 1) {
		/* the cheap route: match on the new playlist length
		   and its version, we can keep our local playlist
		   copy in sync */
		c->playlist.version = mpd_status_get_queue_version(status);

		/* remove the song from the local playlist */
		playlist_remove(&c->playlist, idx);

		/* remove references to the song */
		if (c->song == song)
			c->song = NULL;
	}

	return true;
}

bool
mpdclient_cmd_delete_range(struct mpdclient *c, unsigned start, unsigned end)
{
	if (end == start + 1)
		/* if that's not really a range, we choose to use the
		   safer "deleteid" version */
		return mpdclient_cmd_delete(c, start);

	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	/* send the delete command to mpd; at the same time, get the
	   new status (to verify the playlist id) */

	if (!mpd_command_list_begin(connection, false) ||
	    !mpd_send_delete_range(connection, start, end) ||
	    !mpd_send_status(connection) ||
	    !mpd_command_list_end(connection))
		return mpdclient_handle_error(c);

	c->events |= MPD_IDLE_QUEUE;

	struct mpd_status *status = mpdclient_recv_status(c);
	if (status == NULL)
		return false;

	if (!mpd_response_finish(connection))
		return mpdclient_handle_error(c);

	if (mpd_status_get_queue_length(status) == playlist_length(&c->playlist) - (end - start) &&
	    mpd_status_get_queue_version(status) == c->playlist.version + 1) {
		/* the cheap route: match on the new playlist length
		   and its version, we can keep our local playlist
		   copy in sync */
		c->playlist.version = mpd_status_get_queue_version(status);

		/* remove the song from the local playlist */
		while (end > start) {
			--end;

			/* remove references to the song */
			if (c->song == playlist_get(&c->playlist, end))
				c->song = NULL;

			playlist_remove(&c->playlist, end);
		}
	}

	return true;
}

bool
mpdclient_cmd_move(struct mpdclient *c, unsigned dest_pos, unsigned src_pos)
{
	if (dest_pos == src_pos)
		return true;

	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	/* send the "move" command to MPD; at the same time, get the
	   new status (to verify the playlist id) */

	if (!mpd_command_list_begin(connection, false) ||
	    !mpd_send_move(connection, src_pos, dest_pos) ||
	    !mpd_send_status(connection) ||
	    !mpd_command_list_end(connection))
		return mpdclient_handle_error(c);

	c->events |= MPD_IDLE_QUEUE;

	struct mpd_status *status = mpdclient_recv_status(c);
	if (status == NULL)
		return false;

	if (!mpd_response_finish(connection))
		return mpdclient_handle_error(c);

	if (mpd_status_get_queue_length(status) == playlist_length(&c->playlist) &&
	    mpd_status_get_queue_version(status) == c->playlist.version + 1) {
		/* the cheap route: match on the new playlist length
		   and its version, we can keep our local playlist
		   copy in sync */
		c->playlist.version = mpd_status_get_queue_version(status);

		/* swap songs in the local playlist */
		playlist_move(&c->playlist, dest_pos, src_pos);
	}

	return true;
}

/* The client-to-client protocol (MPD 0.17.0) */

bool
mpdclient_cmd_subscribe(struct mpdclient *c, const char *channel)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);

	if (connection == NULL)
		return false;

	if (!mpd_send_subscribe(connection, channel))
		return mpdclient_handle_error(c);

	return mpdclient_finish_command(c);
}

bool
mpdclient_cmd_unsubscribe(struct mpdclient *c, const char *channel)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	if (!mpd_send_unsubscribe(connection, channel))
		return mpdclient_handle_error(c);

	return mpdclient_finish_command(c);
}

bool
mpdclient_cmd_send_message(struct mpdclient *c, const char *channel,
			   const char *text)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	if (!mpd_send_send_message(connection, channel, text))
		return mpdclient_handle_error(c);

	return mpdclient_finish_command(c);
}

bool
mpdclient_send_read_messages(struct mpdclient *c)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	return mpd_send_read_messages(connection)?
		true : mpdclient_handle_error(c);
}

struct mpd_message *
mpdclient_recv_message(struct mpdclient *c)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	struct mpd_message *message = mpd_recv_message(connection);
	if (message == NULL &&
	    mpd_connection_get_error(connection) != MPD_ERROR_SUCCESS)
		mpdclient_handle_error(c);

	return message;
}

/****************************************************************************/
/*** Playlist management functions ******************************************/
/****************************************************************************/

/* update playlist */
bool
mpdclient_playlist_update(struct mpdclient *c)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	playlist_clear(&c->playlist);

	mpd_send_list_queue_meta(connection);

	struct mpd_entity *entity;
	while ((entity = mpd_recv_entity(connection))) {
		if (mpd_entity_get_type(entity) == MPD_ENTITY_TYPE_SONG)
			playlist_append(&c->playlist, mpd_entity_get_song(entity));

		mpd_entity_free(entity);
	}

	c->playlist.version = mpd_status_get_queue_version(c->status);
	c->song = NULL;

	return mpdclient_finish_command(c);
}

/* update playlist (plchanges) */
bool
mpdclient_playlist_update_changes(struct mpdclient *c)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);

	if (connection == NULL)
		return false;

	mpd_send_queue_changes_meta(connection, c->playlist.version);

	struct mpd_song *song;
	while ((song = mpd_recv_song(connection)) != NULL) {
		int pos = mpd_song_get_pos(song);

		if (pos >= 0 && (guint)pos < c->playlist.list->len) {
			/* update song */
			playlist_replace(&c->playlist, pos, song);
		} else {
			/* add a new song */
			playlist_append(&c->playlist, song);
		}

		mpd_song_free(song);
	}

	/* remove trailing songs */

	unsigned length = mpd_status_get_queue_length(c->status);
	while (length < c->playlist.list->len) {
		guint pos = c->playlist.list->len - 1;

		/* Remove the last playlist entry */
		playlist_remove(&c->playlist, pos);
	}

	c->song = NULL;
	c->playlist.version = mpd_status_get_queue_version(c->status);

	return mpdclient_finish_command(c);
}


/****************************************************************************/
/*** Filelist functions *****************************************************/
/****************************************************************************/

bool
mpdclient_filelist_add_all(struct mpdclient *c, struct filelist *fl)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);
	if (connection == NULL)
		return false;

	if (filelist_is_empty(fl))
		return true;

	mpd_command_list_begin(connection, false);

	for (unsigned i = 0; i < filelist_length(fl); ++i) {
		struct filelist_entry *entry = filelist_get(fl, i);
		struct mpd_entity *entity  = entry->entity;

		if (entity != NULL &&
		    mpd_entity_get_type(entity) == MPD_ENTITY_TYPE_SONG) {
			const struct mpd_song *song =
				mpd_entity_get_song(entity);

			mpd_send_add(connection, mpd_song_get_uri(song));
		}
	}

	mpd_command_list_end(connection);
	return mpdclient_finish_command(c);
}
