/*
 *  shadedgrid_data.h - Default override lists for the occlusion grid.
 *
 *  Copyright (C) 2025  The Exult Team
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
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 *  The light_passes_through, roof_shapes and floor_shapes sections below
 *  are copied unchanged from the natural light work by Dominus (Exult
 *  team): data/bg/shape_info.txt and data/si/shape_info.txt of branch
 *  in_vs_out_light at commit f91360f70.  The canopy_shapes section and
 *  anything marked as ours are additions for the occlusion grid.
 *
 *  A mod or patch overrides these with <PATCH>/shaded_grid.txt in the
 *  same format.
 */

#ifndef SHADEDGRID_DATA_H
#define SHADEDGRID_DATA_H 1

#include <string_view>

constexpr std::string_view shaded_grid_bg_text = R"txt(
#
#	Occlusion grid overrides (shaded_grid.txt).
#
#	light_passes_through	:shape/frame/percent
#		The shape's box lets percent of the light through (100 when
#		omitted; 0 makes it opaque even if it is not solid).
#	canopy_shapes		:shape/frame/percent
#		Tree crowns and other partial cover; same values.
#	roof_shapes		:shape
#	floor_shapes		:shape
#		Roofs and upper-storey floors: marked, and opaque unless a
#		light_passes_through row says otherwise.
#	Frame -1 or no frame means all frames; a row for the frame itself
#	(matched on frame & 31) wins over it.  Rows in a patch file replace
#	rows with the same shape and frame.
#
%%section light_passes_through
#
#	These shapes let light pass through from inside to outside.
#	Mostly open doors, shutters, glass windows
#
#	Format:
#		:shapenum/frame/spill percentage
#	No frame number or value of -1 means all the frames.
#	Spill percentage is the percentage of light escaping through the shape.

# broken door
:208/-1/50
:211/-1/50

# door
:270/4/70
:270/5/70
:270/6/70
:270/7/70
:270/8/10
:270/9/10
:270/10/10
:270/11/10
:376/4/70
:376/5/70
:376/6/70
:376/7/70
:376/8/10
:376/9/10
:376/10/10
:376/11/10

# portculli
:271/-1/70
:272/-1/70

# fenced wall
:254/0/20
:254/1/70
:345/3/20
:345/4/70
:350/1/20
:350/2/70
:358/3/20
:371/3/20
:371/4/70
:374/3/20
:497/0/20
:497/1/20
:497/2/20
:497/3/20
:497/4/20
:497/5/20
:869/8/20
:869/9/70
:869/13/80
:871/8/20
:871/9/70
:871/17/80
:879/18/80
:879/19/80
:879/20/80
:879/21/80
:879/22/80
:879/23/80
:904/6/20
:909/6/20
:909/7/70
:910/6/20
:910/7/70

#broken wall
:216/-1/20
:217/-1/20
:218/-1/20
:219/-1/20
:220/-1/20
:221/-1/20
:255/-1/40
:347/-1/20
:356/-1/40
:359/3/40
:359/4/40
:869/10/40
:869/11/60
:869/12/50
:871/10/10
:871/11/20
:871/12/40
:871/13/60
:871/14/40
:871/15/60
:871/16/70
:904/7/30
:904/8/30
:904/9/20
:909/8/50
:909/9/20
:909/10/80
:909/11/70
:909/12/40
:909/13/40
:909/14/20
:909/15/20
:909/16/20
:909/17/40
:909/18/70
:909/19/70
:910/8/50
:910/9/50
:910/10/40
:910/11/30
:910/12/20
:910/13/30
:910/14/40
:910/15/60
:910/16/65
:910/17/70
:910/18/20
:910/19/40
:967/-1/40

# broken roof
:223/-1/60

# sliding metal wall
:303/0/90
:303/1/75
:303/2/50
:303/3/25
:876/0/90
:876/1/75
:876/2/50
:876/3/25
:935
:936

#open shutters
:322/-1/50
:372/-1/50

# glass wall
:373/-1/90
:832/-1/90
:833/-1/90
:841/-1/90
:996/-1/90
:1005/-1/90
:1007/-1/90
:1019/-1/90

# windows
:438/-1/50
:508/-1/50
:512/-1/50
:732/-1/50

# bars
:484/-1/70

# curtains
# Disabled for now as these had bad natural light effects in houses.
#:657/0/70
#:657/1/5
#:657/2/70
#:657/3/5
#:657/4/70
#:657/5/5
#:657/6/70
#:657/7/5
#:657/8/70
#:657/9/5
#:657/10/70
#:657/11/5
#:678/0/70
#:678/1/5
#:678/2/70
#:678/3/5
#:678/4/70
#:678/5/5
#:678/6/70
#:678/7/5
#:678/8/70
#:678/9/5
#:678/10/70
#:678/11/5

# greenhouse roof
:954/-1/90

%%endsection

%%section roof_shapes
:156
:161
:162
:164
:165
:166
:167
:169
:170
:171
:172
:173
:174
:175
:176
:257
:891
:908
:956
:962
:963
:966

%%endsection

%%section floor_shapes
:193
:367
:368
:369
:370
:441
:829
:1014
:1016

%%endsection
%%section canopy_shapes
# Ours, not from Dominus: solid tree shapes seen in the occlusion grid
# dumps (item names from the info files), as 50 % cover.
# tree (Trinsic)
:453/-1/50
%%endsection
)txt";

constexpr std::string_view shaded_grid_si_text = R"txt(
#
#	Occlusion grid overrides (shaded_grid.txt).
#
#	light_passes_through	:shape/frame/percent
#		The shape's box lets percent of the light through (100 when
#		omitted; 0 makes it opaque even if it is not solid).
#	canopy_shapes		:shape/frame/percent
#		Tree crowns and other partial cover; same values.
#	roof_shapes		:shape
#	floor_shapes		:shape
#		Roofs and upper-storey floors: marked, and opaque unless a
#		light_passes_through row says otherwise.
#	Frame -1 or no frame means all frames; a row for the frame itself
#	(matched on frame & 31) wins over it.  Rows in a patch file replace
#	rows with the same shape and frame.
#
%%section light_passes_through
#
#	These shapes let light pass through from inside to outside.
#	Mostly open doors, shutters, glass windows
#
#	Format:
#		:shapenum/frame/spill percentage
#	No frame number or value of -1 means all the frames.
#	Spill percentage is the percentage of light escaping through the shape.

# broken door
:208/-1/50
:211/-1/50

# door
:270/4/70
:270/5/70
:270/6/70
:270/7/70
:270/8/10
:270/9/10
:270/10/10
:270/11/10
:376/4/70
:376/5/70
:376/6/70
:376/7/70
:376/8/10
:376/9/10
:376/10/10
:376/11/10

# portculli
:271/-1/70
:272/-1/70

# fenced wall
:151/12/90
:151/13/90
:151/15/70
:151/17/70
:152/16/90
:152/17/90
:152/22/70
:254/0/20
:254/1/70
:345/3/20
:345/4/70
:346/7/40
:350/1/20
:350/2/70
:358/3/20
:359/8/40
:371/3/20
:371/4/70
:374/3/20
:484/-1/70
:497/0/20
:497/1/20
:497/2/20
:497/3/20
:497/4/20
:497/5/20
:757/1/20
:757/2/40
:757/4/20
:757/5/50
:764/1/20
:764/2/40
:764/4/20
:764/5/40
:769/1/20
:769/2/40
:769/4/20
:769/5/40
:869/8/20
:871/8/20
:871/9/70
:871/17/80
:879/18/80
:879/19/80
:879/20/80
:879/21/80
:879/22/80
:879/23/80
:899/9/40
:904/6/20
:904/12/40
:909/6/20
:909/7/70
:910/6/20
:910/7/70

#broken wall
:205/-1/20
:206/0/20
:206/1/20
:206/2/20
:206/3/20
:216/-1/20
:217/-1/20
:218/2/20
:218/4/30
:218/5/30
:218/6/30
:218/7/40
:218/10/30
:220/0/20
:220/1/20
:220/2/20
:220/3/20
:220/5/70
:253/2/20
:253/4/30
:253/5/30
:253/6/20
:253/7/40
:253/8/20
:253/9/40
:253/10/30
:255/-1/40
:273/4/40
:273/7/40
:308/4/40
:308/7/40
:344/10/70
:344/13/70
:344/17/70
:344/18/70
:347-1/20
:348/5/30
:348/7/30
:348/8/40
:348/9/70
:348/10/40
:348/13/30
:348/14/70
:348/15/40
:348/17/20
:348/18/20
:353/1/50
:356/-1/40
:359/3/40
:359/4/40
:362/8/80
:362/11/80
:362/12/70
:362/13/70
:378/8/20
:378/9/20
:538/8/20
:538/9/20
:748/2/40
:748/4/30
:748/5/40
:748/6/80
:748/7/40
:748/8/20
:748/9/10
:770/2/30
:869/9/30
:869/10/40
:869/12/50
:871/10/10
:871/11/20
:871/12/40
:871/13/60
:871/14/40
:871/15/60
:871/16/70
:899/6/20
:904/7/30
:904/8/30
:904/9/20
:909/8/50
:909/9/20
:909/10/80
:909/11/70
:909/12/40
:909/13/40
:909/14/20
:909/15/20
:909/16/20
:909/17/40
:909/18/70
:909/19/70
:909/20/70
:910/8/50
:910/9/50
:910/10/40
:910/11/30
:910/12/20
:910/13/30
:910/14/40
:910/15/60
:910/16/65
:910/17/70
:910/18/20
:910/19/40
:910/21/70
:967/-1/40
:1006/-1/40

# broken roof
:223/-1/60

# sliding metal wall
:303/0/90
:303/1/75
:303/2/50
:303/3/25
:303/5/95
:303/6/90
:303/7/85
:303/8/80
:303/9/70
:303/10/70
:303/11/50
:303/12/30
:303/13/10
:876/0/90
:876/1/75
:876/2/50
:876/3/25
:876/5/95
:876/6/90
:876/7/85
:876/8/80
:876/9/70
:876/10/70
:876/11/50
:876/12/30
:876/13/10
:935
:936

#open shutters
:290/4/90
:290/5/90
:290/6/90
:290/7/90
:291/4/90
:291/5/90
:291/6/90
:291/7/90

# glass wall
:366/6/90
:373/-1/90
:832/-1/90
:833/-1/90
:841/-1/90
:996/-1/90
:972/4/-1/90
:980/4/-1/90
:1005/-1/90
:1007/-1/90
:1019/-1/90

# windows
:158/1/50
:158/2/50
:158/5/50
:438/-1/50
:508/-1/50
:512/-1/50
:732/-1/50

# bars
:484/-1/70

# curtains
# Disabled for now as these had bad natural light effects in houses.
#:657/0/70
#:657/1/5
#:657/2/70
#:657/3/5
#:657/4/70
#:657/5/5
#:657/6/70
#:657/7/5
#:657/8/70
#:657/9/5
#:657/10/70
#:657/11/5
#:657/12/80
#:657/13/80
#:657/14/5
#:678/0/70
#:678/1/5
#:678/2/70
#:678/3/5
#:678/4/70
#:678/5/5
#:678/6/70
#:678/7/5
#:678/8/70
#:678/9/5
#:678/10/70
#:678/11/5
#:678/12/80
#:678/13/80

# greenhouse roof
:954/-1/90

# force wall
:728/0/80
:748/10/80
:910/24/80

# web of fate
:784/-1/90

# melted door
:884/-1/50

%%endsection

%%section roof_shapes
:156
:161
:162
:164
:165
:166
:167
:169
:170
:171
:172
:173
:174
:175
:257
:908
:956
:962
:963
:966
%%endsection

%%section floor_shapes
:193
:367
:368
:369
:370
:388
:441
:463
:513
:829
:1014
:1016

%%endsection
%%section canopy_shapes
%%endsection
)txt";

#endif
