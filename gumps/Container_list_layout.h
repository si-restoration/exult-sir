/*
Copyright (C) 2026 The Exult Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#ifndef CONTAINER_LIST_LAYOUT_H
#define CONTAINER_LIST_LAYOUT_H

#include "gumpinf.h"

/*
 *  Geometry of the container list view (Container_list_gump) derived
 *  from its List_layout (the list_layout section of gump_info.txt, whose
 *  defaults are the built-in layout).  All values are in gump-local
 *  pixels, with (0,0) at the top-left of the widened background.  The
 *  list's row arithmetic is in Dynamic_list_layout.h.
 */
namespace Container_list_layout {
	// Width of the widened background.
	inline int total_w(const List_layout& L) {
		return L.bg_width + L.extra_w;
	}

	// Width of the list column, including its scroll controls.
	inline int list_w(const List_layout& L) {
		return L.list.width + 15;
	}

	// Left edge of a picture w pixels wide.
	inline int picture_left(const List_layout& L, int w) {
		return L.pic_right - w;
	}

	// Map a column of the widened gump to a column of the background
	// frame: columns before strip_x1 as they are, then the plain strip
	// (strip_x0 to strip_x1 - 1) repeated over extra_w, then the rest
	// shifted right by extra_w.
	inline int source_column(const List_layout& L, int gx) {
		if (gx < L.strip_x1 || L.extra_w <= 0) {
			return gx;
		}
		if (gx < L.strip_x1 + L.extra_w) {
			return L.strip_x0 + (gx - L.strip_x1) % (L.strip_x1 - L.strip_x0);
		}
		return gx - L.extra_w;
	}

	// Can the background be widened as described?  (Else it is drawn as
	// it is.)
	inline bool can_widen(const List_layout& L) {
		return L.extra_w > 0 && L.strip_x0 >= 0 && L.strip_x0 < L.strip_x1 && L.strip_x1 <= L.bg_width;
	}
	// Where the themed panel goes, in gump-local pixels: the opaque area
	// (ax, ay, aw, ah) of the unwidened background frame, widened the same
	// way as the art (the right margin keeps its size).  An empty area
	// gives the whole widened background.
	struct Panel_box {
		int x, y, w, h;
	};

	inline Panel_box panel_box(const List_layout& L, int ax, int ay, int aw, int ah) {
		if (aw <= 0 || ah <= 0 || ax < 0 || ay < 0 || ax + aw > L.bg_width || ay + ah > L.bg_height) {
			return {0, 0, total_w(L), L.bg_height};
		}
		const int right_margin = L.bg_width - (ax + aw);
		return {ax, ay, total_w(L) - ax - right_margin, ah};
	}
}    // namespace Container_list_layout

#endif
