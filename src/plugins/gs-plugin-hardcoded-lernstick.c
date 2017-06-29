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

#include <config.h>

#include <glib/gi18n.h>

#include <gnome-software.h>

void
gs_plugin_initialize (GsPlugin *plugin)
{
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "appstream");
}

typedef struct {
	const gchar	*id;
	const gchar	*name;
	const gchar	*fdo_cats[16];
} GsDesktopMap;

typedef struct {
	const gchar	*id;
	const GsDesktopMap *mapping;
	const gchar	*name;
	const gchar	*icon;
	const gchar	*key_colors;
	gint		 score;
} GsDesktopData;


static const GsDesktopMap lmap_audiovisual[] = {
	{ "all",		NC_("Menu of Lernstick", "All"), { NULL }},
    	{ "edu",		NC_("Menu of Lernstick", "Edu"),
					 { "X-Lernstick", 
					   NULL } },
	{ NULL }
};

static const GsDesktopData lmsdata[] = {
	{ "lernstick",	lmap_audiovisual,	N_("Lernstick"),
				"folder-music-symbolic", "#215d9c", 9999 },
	{ NULL }
};

gboolean
gs_plugin_add_categories (GsPlugin *plugin,
			  GPtrArray *list,
			  GCancellable *cancellable,
			  GError **error)
{
	gchar msgctxt[100];
	guint i, j, k;

	for (i = 0; lmsdata[i].id != NULL; i++) {
		GdkRGBA key_color;
		GsCategory *category;

		/* add parent category */
		category = gs_category_new (lmsdata[i].id);
		gs_category_set_icon (category, lmsdata[i].icon);
		gs_category_set_name (category, lmsdata[i].name);
		gs_category_set_score (category, lmsdata[i].score);
		if (gdk_rgba_parse (&key_color, lmsdata[i].key_colors))
			gs_category_add_key_color (category, &key_color);
		g_ptr_array_add (list, category);
		g_snprintf (msgctxt, sizeof(msgctxt),
			    "Menu subcategory of %s", lmsdata[i].name);

		/* add subcategories */
		for (j = 0; lmsdata[i].mapping[j].id != NULL; j++) {
			const GsDesktopMap *map = &lmsdata[i].mapping[j];
			g_autoptr(GsCategory) sub = gs_category_new (map->id);
			for (k = 0; map->fdo_cats[k] != NULL; k++)
				gs_category_add_desktop_group (sub, map->fdo_cats[k]);
			gs_category_set_name (sub, g_dpgettext2 (GETTEXT_PACKAGE,
								 msgctxt,
								 map->name));
			gs_category_add_child (category, sub);
		}
	}
	return TRUE;
}
