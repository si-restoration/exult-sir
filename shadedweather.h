/*
 *  shadedweather.h - Weather in the "shaded light" renderer.
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
 *  The weather Exult's effects show (overcast, clouds, fog, lightning,
 *  rain and snow) as changes of the shaded light: the sky's share of a
 *  pixel's light is dimmed by cloud patches, only where the pixel is open
 *  to the sky.
 *
 *  Like the rest of shaded light, everything is plain data: the sky map
 *  (R8 over the occlusion grid's columns, 255 = open, fading to 0 under
 *  cover), cloud masks (R8, one per cloud frame) and fixed-size records,
 *  so a shader can mirror the inline helpers below unchanged.
 */

#ifndef SHADEDWEATHER_H
#define SHADEDWEATHER_H 1

#include "shadedgrid.h"
#include "shadedlight.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class Shape_frame;

/*
 *  One drifting cloud, as Clouds_effect has it: the world pixel of its
 *  sprite's origin (drawn at lift 0) and its frame.
 */
struct Shaded_cloud_source {
	long         wx, wy;
	Shape_frame* frame;
};

/*
 *  The weather the effects report, gathered every paint.
 */
struct Shaded_weather_input {
	std::vector<Shaded_cloud_source> clouds;
	bool                             clouds_effect = false;    // A Clouds_effect exists.
	bool                             fog           = false;    // A started Fog_effect.
	bool                             rain          = false;
	bool                             snow          = false;
};

/*
 *  A cloud of the current paint (std430): world pixel of the sprite's
 *  origin and index into Shaded_weather_set::masks.
 */
struct Shaded_cloud {
	float   wx = 0.0f, wy = 0.0f;
	int32_t mask = 0;
	int32_t pad  = 0;
};

static_assert(sizeof(Shaded_cloud) == 16);

/*
 *  A soft coverage mask of a cloud frame (std430): w x h bytes at offset
 *  in Shaded_weather_set::mask_data, 0 = clear .. 255 = full cover; mask
 *  pixel (0, 0) is (x0, y0) pixels from the sprite's origin.
 */
struct Shaded_cloud_mask {
	int32_t  x0 = 0, y0 = 0, w = 0, h = 0;
	uint32_t offset = 0;
	int32_t  pad[3] = {0, 0, 0};
};

static_assert(sizeof(Shaded_cloud_mask) == 32);

// World pixels round the wrapped world.
constexpr int32_t shaded_world_pixels = c_num_tiles * c_tilesize;

/*
 *  Everything the weather terms of the light pass read.
 */
struct Shaded_weather_set {
	const Shaded_grid_volume* grid = nullptr;    // Null: every pixel is open.
	// The sky map over the grid's columns: open_w x open_h, column (0, 0)
	// at world tile (open_tx, open_ty); 255 open .. 0 covered.
	int32_t                        open_w = 0, open_h = 0, open_tx = 0, open_ty = 0;
	std::vector<uint8_t>           open;
	std::vector<Shaded_cloud>      clouds;
	std::vector<Shaded_cloud_mask> masks;
	std::vector<uint8_t>           mask_data;
	// Lightning (FEAT-003): the flash map, -1 = none.
	Shaded_shadow_set flash;
	int32_t           flash_map = -1;
	// Rain and snow (FEAT-004): 1 per pixel painted by a drop.
	const uint8_t* particles = nullptr;
};

/*
 *  The weather outputs of the light pass, indexed like the buffers (any
 *  may be null).
 */
struct Shaded_weather_out {
	uint8_t* sky   = nullptr;    // 0 = covered, else 1 + 254 * sky factor.
	uint8_t* fog   = nullptr;
	uint8_t* flash = nullptr;
};

/*
 *  How open to the sky a surface point p with normal n is, 0..1.  The
 *  column sampled is the one in front of the face (half a tile along the
 *  normal's x and y).  At or above the column's cover (roof tops, open
 *  ground) it is 1; below it, the sky map bilinear over the 4 columns round
 *  that point, leaving out columns solid at p's lift (walls), so the sky
 *  light fades in at doorways and never comes through a wall.
 */
inline float Shaded_weather_open(const Shaded_weather_set& set, const Shaded_point& p, const Shaded_normalf& n) {
	const Shaded_grid_volume* vol = set.grid;
	if (!vol || set.open_w <= 0 || set.open_h <= 0) {
		return 1.0f;
	}
	auto column = [](float w, int origin, int size) {
		int c = static_cast<int>(std::floor(w)) - origin;
		c %= c_num_tiles;
		c += c < 0 ? c_num_tiles : 0;
		return c < size ? c : -1;
	};
	const float qx = p.x + 0.5f * n.x;
	const float qy = p.y + 0.5f * n.y;
	const int   tx = column(qx, set.open_tx, set.open_w);
	const int   ty = column(qy, set.open_ty, set.open_h);
	if (tx < 0 || ty < 0) {
		return 1.0f;    // Off the grid: nothing known to cover it.
	}
	const size_t col = static_cast<size_t>(ty) * static_cast<size_t>(set.open_w) + static_cast<size_t>(tx);
	if (p.z + 1.0f / 32.0f >= static_cast<float>(vol->sky[col])) {
		return 1.0f;
	}
	const int   lift = std::clamp(static_cast<int>(std::floor(p.z)), 0, vol->size_z - 1);
	const float gx   = qx - 0.5f;
	const float gy   = qy - 0.5f;
	const float fx   = gx - std::floor(gx);
	const float fy   = gy - std::floor(gy);
	const int   cx   = column(gx, set.open_tx, set.open_w);
	const int   cy   = column(gy, set.open_ty, set.open_h);
	float       wsum = 0.0f;
	float       vsum = 0.0f;
	for (int i = 0; i < 4; i++) {
		// Neighbours past the grid's edge are left out.
		const int x = cx < 0 ? -1 : cx + (i & 1);
		const int y = cy < 0 ? -1 : cy + (i >> 1);
		if (x < 0 || y < 0 || x >= set.open_w || y >= set.open_h
			|| vol->opacity(x, y, lift) >= shaded_grid_sky_threshold) {
			continue;
		}
		const float w = ((i & 1) ? fx : 1.0f - fx) * ((i & 2) ? fy : 1.0f - fy);
		wsum += w;
		vsum += w * static_cast<float>(set.open[static_cast<size_t>(y) * static_cast<size_t>(set.open_w) + static_cast<size_t>(x)]);
	}
	if (wsum <= 1e-6f) {
		return static_cast<float>(set.open[col]) / 255.0f;
	}
	return vsum / (wsum * 255.0f);
}

/*
 *  Cloud cover over world point p, 0..1: p's ground projection (a cloud
 *  sprite is drawn at lift 0) in each cloud's mask, summed and capped at 1
 *  (darkening multiplied per cloud goes black where many overlap).
 */
inline float Shaded_cloud_cover(const Shaded_weather_set& set, const Shaded_point& p) {
	constexpr auto world = static_cast<float>(shaded_world_pixels);
	// Shortest way round the wrapped world, in pixels.
	auto wrap = [](float d) {
		d = std::fmod(d, world);
		if (d > world * 0.5f) {
			d -= world;
		} else if (d < -world * 0.5f) {
			d += world;
		}
		return d;
	};
	float cover = 0.0f;
	for (const Shaded_cloud& c : set.clouds) {
		const Shaded_cloud_mask& m  = set.masks[static_cast<size_t>(c.mask)];
		const float              u  = wrap(p.x * static_cast<float>(c_tilesize) - c.wx);
		const float              v  = wrap(p.y * static_cast<float>(c_tilesize) - c.wy);
		const int                mx = static_cast<int>(std::floor(u)) - m.x0;
		const int                my = static_cast<int>(std::floor(v)) - m.y0;
		if (mx < 0 || my < 0 || mx >= m.w || my >= m.h) {
			continue;
		}
		cover += static_cast<float>(set.mask_data[m.offset + static_cast<size_t>(my) * static_cast<size_t>(m.w) + static_cast<size_t>(mx)])
				 / 255.0f;
		if (cover >= 1.0f) {
			return 1.0f;
		}
	}
	return cover;
}

// Fog tables: 64 rows of 256, row code fk + 8 * fl (fog amount fk / 8, fog
// light fl / 7, both 0..7).
constexpr int shaded_fog_rows       = 64;
constexpr int shaded_fog_table_size = shaded_fog_rows * 256;

/*
 *  How much fog is in front of point p (lifts above ground, tiles), as
 *  open to the sky as open, 0..7/8: none within fog_clear tiles of the
 *  Avatar, thickening with the horizontal distance beyond (fog_distance
 *  tiles to 63 %) and thinning with height (fog_height lifts to 37 %),
 *  reaching fog_mix (the FOG palette's share) far off on the ground.
 */
inline float Shaded_fog_factor(const Shaded_light_params& params, const Shaded_point& p, float open) {
	if (open <= 0.0f || params.fog_density <= 0.0f) {
		return 0.0f;
	}
	constexpr auto tiles = static_cast<float>(c_num_tiles);
	// Shortest way round the wrapped world, in tiles.
	auto wrap = [](float d) {
		d = std::fmod(d, tiles);
		if (d > tiles * 0.5f) {
			d -= tiles;
		} else if (d < -tiles * 0.5f) {
			d += tiles;
		}
		return d;
	};
	const float dx   = wrap(p.x - params.avatar_x);
	const float dy   = wrap(p.y - params.avatar_y);
	const float past = std::sqrt(dx * dx + dy * dy) - params.fog_clear;
	if (past <= 0.0f) {
		return 0.0f;
	}
	const float d = 1.0f - std::exp(-past / std::max(params.fog_distance, 0.01f));
	const float h = std::exp(-std::max(p.z, 0.0f) / std::max(params.fog_height, 0.01f));
	return std::clamp(open * params.fog_density * params.fog_mix * d * h, 0.0f, 7.0f / 8.0f);
}

// Lightning: flash tables of 8 rows of 256 (flash level 0..7), the
// flash's elevation, and the lift its shadow traces end at (the top of
// the occlusion grid).
constexpr int   shaded_flash_rows       = 8;
constexpr int   shaded_flash_table_size = shaded_flash_rows * 256;
constexpr float shaded_flash_elevation  = 50.0f;    // Degrees.
constexpr float shaded_flash_top        = static_cast<float>(shaded_grid_lifts);
constexpr int   shaded_flash_dump_first = 100;    // Number of the first flash dump.

/*
 *  The light level of a rain drop or snowflake pixel: the surface level
 *  beneath it (surface) under cover, the light scattered at its screen
 *  position (scattered: the sky and the lamps, no facing, no shadows) in
 *  the open, and between them as open (0..1) as the surface is.
 */
inline float Shaded_particle_level(float surface, float scattered, float open) {
	return surface + (scattered - surface) * open;
}

namespace Shaded_cpu {
	// The sky map of a grid volume (size_x x size_y): 255 for columns open
	// to the sky, and under cover 255 * (1 - d / fade_tiles) (at least 0)
	// with d the chamfer distance in tiles to the nearest open column,
	// reached only through columns clear at lift 1 (walls and closed doors
	// stop it).
	void build_sky_open(const Shaded_grid_volume& vol, float fade_tiles, std::vector<uint8_t>& open);
	// The soft coverage mask of an RLE frame (xleft, yabove, w x h): 255
	// for every painted pixel, padded by blur and box-blurred twice with
	// radius blur.  Appends the pixels to data and describes them in mask.
	void build_cloud_mask(
			const unsigned char* rle, int xleft, int yabove, int w, int h, int blur, std::vector<uint8_t>& data,
			Shaded_cloud_mask& mask);
	// The fog tables (shaded_fog_table_size bytes) from DAY and the game's
	// FOG palette (raw 6-bit colours): FOG fitted per channel as
	// a * DAY + b by least squares over the relit indices gives the fog's
	// share fog_mix = 1 - mean a (0.05..0.95) and colour b / fog_mix.  Row
	// fk + 8 * fl maps each relit DAY index to the DAY colour nearest (CIELAB,
	// lightness and hue weighted 4) DAY * (1 - fk / 8) + colour * (fl / 7) * (fk / 8); rows with fk = 0,
	// index 0 and the cycling colours are left as they are.
	void build_fog_tables(
			const uint8_t* day768, const uint8_t* fog768, float& fog_mix, float colour[3], uint8_t* table);
	// The grid's transmittance for the flash from p to target (both in
	// world tiles, tiles, lifts): a grid ray with its end cells skipped,
	// as for the lamps.
	float flash_grid_transmittance(const Shaded_grid_volume& vol, const Shaded_point& p, const Shaded_point& target);
	// The flash tables (shaded_flash_table_size bytes) from DAY and the
	// game's LIGHTNING palette (raw 6-bit colours): row k maps each relit
	// DAY index i to the DAY colour nearest (CIELAB, lightness weighted 4)
	// DAY[i] mixed k / 7 of the way to LIGHTNING[i]; row 0, index 0 and
	// the cycling colours are left as they are.
	void build_flash_table(const uint8_t* day768, const uint8_t* lightning768, uint8_t* table);
	// Mark every pixel an RLE frame (a drop or flake) paints at (xoff,
	// yoff), translucent ones too, as 1 in particle_mask, clipped to clip.
	void mark_spans(
			Shaded_scene_buffers& buf, const Shaded_rect& clip, const unsigned char* rle, int xoff, int yoff);
}    // namespace Shaded_cpu

// Synthetic tests over hand-built volumes; prints SHADED-WEATHER: TEST
// lines.  With the game's DAY and FOG palettes (raw 6-bit colours), also
// how well the fitted fog tables reproduce FOG, and with LIGHTNING the
// flash table's identities.  Returns the number of failures.
int Shaded_weather_selftest(
		const uint8_t* day768 = nullptr, const uint8_t* fog768 = nullptr, const uint8_t* lightning768 = nullptr);

#endif
