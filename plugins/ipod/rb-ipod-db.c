/*
 *  arch-tag: Abstraction of libgpod Itdb_ItunesDB object
 *
 *  Copyright (C) 2007 Christophe Fergeau  <teuf@gnome.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA.
 *
 */
#include <string.h>

#include "rb-ipod-db.h"
#include "rb-debug.h"

/*
 * This class is used to handle asynchronous saving of the iPod database 
 * content. We must be really careful when doing that because libgpod API
 * - and Itdb_iTunesDB in particular - isn't thread-safe at all.
 * Thus this class was introduced in an attempt to wrap that complexity, and
 * to hide it as much as possible from RbIpodSource.
 * 
 * When a save request for the iPod metadata is requested through 
 * rb_ipod_db_save_async, we start by delaying the saving by a few seconds
 * (using g_timeout_add) in case we'd get a bunch of very close save requests.
 * When the timeout callback triggers, we start by marking the IpodDB object
 * as read-only, ie while the async save is going on *NO MODIFICATIONS AT ALL
 * MUST BE MADE TO THE ITDB_ITUNESDB OBJECT*. Once the IpodDB is marked as 
 * read-only, we create a thread whose only purpose is to save the 
 * Itdb_iTunesDB content, and once it has done its job, we go back to the main
 * thread through a g_idle_add and mark the IpodDB object as read/write.
 *
 * Since the UI is not blocked during the async database saving (that's the 
 * whole point of this exercise after all ;), we log all IpodDB modifications
 * attempts in IpodDB::delayed_actions, and we'll replay them once the 
 * async saving is done. When these IpodDB modifications should trigger changes
 * in what RB displays (eg, it may be a song removal/addition from/to 
 * the iPod), RbIpodSource should update the GUI immediatly even if the IpodDB
 * hasn't been updated yet (actually, RbIpodSource shouldn't even know if
 * the IpodDB changes it requested were delayed or not).
 *
 * The approach describes above could seem to nicely hide the Itdb_iTunesDB
 * from RbIpodSource and prevent all unwanted modifications of the 
 * Itdb_iTunesDB while it's being saved, but there's unfortunately a big
 * gotcha: the Itdb_Track and Itdb_Playlist classes contains a pointer to the
 * Itdb_iTunesDB they are attached to, and seemingly innocent libgpod functions
 * can be called on those objects and modify the Itdb_iTunesDB through that 
 * pointer without RbIpodDB knowing. As an example, itdb_track_remove is one 
 * such functions. Consequently, one needs to be *really* careful when calling
 * libgpod functions from RbIpodSource and needs to consider if this function
 * should be wrapped in RbIpodDb (with the appropriate delayed handling) 
 * instead of directly calling it from RbIpodSource
 */

typedef struct _RbIpodDelayedAction RbIpodDelayedAction;
static void rb_ipod_free_delayed_action (RbIpodDelayedAction *action);
static void rb_ipod_db_queue_remove_track (RbIpodDb *db,
					   Itdb_Track *track);
static void rb_ipod_db_queue_set_ipod_name (RbIpodDb *db, 
					    const char *new_name);
static void rb_ipod_db_queue_add_track (RbIpodDb *db, Itdb_Track *track);
static void rb_ipod_db_queue_add_playlist (RbIpodDb *db, 
					   Itdb_Playlist *playlist);
static void rb_ipod_db_queue_add_to_playlist (RbIpodDb *ipod_db, 
					      Itdb_Playlist *playlist,
					      Itdb_Track *track);
static void rb_ipod_db_queue_remove_from_playlist (RbIpodDb *ipod_db, 
						   Itdb_Playlist *playlist,
						   Itdb_Track *track);
static void rb_ipod_db_queue_set_thumbnail (RbIpodDb *db,
					    Itdb_Track *track,
					    GdkPixbuf *pixbuf);
static void rb_ipod_db_process_delayed_actions (RbIpodDb *ipod_db);

typedef struct {
	Itdb_iTunesDB *itdb;
	gboolean needs_shuffle_db;

	/* Read-only only applies to the iPod database, it's not an issue if
	 * the track transfer code copies a new song to the iPod while it's set
	 */
	/* FIXME: if a track is copied to the iPod while this is set, and if 
	 * the iPod goes away before we process the delayed actions and sync
	 * the database, then the copied file will be leaked (ie hidden in the 
	 * iPod directory tree but not accessible from the iPod UI)
	 */
	gboolean read_only;
	GQueue *delayed_actions;
	GThread *saving_thread;

	guint save_timeout_id;
	guint save_idle_id;

} RbIpodDbPrivate;

G_DEFINE_TYPE (RbIpodDb, rb_ipod_db, G_TYPE_OBJECT)

#define IPOD_DB_GET_PRIVATE(o)   (G_TYPE_INSTANCE_GET_PRIVATE ((o), RB_TYPE_IPOD_DB, RbIpodDbPrivate))

static void rb_itdb_save (RbIpodDb *ipod_db, GError **error)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	itdb_write (priv->itdb, error);
	if ((error != NULL) && (*error != NULL)) {
		return;
	}
	if (priv->needs_shuffle_db) {
		itdb_shuffle_write (priv->itdb, error);
	}
}

static void
rb_ipod_db_init (RbIpodDb *db)

{	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (db);

	priv->delayed_actions = g_queue_new ();
}

static void 
rb_ipod_db_dispose (GObject *object)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (object);
	gboolean db_dirty = FALSE;

	if (priv->saving_thread != NULL) {
		g_thread_join (priv->saving_thread);
		priv->saving_thread = NULL;
	}

	if (priv->save_idle_id != 0) {
		g_source_remove (priv->save_idle_id);
		priv->save_idle_id = 0;
	}

	/* Be careful, the order of the following cleanups is important, first
	 * we process the queued ipod database modifications, which may trigger
	 * modifications of save_timeout_id, then we make sure we cancel 
	 * potential in progress saves, and finally we sync the database
	 */
	if (priv->delayed_actions) {
		
		if (g_queue_get_length (priv->delayed_actions) != 0) {
			rb_ipod_db_process_delayed_actions (RB_IPOD_DB(object));
			db_dirty = TRUE;
		}
		/* The queue should be empty, but better be safe than 
		 * leaking 
		 */
		g_queue_foreach (priv->delayed_actions, 
				 (GFunc)rb_ipod_free_delayed_action,
				 NULL);
		g_queue_free (priv->delayed_actions);
		priv->delayed_actions = NULL;
	}

	if (priv->save_timeout_id != 0) {
		g_source_remove (priv->save_timeout_id);
		priv->save_timeout_id = 0;
		db_dirty = TRUE;
	}

 	if (priv->itdb != NULL) {
		if (db_dirty) {
			rb_itdb_save (RB_IPOD_DB (object), NULL);
		}
 		itdb_free (priv->itdb);

 		priv->itdb = NULL;
	}


	G_OBJECT_CLASS (rb_ipod_db_parent_class)->dispose (object);
}

static void
rb_ipod_db_class_init (RbIpodDbClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->dispose = rb_ipod_db_dispose;

	g_type_class_add_private (klass, sizeof (RbIpodDbPrivate));
}

enum _RbIpodDelayedActionType {
	RB_IPOD_ACTION_SET_NAME,
	RB_IPOD_ACTION_ADD_TRACK,
	RB_IPOD_ACTION_ADD_PLAYLIST,
	RB_IPOD_ACTION_REMOVE_TRACK,
	RB_IPOD_ACTION_SET_THUMBNAIL,
	RB_IPOD_ACTION_ADD_TO_PLAYLIST,
	RB_IPOD_ACTION_REMOVE_FROM_PLAYLIST
};
typedef enum _RbIpodDelayedActionType RbIpodDelayedActionType;

struct _RbIpodDelayedSetThumbnail {
	Itdb_Track *track;
	GdkPixbuf *pixbuf;
};
typedef struct _RbIpodDelayedSetThumbnail RbIpodDelayedSetThumbnail;

struct _RbIpodDelayedPlaylistOp {
	Itdb_Playlist *playlist;
	Itdb_Track *track;
};
typedef struct _RbIpodDelayedPlaylistOp RbIpodDelayedPlaylistOp;

struct _RbIpodDelayedAction {
	RbIpodDelayedActionType type;
	union {
		char *name;
		Itdb_Playlist *playlist;
		Itdb_Track *track;
		RbIpodDelayedSetThumbnail thumbnail_data;
		RbIpodDelayedPlaylistOp playlist_op;
	};
};

static void rb_ipod_free_delayed_action (RbIpodDelayedAction *action) 
{
	switch (action->type) {
	case RB_IPOD_ACTION_SET_NAME:
		g_free (action->name);
		break;
	case RB_IPOD_ACTION_SET_THUMBNAIL:
		g_object_unref (action->thumbnail_data.pixbuf);
		break;
	case RB_IPOD_ACTION_ADD_TRACK:
		itdb_track_free (action->track);
		break;
	case RB_IPOD_ACTION_ADD_PLAYLIST:
		itdb_playlist_free (action->playlist);
		break;
	case RB_IPOD_ACTION_REMOVE_TRACK:
		/* Do nothing */
		break;
	case RB_IPOD_ACTION_ADD_TO_PLAYLIST:
		/* Do nothing */
		break;
	case RB_IPOD_ACTION_REMOVE_FROM_PLAYLIST:
		/* Do nothing */
		break;
	}
	g_free (action);
}

const char *rb_ipod_db_get_ipod_name (RbIpodDb *db)
{
	Itdb_Playlist *mpl;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (db);
	mpl = itdb_playlist_mpl (priv->itdb);
	if (mpl == NULL) {
		rb_debug ("Couldn't find iPod master playlist");
		return NULL;
	}

	return mpl->name;	
}

static void
rb_ipod_db_set_ipod_name_internal (RbIpodDb *ipod_db, const char *name)
{
	Itdb_Playlist *mpl;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	mpl = itdb_playlist_mpl (priv->itdb);
	if (mpl != NULL) {
		if (mpl->name != NULL) {
			rb_debug ("Renaming iPod from %s to %s", mpl->name, name);
			if (strcmp (mpl->name, name) == 0) {
				rb_debug ("iPod is already named %s", name);
				return;
			}
		}
		g_free (mpl->name);
		mpl->name = g_strdup (name);
	} else {
		g_warning ("iPod's master playlist is missing");
	}

	rb_ipod_db_save_async (ipod_db);
}

static void
rb_ipod_db_remove_track_internal (RbIpodDb *ipod_db, Itdb_Track *track)
{
	GList *it;

	for (it = track->itdb->playlists; it != NULL; it = it->next) {
		itdb_playlist_remove_track ((Itdb_Playlist *)it->data, track);
	}
	itdb_track_remove (track);

	rb_ipod_db_save_async (ipod_db);
}

static void
rb_ipod_db_set_thumbnail_internal (RbIpodDb *ipod_db, Itdb_Track *track, 
				   GdkPixbuf *pixbuf)
{
	gchar *image_data;
	gsize image_data_len;
	GError *err = NULL;
	gboolean success;

	g_return_if_fail (track != NULL);
	g_return_if_fail (pixbuf != NULL);

#ifdef HAVE_ITDB_TRACK_SET_THUMBNAILS_FROM_PIXBUF
	itdb_track_set_thumbnails_from_pixbuf (track, pixbuf);
#else /* HAVE_ITDB_TRACK_SET_THUMBNAILS_FROM_PIXBUF */

	success = gdk_pixbuf_save_to_buffer (pixbuf,
					     &image_data, &image_data_len,
					     "jpeg", &err,
					     "quality", "100",
					     NULL);
	if (!success) {
		g_assert (image_data == NULL);
		g_warning ("Failed to save pixbuf to buffer %s", err->message);
		g_error_free (err);
		return;
	}

	itdb_track_set_thumbnails_from_data (track, (guchar *) image_data, 
					     image_data_len);
	g_free (image_data);
#endif /* HAVE_ITDB_TRACK_SET_THUMBNAILS_FROM_PIXBUF */

	rb_ipod_db_save_async (ipod_db);
}


static void
rb_ipod_db_add_playlist_internal (RbIpodDb *ipod_db, 
				  Itdb_Playlist *playlist)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	itdb_playlist_add (priv->itdb, playlist, -1);

	rb_ipod_db_save_async (ipod_db);
}

static void
rb_ipod_db_add_track_internal (RbIpodDb *ipod_db, Itdb_Track *track)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	itdb_track_add (priv->itdb, track, -1);
	itdb_playlist_add_track (itdb_playlist_mpl (priv->itdb),
				 track, -1);

	rb_ipod_db_save_async (ipod_db);
}

static void
rb_ipod_db_add_to_playlist_internal (RbIpodDb* ipod_db,
				     Itdb_Playlist *playlist,
				     Itdb_Track *track)
{
	itdb_playlist_add_track (playlist, track, -1);
	rb_ipod_db_save_async (ipod_db);
}

static void 
rb_ipod_db_remove_from_playlist_internal (RbIpodDb* ipod_db, 
					  Itdb_Playlist *playlist,
					  Itdb_Track *track)
{
	itdb_playlist_remove_track (playlist, track);
	rb_ipod_db_save_async (ipod_db);
}


void rb_ipod_db_set_thumbnail (RbIpodDb* ipod_db, Itdb_Track *track, 
			       GdkPixbuf *pixbuf)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	if (priv->read_only) {
		rb_ipod_db_queue_set_thumbnail (ipod_db, track, pixbuf);
	} else {
		rb_ipod_db_set_thumbnail_internal (ipod_db, track, pixbuf);
	}
}

void rb_ipod_db_add_track (RbIpodDb* ipod_db, Itdb_Track *track)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	if (priv->read_only) {
		rb_ipod_db_queue_add_track (ipod_db, track);
	} else {
		rb_ipod_db_add_track_internal (ipod_db, track);
	}
}

void rb_ipod_db_add_playlist (RbIpodDb* ipod_db, Itdb_Playlist *playlist)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	if (priv->read_only) {
		rb_ipod_db_queue_add_playlist (ipod_db, playlist);
	} else {
		rb_ipod_db_add_playlist_internal (ipod_db, playlist);
	}
}

void rb_ipod_db_remove_track (RbIpodDb* ipod_db, Itdb_Track *track)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	if (priv->read_only) {
		rb_ipod_db_queue_remove_track (ipod_db, track);
	} else {
		rb_ipod_db_remove_track_internal (ipod_db, track);
	}
}

void rb_ipod_db_set_ipod_name (RbIpodDb *ipod_db, const char *name)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	if (priv->read_only) {
		rb_ipod_db_queue_set_ipod_name (ipod_db, name);
	} else {
		rb_ipod_db_set_ipod_name_internal (ipod_db, name);
	}
}

void rb_ipod_db_add_to_playlist (RbIpodDb* ipod_db, Itdb_Playlist *playlist,
				 Itdb_Track *track)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	if (priv->read_only) {
		rb_ipod_db_queue_add_to_playlist (ipod_db, playlist, track);
	} else {
		rb_ipod_db_add_to_playlist_internal (ipod_db, playlist, track);
	}	
}

void rb_ipod_db_remove_from_playlist (RbIpodDb* ipod_db, 
				      Itdb_Playlist *playlist,
				      Itdb_Track *track)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	if (priv->read_only) {
		rb_ipod_db_queue_remove_from_playlist (ipod_db, playlist,
						       track);
	} else {
		rb_ipod_db_remove_from_playlist_internal (ipod_db, 
							  playlist, track);
	}	
}


static void 
rb_ipod_db_process_delayed_actions (RbIpodDb *ipod_db)
{
	RbIpodDelayedAction *action;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	rb_debug ("Handling delayed iPod actions");

	action = g_queue_pop_head (priv->delayed_actions);
	if (action != NULL) {
		/* Schedule a database save if there was at least one delayed
		 * action
		 */
		rb_ipod_db_save_async (ipod_db);
	}
	while (action != NULL) {
		switch (action->type) {
		case RB_IPOD_ACTION_SET_NAME:
			rb_debug ("IPOD_ACTION_SET_NAME (%s)",
				  action->name);
			rb_ipod_db_set_ipod_name_internal (ipod_db,
							   action->name);
			break;
		case RB_IPOD_ACTION_SET_THUMBNAIL:
			rb_debug ("IPOD_ACTION_SET_THUMBNAIL");
			rb_ipod_db_set_thumbnail_internal (ipod_db,
							   action->thumbnail_data.track,
							   action->thumbnail_data.pixbuf);
			break;
		case RB_IPOD_ACTION_REMOVE_TRACK:
			rb_debug ("IPOD_ACTION_REMOVE_TRACK");
			rb_ipod_db_remove_track_internal (ipod_db,
							  action->track);
			break;
		case RB_IPOD_ACTION_ADD_TRACK:
			rb_debug ("IPOD_ACTION_ADD_TRACK");
			rb_ipod_db_add_track_internal (ipod_db, action->track);
			/* The track was added to the iPod database, 'action'
			 * is no longer responsible for its memory handling
			 */
			action->track = NULL;
			break;
		case RB_IPOD_ACTION_ADD_PLAYLIST:
			rb_debug ("IPOD_ACTION_ADD_PLAYLIST");
			rb_ipod_db_add_playlist_internal (ipod_db, 
							  action->playlist);
			/* The playlist was added to the iPod database, 
			 * 'action' is no longer responsible for its memory 
			 * handling
			 */
			action->playlist = NULL;
			break;
		case RB_IPOD_ACTION_ADD_TO_PLAYLIST:
			rb_debug ("IPOD_ACTION_ADD_TO_PLAYLIST");
			rb_ipod_db_add_to_playlist_internal (ipod_db, 
							     action->playlist_op.playlist,
							     action->playlist_op.track);
			break;
		case RB_IPOD_ACTION_REMOVE_FROM_PLAYLIST:
			rb_debug ("IPOD_ACTION_REMOVE_FROM_PLAYLIST");
			rb_ipod_db_remove_from_playlist_internal (ipod_db, 
								  action->playlist_op.playlist,
								  action->playlist_op.track);
			break;
		}
		rb_ipod_free_delayed_action (action);
		action = g_queue_pop_head (priv->delayed_actions);
	}
}

static void rb_ipod_db_queue_remove_track (RbIpodDb *ipod_db,
					   Itdb_Track *track)
{
	RbIpodDelayedAction *action;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);
	
	g_assert (priv->read_only);
	rb_debug ("Queueing move to trash action since the iPod database is currently read-only");
	action = g_new0 (RbIpodDelayedAction, 1);
	action->type = RB_IPOD_ACTION_REMOVE_TRACK;
	action->track = track;
	g_queue_push_tail (priv->delayed_actions, action);
}

static void rb_ipod_db_queue_set_ipod_name (RbIpodDb *ipod_db, 
					    const char *new_name)
{
	RbIpodDelayedAction *action;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);
		
	g_assert (priv->read_only);
	rb_debug ("Queueing set name action since the iPod database is currently read-only");
	action = g_new0 (RbIpodDelayedAction, 1);
	action->type = RB_IPOD_ACTION_SET_NAME;
	action->name = g_strdup (new_name);
	g_queue_push_tail (priv->delayed_actions, action);
}

static void rb_ipod_db_queue_add_playlist (RbIpodDb *ipod_db,
					   Itdb_Playlist *playlist)
{
	RbIpodDelayedAction *action;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);
	
	g_assert (priv->read_only);
	rb_debug ("Queueing add track action since the iPod database is currently read-only");
	action = g_new0 (RbIpodDelayedAction, 1);
	action->type = RB_IPOD_ACTION_ADD_PLAYLIST;
	action->playlist = playlist;
	g_queue_push_tail (priv->delayed_actions, action);
}

static void rb_ipod_db_queue_add_track (RbIpodDb *ipod_db, Itdb_Track *track)
{
	RbIpodDelayedAction *action;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);
	
	g_assert (priv->read_only);
	rb_debug ("Queueing add track action since the iPod database is currently read-only");
	action = g_new0 (RbIpodDelayedAction, 1);
	action->type = RB_IPOD_ACTION_ADD_TRACK;
	action->track = track;
	g_queue_push_tail (priv->delayed_actions, action);
}

static void rb_ipod_db_queue_add_to_playlist (RbIpodDb *ipod_db, 
					      Itdb_Playlist *playlist,
					      Itdb_Track *track)
{
	RbIpodDelayedAction *action;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);
	
	g_assert (priv->read_only);
	rb_debug ("Queueing add to playlist action since the iPod database is currently read-only");
	action = g_new0 (RbIpodDelayedAction, 1);
	action->type = RB_IPOD_ACTION_ADD_TO_PLAYLIST;
	action->playlist_op.playlist = playlist;
	action->playlist_op.track = track;
	g_queue_push_tail (priv->delayed_actions, action);
}

static void rb_ipod_db_queue_remove_from_playlist (RbIpodDb *ipod_db, 
						   Itdb_Playlist *playlist,
						   Itdb_Track *track)
{
	RbIpodDelayedAction *action;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);
	
	g_assert (priv->read_only);
	rb_debug ("Queueing remove from playlist action since the iPod database is currently read-only");
	action = g_new0 (RbIpodDelayedAction, 1);
	action->type = RB_IPOD_ACTION_REMOVE_FROM_PLAYLIST;
	action->playlist_op.playlist = playlist;
	action->playlist_op.track = track;
	g_queue_push_tail (priv->delayed_actions, action);
}

static void rb_ipod_db_queue_set_thumbnail (RbIpodDb *ipod_db,
					    Itdb_Track *track,
					    GdkPixbuf *pixbuf)
{
	RbIpodDelayedAction *action;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);
	
	g_assert (priv->read_only);
	rb_debug ("Queueing set thumbnail action since the iPod database is currently read-only");
	action = g_new0 (RbIpodDelayedAction, 1);
	action->type = RB_IPOD_ACTION_SET_THUMBNAIL;
	action->thumbnail_data.track = track;
	action->thumbnail_data.pixbuf = g_object_ref (G_OBJECT (track));
	g_queue_push_tail (priv->delayed_actions, action);
}

static gchar *
rb_ipod_db_get_volume_path (GnomeVFSVolume *volume)
{
	gchar *path;
	gchar *uri;

	uri = gnome_vfs_volume_get_activation_uri (volume);
	path = g_filename_from_uri (uri, NULL, NULL);
	g_assert (path != NULL);
	g_free (uri);

	return path;
}

static void rb_ipod_db_load (RbIpodDb *ipod_db, GnomeVFSVolume *volume)
{
	char *mount_path;
	const Itdb_IpodInfo *info;
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	mount_path = rb_ipod_db_get_volume_path (volume);
 	priv->itdb = itdb_parse (mount_path, NULL);
	g_free (mount_path);

	info = itdb_device_get_ipod_info(priv->itdb->device);
	if (info->ipod_generation == ITDB_IPOD_GENERATION_UNKNOWN ||
	    info->ipod_model == ITDB_IPOD_MODEL_SHUFFLE) {
		priv->needs_shuffle_db = TRUE;
	} else {
		priv->needs_shuffle_db = FALSE;
	}
}

RbIpodDb *rb_ipod_db_new (GnomeVFSVolume *volume)
{
	RbIpodDb *db;

	g_return_val_if_fail (volume != NULL, NULL);

	db = g_object_new (RB_TYPE_IPOD_DB, NULL);
	if (db == NULL) {
		return NULL;
	}

	rb_ipod_db_load (db, volume);

	return db;
}


/* Threaded iTunesDB save */
static gboolean
ipod_db_saved_idle_cb (RbIpodDb *ipod_db)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	g_assert (priv->read_only);
	g_thread_join (priv->saving_thread);
	priv->saving_thread = NULL;
	priv->read_only = FALSE;
	rb_debug ("Switching iPod database to read-write");

	rb_ipod_db_process_delayed_actions (ipod_db);

	priv->save_idle_id = 0;

	rb_debug ("End of iPod database save");
	return FALSE;
}

static gpointer saving_thread (RbIpodDb *ipod_db)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	g_assert (priv->read_only);

	rb_debug ("Writing iPod database to disk");
	rb_itdb_save (ipod_db, NULL);
	priv->save_idle_id = g_idle_add ((GSourceFunc)ipod_db_saved_idle_cb, 
					 ipod_db);
	
	return NULL;
}

static gboolean
save_timeout_cb (RbIpodDb *ipod_db)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	if (priv->read_only) {
		g_warning ("Database is read-only, not saving");
		return TRUE;
	}
	rb_debug ("Starting iPod database save");
	rb_debug ("Switching iPod database to read-only");
	priv->read_only = TRUE;
	
	priv->saving_thread = g_thread_create ((GThreadFunc)saving_thread,
					       ipod_db, TRUE, NULL);
	priv->save_timeout_id = 0;
	return FALSE;
}

void
rb_ipod_db_save_async (RbIpodDb *ipod_db)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	if (priv->save_timeout_id == 0) {
		rb_debug ("Scheduling iPod database save in 15 seconds");
		priv->save_timeout_id = g_timeout_add (15000, 
						       (GSourceFunc)save_timeout_cb,
						       ipod_db);
	} else {
		rb_debug ("Database save already scheduled");
	}
}

GList *rb_ipod_db_get_playlists (RbIpodDb *ipod_db)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	return priv->itdb->playlists;
}

GList *rb_ipod_db_get_tracks (RbIpodDb *ipod_db)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	return priv->itdb->tracks;
}

const char *rb_ipod_db_get_mount_path (RbIpodDb *ipod_db)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);

	return itdb_get_mountpoint (priv->itdb);
}

Itdb_Device *rb_ipod_db_get_device (RbIpodDb *ipod_db)
{
	RbIpodDbPrivate *priv = IPOD_DB_GET_PRIVATE (ipod_db);
	if (priv->itdb == NULL) {
		return NULL;
	}

	return priv->itdb->device;
}