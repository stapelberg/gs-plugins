/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015-2016 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <gnome-software.h>

#include "gs-appstream.h"

#define	GS_APPSTREAM_MAX_SCREENSHOTS	5

GsApp *
gs_appstream_create_app (GsPlugin *plugin, AsApp *item, GError **error)
{
	const gchar *unique_id = as_app_get_unique_id (item);
	GsApp *app = gs_plugin_cache_lookup (plugin, unique_id);
	if (app == NULL) {
		app = gs_app_new (as_app_get_id (item));
		gs_app_set_metadata (app, "GnomeSoftware::Creator",
				     gs_plugin_get_name (plugin));
		if (!gs_appstream_refine_app (plugin, app, item, error))
			return NULL;
		gs_plugin_cache_add (plugin, unique_id, app);
	}
	return app;
}

static AsIcon *
gs_appstream_get_icon_by_kind (AsApp *app, AsIconKind icon_kind)
{
	GPtrArray *icons;
	guint i;

	icons = as_app_get_icons (app);
	for (i = 0; i < icons->len; i++) {
		AsIcon *icon = g_ptr_array_index (icons, i);
		if (as_icon_get_kind (icon) == icon_kind)
			return icon;
	}
	return NULL;
}

static AsIcon *
gs_appstream_get_icon_by_kind_and_size (AsApp *app, AsIconKind icon_kind, guint sz)
{
	GPtrArray *icons;
	guint i;

	icons = as_app_get_icons (app);
	for (i = 0; i < icons->len; i++) {
		AsIcon *icon = g_ptr_array_index (icons, i);
		if (as_icon_get_kind (icon) == icon_kind &&
		    as_icon_get_width (icon) == sz &&
		    as_icon_get_height (icon) == sz)
			return icon;
	}
	return NULL;
}

static void
gs_refine_item_icon (GsPlugin *plugin, GsApp *app, AsApp *item)
{
	AsIcon *icon;

	/* try a stock icon first */
	icon = gs_appstream_get_icon_by_kind (item, AS_ICON_KIND_STOCK);
	if (icon != NULL)
		gs_app_add_icon (app, icon);

	/* if HiDPI get a 128px cached icon */
	if (gs_plugin_get_scale (plugin) == 2) {
		icon = gs_appstream_get_icon_by_kind_and_size (item,
							       AS_ICON_KIND_CACHED,
							       128);
		if (icon != NULL)
			gs_app_add_icon (app, icon);
	}

	/* non-HiDPI cached icon */
	icon = gs_appstream_get_icon_by_kind_and_size (item,
						       AS_ICON_KIND_CACHED,
						       64);
	if (icon != NULL)
		gs_app_add_icon (app, icon);

	/* prefer local */
	icon = gs_appstream_get_icon_by_kind (item, AS_ICON_KIND_LOCAL);
	if (icon != NULL) {
		/* does not exist, so try to find using the icon theme */
		if (as_icon_get_kind (icon) == AS_ICON_KIND_LOCAL &&
		    as_icon_get_filename (icon) == NULL) {
			g_debug ("converting missing LOCAL icon %s to STOCK",
				 as_icon_get_name (icon));
			as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
		}
		gs_app_add_icon (app, icon);
	}

	/* remote as a last resort */
	icon = gs_appstream_get_icon_by_kind (item, AS_ICON_KIND_REMOTE);
	if (icon != NULL)
		gs_app_add_icon (app, icon);
}

static gboolean
gs_appstream_refine_add_addons (GsPlugin *plugin,
				GsApp *app,
				AsApp *item,
				GError **error)
{
	GPtrArray *addons;
	guint i;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* we only care about addons to desktop apps */
	if (gs_app_get_kind (app) != AS_APP_KIND_DESKTOP)
		return TRUE;

	/* search categories for the search term */
	ptask = as_profile_start (gs_plugin_get_profile (plugin),
				  "appstream::refine-addons{%s}",
				  gs_app_get_unique_id (app));
	g_assert (ptask != NULL);

	addons = as_app_get_addons (item);
	if (addons == NULL)
		return TRUE;

	for (i = 0; i < addons->len; i++) {
		AsApp *as_addon = g_ptr_array_index (addons, i);
		g_autoptr(GsApp) addon = NULL;

		addon = gs_appstream_create_app (plugin, as_addon, error);
		if (addon == NULL)
			return FALSE;

		/* add all the data we can */
		if (!gs_appstream_refine_app (plugin, addon, as_addon, error))
			return FALSE;
		gs_app_add_addon (app, addon);
	}
	return TRUE;
}

static void
gs_appstream_refine_add_screenshots (GsApp *app, AsApp *item)
{
	AsScreenshot *ss;
	GPtrArray *images_as;
	GPtrArray *screenshots_as;
	guint i;

	/* do we have any to add */
	screenshots_as = as_app_get_screenshots (item);
	if (screenshots_as->len == 0)
		return;

	/* does the app already have some */
	gs_app_add_kudo (app, GS_APP_KUDO_HAS_SCREENSHOTS);
	if (gs_app_get_screenshots(app)->len > 0)
		return;

	/* add any we know */
	for (i = 0; i < screenshots_as->len &&
		    i < GS_APPSTREAM_MAX_SCREENSHOTS; i++) {
		ss = g_ptr_array_index (screenshots_as, i);
		images_as = as_screenshot_get_images (ss);
		if (images_as->len == 0)
			continue;
		if (as_screenshot_get_kind (ss) == AS_SCREENSHOT_KIND_UNKNOWN)
			continue;
		gs_app_add_screenshot (app, ss);
	}
}

static void
gs_appstream_refine_add_reviews (GsApp *app, AsApp *item)
{
	AsReview *review;
	GPtrArray *reviews;
	guint i;

	/* do we have any to add */
	if (gs_app_get_reviews(app)->len > 0)
		return;
	reviews = as_app_get_reviews (item);
	for (i = 0; i < reviews->len; i++) {
		review = g_ptr_array_index (reviews, i);
		gs_app_add_review (app, review);
	}
}

static void
gs_appstream_refine_add_provides (GsApp *app, AsApp *item)
{
	AsProvide *provide;
	GPtrArray *provides;
	guint i;

	/* do we have any to add */
	if (gs_app_get_provides(app)->len > 0)
		return;
	provides = as_app_get_provides (item);
	for (i = 0; i < provides->len; i++) {
		provide = g_ptr_array_index (provides, i);
		gs_app_add_provide (app, provide);
	}
}

static gboolean
gs_appstream_is_recent_release (AsApp *app)
{
	AsRelease *release;
	GPtrArray *releases;
	guint64 secs;

	/* get newest release */
	releases = as_app_get_releases (app);
	if (releases->len == 0)
		return FALSE;
	release = g_ptr_array_index (releases, 0);

	/* is last build less than one year ago? */
	secs = ((guint64) g_get_real_time () / G_USEC_PER_SEC) -
		as_release_get_timestamp (release);
	return secs / (60 * 60 * 24) < 365;
}

static gboolean
gs_appstream_are_screenshots_perfect (AsApp *app)
{
	AsImage *image;
	AsScreenshot *screenshot;
	GPtrArray *screenshots;
	guint height;
	guint i;
	guint width;

	screenshots = as_app_get_screenshots (app);
	if (screenshots->len == 0)
		return FALSE;
	for (i = 0; i < screenshots->len; i++) {

		/* get the source image as the thumbs will be resized & padded */
		screenshot = g_ptr_array_index (screenshots, i);
		image = as_screenshot_get_source (screenshot);
		if (image == NULL)
			return FALSE;

		width = as_image_get_width (image);
		height = as_image_get_height (image);

		/* too small */
		if (width < AS_IMAGE_LARGE_WIDTH || height < AS_IMAGE_LARGE_HEIGHT)
			return FALSE;

		/* too large */
		if (width > AS_IMAGE_LARGE_WIDTH * 2 || height > AS_IMAGE_LARGE_HEIGHT * 2)
			return FALSE;

		/* not 16:9 */
		if ((width / 16) * 9 != height)
			return FALSE;
	}
	return TRUE;
}

static void
gs_appstream_copy_metadata (GsApp *app, AsApp *item)
{
	GHashTable *hash;
	GList *l;
	g_autoptr(GList) keys = NULL;

	hash = as_app_get_metadata (item);
	keys = g_hash_table_get_keys (hash);
	for (l = keys; l != NULL; l = l->next) {
		const gchar *key = l->data;
		const gchar *value = g_hash_table_lookup (hash, key);
		if (gs_app_get_metadata_item (app, key) != NULL)
			continue;
		gs_app_set_metadata (app, key, value);
	}
}

GsApp *
gs_appstream_create_runtime (GsPlugin *plugin,
			     GsApp *parent,
			     const gchar *runtime)
{
	GsApp *app_cache;
	g_autofree gchar *id = NULL;
	g_autofree gchar *source = NULL;
	g_auto(GStrv) split = NULL;
	g_autoptr(GsApp) app = NULL;

	/* get the name/arch/branch */
	split = g_strsplit (runtime, "/", -1);
	if (g_strv_length (split) != 3)
		return NULL;

	/* create the complete GsApp from the single string */
	id = g_strdup_printf ("%s.runtime", split[0]);
	app = gs_app_new (id);
	source = g_strdup_printf ("runtime/%s", runtime);
	gs_app_add_source (app, source);
	gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_FLATPAK);
	gs_app_set_kind (app, AS_APP_KIND_RUNTIME);
	gs_app_set_branch (app, split[2]);
	gs_app_set_scope (app, gs_app_get_scope (parent));

	/* search in the cache */
	app_cache = gs_plugin_cache_lookup (plugin, gs_app_get_unique_id (app));
	if (app_cache != NULL) {
		/* since the cached runtime can have been created somewhere else
		 * (we're using a global cache), we need to make sure that a
		 * source is set */
		if (gs_app_get_source_default (app_cache) == NULL)
			gs_app_add_source (app_cache, source);
		return g_object_ref (app_cache);
	}

	/* save in the cache */
	gs_plugin_cache_add (plugin, NULL, app);
	return g_steal_pointer (&app);
}

static void
gs_refine_item_management_plugin (GsPlugin *plugin, GsApp *app, AsApp *item)
{
	GPtrArray *bundles;
	const gchar *management_plugin = NULL;
	const gchar *runtime = NULL;
	guint i;

	/* allow override */
	management_plugin = as_app_get_metadata_item (item, "GnomeSoftware::Plugin");
	if (management_plugin != NULL)
		gs_app_set_management_plugin (app, management_plugin);

	/* find the default bundle kind */
	bundles = as_app_get_bundles (item);
	for (i = 0; i < bundles->len; i++) {
		AsBundle *bundle = g_ptr_array_index (bundles, i);
		AsBundleKind kind = as_bundle_get_kind (bundle);

		gs_app_add_source (app, as_bundle_get_id (bundle));

		/* automatically add runtime */
		if (kind == AS_BUNDLE_KIND_FLATPAK) {
			runtime = as_bundle_get_runtime (bundle);
			if (runtime != NULL) {
				g_autoptr(GsApp) app2 = NULL;
				app2 = gs_appstream_create_runtime (plugin, app, runtime);
				if (app2 != NULL) {
					g_debug ("runtime for %s is %s",
						 gs_app_get_unique_id (app),
						 runtime);
					gs_app_set_runtime (app, app2);
				}
			}
			break;
		}
	}
}

static gboolean
gs_appstream_refine_app_updates (GsPlugin *plugin,
				 GsApp *app,
				 AsApp *item,
				 GError **error)
{
	AsUrgencyKind urgency_best = AS_URGENCY_KIND_UNKNOWN;
	GPtrArray *releases;
	g_autoptr(GPtrArray) updates_list = NULL;

	/* make a list of valid updates */
	updates_list = g_ptr_array_new ();
	releases = as_app_get_releases (item);
	for (guint i = 0; i < releases->len; i++) {
		AsRelease *rel = g_ptr_array_index (releases, i);

		/* already installed */
		g_debug ("installable update %s [%u]",
			 as_release_get_version (rel),
			 as_release_get_state (rel));
		if (as_release_get_state (rel) == AS_RELEASE_STATE_INSTALLED)
			continue;

		/* use the 'worst' urgency, e.g. critical over enhancement */
		if (as_release_get_urgency (rel) > urgency_best)
			urgency_best = as_release_get_urgency (rel);

		/* add updates with a description */
		if (as_release_get_description (rel, NULL) == NULL)
			continue;
		g_ptr_array_add (updates_list, rel);
	}

	/* only set if known */
	if (urgency_best != AS_URGENCY_KIND_UNKNOWN)
		gs_app_set_update_urgency (app, urgency_best);

	/* no prefix on each release */
	if (updates_list->len == 1) {
		g_autofree gchar *desc = NULL;
		AsRelease *rel = g_ptr_array_index (updates_list, 0);
		desc = as_markup_convert (as_release_get_description (rel, NULL),
					  AS_MARKUP_CONVERT_FORMAT_SIMPLE,
					  error);
		if (desc == NULL)
			return FALSE;
		gs_app_set_update_details (app, desc);

	/* get the descriptions with a version prefix */
	} else if (updates_list->len > 1) {
		g_autoptr(GString) update_desc = g_string_new ("");
		for (guint i = 0; i < updates_list->len; i++) {
			g_autofree gchar *desc = NULL;
			AsRelease *rel = g_ptr_array_index (updates_list, i);
			desc = as_markup_convert (as_release_get_description (rel, NULL),
						  AS_MARKUP_CONVERT_FORMAT_SIMPLE,
						  error);
			if (desc == NULL)
				return FALSE;
			g_string_append_printf (update_desc,
						"Version %s:\n%s\n\n",
						as_release_get_version (rel),
						desc);
		}

		/* remove trailing newlines */
		if (update_desc->len > 2)
			g_string_truncate (update_desc, update_desc->len - 2);
		gs_app_set_update_details (app, update_desc->str);
	}

	/* if there is no already set update version use the newest */
	if (gs_app_get_update_version (app) == NULL) {
		AsRelease *rel = as_app_get_release_default (item);
		if (rel != NULL)
			gs_app_set_update_version (app, as_release_get_version (rel));
	}

	/* success */
	return TRUE;
}

/**
 * _gs_utils_locale_has_translations:
 * @locale: A locale, e.g. "en_GB"
 *
 * Looks up if the locale is likely to have translations.
 *
 * Returns: %TRUE if the locale should have translations
 **/
static gboolean
_gs_utils_locale_has_translations (const gchar *locale)
{
	if (g_strcmp0 (locale, "C") == 0)
		return FALSE;
	if (g_strcmp0 (locale, "en") == 0)
		return FALSE;
	if (g_strcmp0 (locale, "en_US") == 0)
		return FALSE;
	return TRUE;
}

static AsBundleKind
gs_appstream_get_bundle_kind (AsApp *item)
{
	GPtrArray *bundles;
	GPtrArray *pkgnames;

	/* prefer bundle */
	bundles = as_app_get_bundles (item);
	if (bundles->len > 0) {
		AsBundle *bundle = g_ptr_array_index (bundles, 0);
		if (as_bundle_get_kind (bundle) != AS_BUNDLE_KIND_UNKNOWN)
			return as_bundle_get_kind (bundle);
	}

	/* fallback to packages */
	pkgnames = as_app_get_pkgnames (item);
	if (pkgnames->len > 0)
		return AS_BUNDLE_KIND_PACKAGE;

	/* nothing */
	return AS_BUNDLE_KIND_UNKNOWN;
}

gboolean
gs_appstream_refine_app (GsPlugin *plugin,
			 GsApp *app,
			 AsApp *item,
			 GError **error)
{
	GHashTable *urls;
	GPtrArray *pkgnames;
	GPtrArray *kudos;
	const gchar *tmp;
	guint i;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* search categories for the search term */
	ptask = as_profile_start (gs_plugin_get_profile (plugin),
				  "appstream::refine-app{%s}",
				  gs_app_get_unique_id (app));
	g_assert (ptask != NULL);

	/* set the kind to be more precise */
	if (gs_app_get_kind (app) == AS_APP_KIND_UNKNOWN ||
	    gs_app_get_kind (app) == AS_APP_KIND_GENERIC) {
		gs_app_set_kind (app, as_app_get_kind (item));
	}

	/* is installed already */
	if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN &&
	    as_app_get_state (item) != AS_APP_STATE_UNKNOWN) {
		gs_app_set_state (app, as_app_get_state (item));
	}

	/* types we can never launch */
	switch (gs_app_get_kind (app)) {
	case AS_APP_KIND_ADDON:
	case AS_APP_KIND_CODEC:
	case AS_APP_KIND_FIRMWARE:
	case AS_APP_KIND_FONT:
	case AS_APP_KIND_GENERIC:
	case AS_APP_KIND_INPUT_METHOD:
	case AS_APP_KIND_LOCALIZATION:
	case AS_APP_KIND_OS_UPDATE:
	case AS_APP_KIND_OS_UPGRADE:
	case AS_APP_KIND_RUNTIME:
	case AS_APP_KIND_SOURCE:
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_LAUNCHABLE);
		break;
	default:
		break;
	}

	/* set management plugin automatically */
	gs_refine_item_management_plugin (plugin, app, item);

	/* set id */
	if (as_app_get_id (item) != NULL && gs_app_get_id (app) == NULL)
		gs_app_set_id (app, as_app_get_id (item));

	/* set source */
	if (gs_app_get_metadata_item (app, "appstream::source-file") == NULL) {
		gs_app_set_metadata (app, "appstream::source-file",
				     as_app_get_source_file (item));
	}

	/* scope */
	if (gs_app_get_scope (app) == AS_APP_SCOPE_UNKNOWN &&
	    as_app_get_scope (item) != AS_APP_SCOPE_UNKNOWN)
		gs_app_set_scope (app, as_app_get_scope (item));

	/* set branch */
	if (as_app_get_branch (item) != NULL &&
	    gs_app_get_branch (app) == NULL)
		gs_app_set_branch (app, as_app_get_branch (item));

	/* bundle-kind */
	if (gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_UNKNOWN)
		gs_app_set_bundle_kind (app, gs_appstream_get_bundle_kind (item));

	/* set name */
	tmp = as_app_get_name (item, NULL);
	if (tmp != NULL)
		gs_app_set_name (app, GS_APP_QUALITY_HIGHEST, tmp);

	/* set summary */
	tmp = as_app_get_comment (item, NULL);
	if (tmp != NULL) {
		gs_app_set_summary (app, GS_APP_QUALITY_HIGHEST, tmp);
	}

	/* add urls */
	urls = as_app_get_urls (item);
	if (g_hash_table_size (urls) > 0 &&
	    gs_app_get_url (app, AS_URL_KIND_HOMEPAGE) == NULL) {
		GList *l;
		g_autoptr(GList) keys = NULL;
		keys = g_hash_table_get_keys (urls);
		for (l = keys; l != NULL; l = l->next) {
			gs_app_set_url (app,
					as_url_kind_from_string (l->data),
					g_hash_table_lookup (urls, l->data));
		}
	}

	/* set license */
	if (as_app_get_project_license (item) != NULL && gs_app_get_license (app) == NULL)
		gs_app_set_license (app,
				    GS_APP_QUALITY_HIGHEST,
				    as_app_get_project_license (item));

	/* set keywords */
	if (as_app_get_keywords (item, NULL) != NULL &&
	    gs_app_get_keywords (app) == NULL) {
		gs_app_set_keywords (app, as_app_get_keywords (item, NULL));
		gs_app_add_kudo (app, GS_APP_KUDO_HAS_KEYWORDS);
	}

	/* set origin */
	if (as_app_get_origin (item) != NULL &&
	    gs_app_get_origin (app) == NULL ) {
		tmp = as_app_get_unique_id (item);
		if (tmp != NULL) {
			if (g_str_has_prefix (tmp, "user/flatpak/") ||
			    g_str_has_prefix (tmp, "system/flatpak/"))
				gs_app_set_origin (app, as_app_get_origin (item));
		}
	}

	/* set description */
	tmp = as_app_get_description (item, NULL);
	if (tmp != NULL) {
		g_autofree gchar *from_xml = NULL;
		from_xml = as_markup_convert_simple (tmp, error);
		if (from_xml == NULL) {
			g_prefix_error (error, "trying to parse '%s': ", tmp);
			return FALSE;
		}
		gs_app_set_description (app, GS_APP_QUALITY_HIGHEST, from_xml);
	}

	/* set icon */
	if (as_app_get_icon_default (item) != NULL &&
	    gs_app_get_icons(app)->len == 0)
		gs_refine_item_icon (plugin, app, item);

	/* set categories */
	if (as_app_get_categories (item) != NULL &&
	    gs_app_get_categories (app)->len == 0)
		gs_app_set_categories (app, as_app_get_categories (item));

	/* set project group */
	if (as_app_get_project_group (item) != NULL &&
	    gs_app_get_project_group (app) == NULL)
		gs_app_set_project_group (app, as_app_get_project_group (item));

	/* this is a core application for the desktop and cannot be removed */
	if (as_app_has_compulsory_for_desktop (item, "GNOME") &&
	    gs_app_get_kind (app) == AS_APP_KIND_DESKTOP)
		gs_app_add_quirk (app, AS_APP_QUIRK_COMPULSORY);

	/* set id kind */
	if (gs_app_get_kind (app) == AS_APP_KIND_UNKNOWN)
		gs_app_set_kind (app, as_app_get_kind (item));

	/* copy all the metadata */
	gs_appstream_copy_metadata (app, item);

	/* set package names */
	pkgnames = as_app_get_pkgnames (item);
	if (pkgnames->len > 0 && gs_app_get_sources(app)->len == 0)
		gs_app_set_sources (app, pkgnames);

	/* set addons */
	if (!gs_appstream_refine_add_addons (plugin, app, item, error))
		return FALSE;

	/* set screenshots */
	gs_appstream_refine_add_screenshots (app, item);

	/* set reviews */
	gs_appstream_refine_add_reviews (app, item);

	/* set provides */
	gs_appstream_refine_add_provides (app, item);

	/* are the screenshots perfect */
	if (gs_appstream_are_screenshots_perfect (item))
		gs_app_add_kudo (app, GS_APP_KUDO_PERFECT_SCREENSHOTS);

	/* was this application released recently */
	if (gs_appstream_is_recent_release (item))
		gs_app_add_kudo (app, GS_APP_KUDO_RECENT_RELEASE);

	/* add kudos */
	tmp = gs_plugin_get_locale (plugin);
	if (!_gs_utils_locale_has_translations (tmp) ||
	    as_app_get_language (item, tmp) > 50)
		gs_app_add_kudo (app, GS_APP_KUDO_MY_LANGUAGE);

	/* add a kudo to featured and popular apps */
	if (as_app_has_kudo (item, "GnomeSoftware::popular"))
		gs_app_add_kudo (app, GS_APP_KUDO_FEATURED_RECOMMENDED);
	if (as_app_has_category (item, "featured"))
		gs_app_add_kudo (app, GS_APP_KUDO_FEATURED_RECOMMENDED);

	/* add new-style kudos */
	kudos = as_app_get_kudos (item);
	for (i = 0; i < kudos->len; i++) {
		tmp = g_ptr_array_index (kudos, i);
		switch (as_kudo_kind_from_string (tmp)) {
		case AS_KUDO_KIND_SEARCH_PROVIDER:
			gs_app_add_kudo (app, GS_APP_KUDO_SEARCH_PROVIDER);
			break;
		case AS_KUDO_KIND_USER_DOCS:
			gs_app_add_kudo (app, GS_APP_KUDO_INSTALLS_USER_DOCS);
			break;
		case AS_KUDO_KIND_APP_MENU:
			gs_app_add_kudo (app, GS_APP_KUDO_USES_APP_MENU);
			break;
		case AS_KUDO_KIND_MODERN_TOOLKIT:
			gs_app_add_kudo (app, GS_APP_KUDO_MODERN_TOOLKIT);
			break;
		case AS_KUDO_KIND_NOTIFICATIONS:
			gs_app_add_kudo (app, GS_APP_KUDO_USES_NOTIFICATIONS);
			break;
		case AS_KUDO_KIND_HIGH_CONTRAST:
			gs_app_add_kudo (app, GS_APP_KUDO_HIGH_CONTRAST);
			break;
		case AS_KUDO_KIND_HI_DPI_ICON:
			gs_app_add_kudo (app, GS_APP_KUDO_HI_DPI_ICON);
			break;
		default:
			break;
		}
	}

	/* we saved the origin hostname in the metadata */
	tmp = as_app_get_metadata_item (item, "GnomeSoftware::OriginHostnameUrl");
	if (tmp != NULL && gs_app_get_origin_hostname (app) == NULL)
		gs_app_set_origin_hostname (app, tmp);

	/* is there any update information */
	if (!gs_appstream_refine_app_updates (plugin, app, item, error))
		return FALSE;

	return TRUE;
}

static gboolean
gs_appstream_store_search_item (GsPlugin *plugin,
				AsApp *item,
				gchar **values,
				GsAppList *list,
				GCancellable *cancellable,
				GError **error)
{
	AsApp *item_tmp;
	GPtrArray *addons;
	guint i;
	guint match_value;
	g_autoptr(GsApp) app = NULL;

	/* match against the app or any of the addons */
	match_value = as_app_search_matches_all (item, values);
	addons = as_app_get_addons (item);
	for (i = 0; i < addons->len; i++) {
		item_tmp = g_ptr_array_index (addons, i);
		match_value |= as_app_search_matches_all (item_tmp, values);
	}

	/* no match */
	if (match_value == 0)
		return TRUE;

	/* create app */
	app = gs_appstream_create_app (plugin, item, error);
	if (app == NULL)
		return FALSE;
	gs_app_set_match_value (app, match_value);
	gs_app_list_add (list, app);
	return TRUE;
}

gboolean
gs_appstream_store_search (GsPlugin *plugin,
			   AsStore *store,
			   gchar **values,
			   GsAppList *list,
			   GCancellable *cancellable,
			   GError **error)
{
	AsApp *item;
	GPtrArray *array;
	gboolean ret = TRUE;
	guint i;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* search categories for the search term */
	ptask = as_profile_start_literal (gs_plugin_get_profile (plugin),
					  "appstream::search");
	g_assert (ptask != NULL);
	array = as_store_get_apps (store);
	for (i = 0; i < array->len; i++) {
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			return FALSE;

		item = g_ptr_array_index (array, i);
		ret = gs_appstream_store_search_item (plugin, item,
						      values, list,
						      cancellable, error);
		if (!ret)
			return FALSE;
	}
	return TRUE;
}

static gboolean
_as_app_matches_desktop_group_set (AsApp *app, gchar **desktop_groups)
{
	guint i;
	for (i = 0; desktop_groups[i] != NULL; i++) {
		if (!as_app_has_category (app, desktop_groups[i]))
			return FALSE;
	}
	return TRUE;
}

static gboolean
_as_app_matches_desktop_group (AsApp *app, const gchar *desktop_group)
{
	g_auto(GStrv) split = g_strsplit (desktop_group, "::", -1);
	return _as_app_matches_desktop_group_set (app, split);
}

static void
gs_appstream_store_add_categories_for_app (GsCategory *parent, AsApp *app)
{
	GPtrArray *children;
	GPtrArray *desktop_groups;
	GsCategory *category;
	guint i, j;

	/* find all the sub-categories */
	children = gs_category_get_children (parent);
	for (j = 0; j < children->len; j++) {
		gboolean matched = FALSE;
		category = GS_CATEGORY (g_ptr_array_index (children, j));

		/* do any desktop_groups match this application */
		desktop_groups = gs_category_get_desktop_groups (category);
		for (i = 0; i < desktop_groups->len; i++) {
			const gchar *desktop_group = g_ptr_array_index (desktop_groups, i);
			if (_as_app_matches_desktop_group (app, desktop_group)) {
				matched = TRUE;
				break;
			}
		}
		if (matched) {
			gs_category_increment_size (category);
			gs_category_increment_size (parent);
		}
	}
}

gboolean
gs_appstream_store_add_category_apps (GsPlugin *plugin,
				      AsStore *store,
				      GsCategory *category,
				      GsAppList *list,
				      GCancellable *cancellable,
				      GError **error)
{
	GPtrArray *array;
	GPtrArray *desktop_groups;
	guint i;
	guint j;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* just look at each app in turn */
	ptask = as_profile_start_literal (gs_plugin_get_profile (plugin),
					  "appstream::add-category-apps");
	g_assert (ptask != NULL);
	array = as_store_get_apps (store);
	desktop_groups = gs_category_get_desktop_groups (category);
	if (desktop_groups->len == 0) {
		g_warning ("no desktop_groups for %s", gs_category_get_id (category));
		return TRUE;
	}
	for (j = 0; j < desktop_groups->len; j++) {
		const gchar *desktop_group = g_ptr_array_index (desktop_groups, j);
		g_auto(GStrv) split = g_strsplit (desktop_group, "::", -1);

		/* match the app */
		for (i = 0; i < array->len; i++) {
			AsApp *item;
			g_autoptr(GsApp) app = NULL;

			/* no ID is invalid */
			item = g_ptr_array_index (array, i);
			if (as_app_get_id (item) == NULL)
				continue;

			/* match all the desktop groups */
			if (!_as_app_matches_desktop_group_set (item, split))
				continue;

			/* add all the data we can */
			app = gs_appstream_create_app (plugin, item, error);
			if (app == NULL)
				return FALSE;
			gs_app_list_add (list, app);
		}
	}
	return TRUE;
}

gboolean
gs_appstream_store_add_categories (GsPlugin *plugin,
				   AsStore *store,
				   GPtrArray *list,
				   GCancellable *cancellable,
				   GError **error)
{
	AsApp *app;
	GPtrArray *array;
	guint i;
	guint j;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* find out how many packages are in each category */
	ptask = as_profile_start_literal (gs_plugin_get_profile (plugin),
					  "appstream::add-categories");
	g_assert (ptask != NULL);
	array = as_store_get_apps (store);
	for (i = 0; i < array->len; i++) {
		app = g_ptr_array_index (array, i);
		if (as_app_get_id (app) == NULL)
			continue;
		if (as_app_get_priority (app) < 0)
			continue;
		for (j = 0; j < list->len; j++) {
			GsCategory *parent = GS_CATEGORY (g_ptr_array_index (list, j));
			gs_appstream_store_add_categories_for_app (parent, app);
		}
	}
	return TRUE;
}

gboolean
gs_appstream_add_popular (GsPlugin *plugin,
			  AsStore *store,
			  GsAppList *list,
			  GCancellable *cancellable,
			  GError **error)
{
	AsApp *item;
	GPtrArray *array;
	guint i;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* find out how many packages are in each category */
	ptask = as_profile_start_literal (gs_plugin_get_profile (plugin),
					  "appstream::add-popular");
	g_assert (ptask != NULL);
	array = as_store_get_apps (store);
	for (i = 0; i < array->len; i++) {
		g_autoptr(GsApp) app = NULL;
		item = g_ptr_array_index (array, i);
		if (as_app_get_id (item) == NULL)
			continue;
		if (!as_app_has_kudo (item, "GnomeSoftware::popular"))
			continue;
		app = gs_app_new (as_app_get_id (item));
		gs_app_add_quirk (app, AS_APP_QUIRK_MATCH_ANY_PREFIX);
		gs_app_list_add (list, app);
	}
	return TRUE;
}

gboolean
gs_appstream_add_featured (GsPlugin *plugin,
			   AsStore *store,
			   GsAppList *list,
			   GCancellable *cancellable,
			   GError **error)
{
	AsApp *item;
	GPtrArray *array;
	guint i;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* find out how many packages are in each category */
	ptask = as_profile_start_literal (gs_plugin_get_profile (plugin),
					  "appstream::add-featured");
	g_assert (ptask != NULL);
	array = as_store_get_apps (store);
	for (i = 0; i < array->len; i++) {
		g_autoptr(GsApp) app = NULL;
		item = g_ptr_array_index (array, i);
		if (as_app_get_id (item) == NULL)
			continue;
		if (as_app_get_metadata_item (item, "GnomeSoftware::FeatureTile-css") == NULL)
			continue;
		app = gs_app_new (as_app_get_id (item));
		gs_app_add_quirk (app, AS_APP_QUIRK_MATCH_ANY_PREFIX);
		gs_app_list_add (list, app);
	}
	return TRUE;
}
