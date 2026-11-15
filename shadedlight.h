/*
 *  shadedlight.h - Height-aware "shaded light" scene record.
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
 *  Stage 1 of an alternative lighting renderer: while the software painter
 *  draws the world, record for every pixel it writes the height of the
 *  surface, the face it lies on and which object drew it.  Later stages
 *  light the frame from this record.
 *
 *  The buffers are plain arrays with one fixed-size element per pixel of
 *  the 8-bit draw surface (guard band included, same pitch), so they can be
 *  uploaded as textures unchanged.  Every step is a function over those
 *  buffers behind a backend switch; only the CPU backend exists so far.
 *  The tunables live in Shaded_light_params, laid out so that it can be
 *  passed to a shader as a uniform block (std140) unchanged.
 */

#ifndef SHADEDLIGHT_H
#define SHADEDLIGHT_H 1

#include "shapeid.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

class Game_object;
class Image_buffer8;
class Image_window8;
class Shape_frame;
struct Shaded_weather_input;
struct Shaded_weather_set;
struct Shaded_weather_out;

// Facing values: the low 3 bits of each facing pixel.
constexpr uint8_t shaded_face_none    = 0;
constexpr uint8_t shaded_face_ground  = 1;    // Flats, terrain, 0-height objects.
constexpr uint8_t shaded_face_top     = 2;
constexpr uint8_t shaded_face_south   = 3;    // Faces +y.
constexpr uint8_t shaded_face_east    = 4;    // Faces +x.
constexpr uint8_t shaded_face_rounded = 5;    // Rounded volume: the normal is in normal_map.
constexpr uint8_t shaded_face_mask    = 0x07;
// Set when the pixel lay outside the object's projected 3D box and its face
// and height were clamped onto the box.
constexpr uint8_t shaded_face_clamped = 0x08;
// Set on a roof seam pixel merged into the slab top beside it
// (Shaded_cpu::merge_roof_seams): it lies on that plane, not in its box.
constexpr uint8_t shaded_face_merged = 0x10;

// Height units per lift in the height buffer (one screen pixel = 1/4 lift).
constexpr int shaded_height_units_per_lift = 16;
// Object ids: 0 = no object, 1..65534 = 1 + index into the object table.
constexpr uint16_t shaded_id_none     = 0;
constexpr uint16_t shaded_id_overflow = 0xffff;

enum class Shaded_view : int32_t {
	normal = 0,
	height,
	facing,
	object_id,
	light,     // Light level as a grey ramp.
	unlit,     // The frame without the light pass.
	shadow,    // Shadowed share of the light as a grey ramp.
	normals,    // Surface normal as false colour (x, y, z to red, green, blue).
	weather,    // Sky factor (grey, black under cover), fog (cyan), flash (white).
	count
};

// Light levels: 0 = black .. shaded_light_levels - 1 = full light.
constexpr int shaded_light_levels = 32;

enum class Shaded_backend : int32_t {
	cpu = 0,
	gpu = 1    // Not built yet (needs the SDL GPU device); falls back to CPU.
};

// How an object's pixels get their geometry: the object's 3D box, or a
// rounded volume fitted to the box and the art's silhouette.  The values
// are the kind column of the volumes section of shaded_light.txt.
enum class Shaded_volume : uint8_t {
	box     = 0,    // The 3D box (Shape_info dimensions).
	figure  = 1,    // Vertical capsule: people, monsters.
	round   = 2,    // Ellipsoid: bushes, rocks.
	upright = 3,    // Cylinder with a rounded top: barrels, pillars.
	tree    = 4     // Trunk capsule under a crown ellipsoid.
};

constexpr int shaded_volume_kinds = 5;

/*
 *  All tunables, in vec4-sized groups of plain 32-bit values (std140).
 */
struct alignas(16) Shaded_light_params {
	int32_t enabled      = 0;
	int32_t view         = 0;    // Shaded_view.
	int32_t backend      = 0;    // Shaded_backend.
	int32_t object_count = 0;
	// Buffer geometry: element (x, y) of the game area is at
	//   buffer_origin + y * buffer_stride + x.
	int32_t buffer_width  = 0;
	int32_t buffer_height = 0;
	int32_t buffer_stride = 0;
	int32_t buffer_origin = 0;
	// Scroll position of the recorded frame, to rebuild world positions.
	int32_t scroll_tx = 0;    // Top-left tile.
	int32_t scroll_ty = 0;
	int32_t scroll_px = 0;    // Extra pixel offset (smooth scrolling).
	int32_t scroll_py = 0;
	float   pixels_per_tile       = 8.0f;
	float   pixels_per_lift       = 4.0f;
	float   height_units_per_lift = static_cast<float>(shaded_height_units_per_lift);
	float   debug_height_range    = 16.0f;    // Lifts covered by the height ramp.
	// Global sliders (config/video/shaded_light/...), as fractions.
	float ambient      = 1.0f;
	float brightness   = 1.0f;
	float radius_scale = 1.0f;
	float softness     = 1.0f;
	// Light pass.
	int32_t light_count = 0;
	int32_t level_count = shaded_light_levels;
	int32_t dither      = 1;    // 4x4 ordered dither on the level.
	int32_t lit         = 0;    // Lighting active (set by the palette step).
	float   ambient_level  = 0.3f;    // Level of unlit surfaces, 0..1.
	float   facing_min     = 0.5f;    // Facing term for light at a grazing angle.
	float   tiles_per_lift = 0.5f;    // Lift height in tiles, for 3D distance.
	float   area_height    = 6.0f;    // Lifts above an area light it reaches.
	// Screen-space light bins over the game area.
	int32_t bin_size = 16;    // Pixels per side.
	int32_t bins_x   = 0;
	int32_t bins_y   = 0;
	int32_t threads  = 0;    // Threads of the light pass (0 = auto until set up).
	// Traced shadows.
	int32_t shadows           = 1;     // On.
	int32_t shadow_resolution = 2;     // Pixels per side of a screen map cell: 1, 2 or 4.
	int32_t shadow_lights     = 4;     // Lights traced per map cell, 1..16.
	int32_t shadow_steps      = 32;    // Most samples of a screen-space trace.
	float   shadow_strength   = 1.0f;     // 0 = no shadows, 1 = full.
	float   shadow_softness   = 1.0f;     // 1 = light size 0.5 tile, 0 = hard edges.
	float   shadow_bias       = 0.25f;    // Tiles skipped at the receiver.
	float   shadow_reserved   = 0.0f;
	// Rounded volumes.  rounded = 0 gives every object its box, exactly as
	// before they were added; roof_seams only applies while rounded is on.
	int32_t rounded          = 1;
	int32_t roof_seams       = 1;
	float   rounded_profile  = 1.0f;    // Reserved: bulge profile scale.
	float   rounded_reserved = 0.0f;
	// Weather (config/video/shaded_light/weather/...).  weather = 0 lights
	// exactly as before it was added; weather_active is set per paint when
	// the light pass has weather terms to apply.
	int32_t weather           = 1;
	int32_t lightning_shadows = 1;
	int32_t precipitation     = 1;
	int32_t weather_active    = 0;
	float   cloud_shadows     = 0.4f;    // Share of the sky light a full cloud takes.
	float   cloud_softness    = 6.0f;    // Blur radius of the cloud patches, pixels.
	float   overcast_soften   = 0.5f;    // Overcast weakens and softens shadows by this.
	float   overcast          = 0.0f;    // 1 while the clock shows the overcast palette.
	float   fog_density       = 1.0f;
	float   fog_height        = 4.0f;    // Lifts.
	float   fog_distance      = 6.0f;    // Tiles.
	float   fog_clear         = 2.0f;    // Tiles round the Avatar left clear.
	float   fog_fade          = 3.0f;    // Tiles over which sky terms fade in at doorways.
	float   fog_scatter       = 1.0f;
	float   fog_mix           = 0.0f;
	// Night tint: 0 = the game's own NIGHT colours, 0.5 = neutral grey, 1 = cool
	// blue.  Only the unlit (ambient) colours change; lamp and fire light stays.
	float   night_tint        = 0.0f;
	int32_t fog               = 0;
	float   avatar_x = 0.0f, avatar_y = 0.0f, avatar_z = 0.0f;
	int32_t cloud_count = 0;
	float   flash   = 0.0f;
	float   flash_x = 0.0f, flash_y = 0.0f, flash_z = 0.0f;
};

static_assert(sizeof(Shaded_light_params) % 16 == 0);
static_assert(offsetof(Shaded_light_params, weather) % 16 == 0);
static_assert(std::is_trivially_copyable_v<Shaded_light_params> && std::is_standard_layout_v<Shaded_light_params>);

/*
 *  One painted object, as uploaded to a storage buffer later.
 */
struct Shaded_object_record {
	int32_t tx, ty, tz;                // Tile (SE bottom corner of the box).
	int32_t xtiles, ytiles, ztiles;    // 3D box (ztiles in lifts).
	int32_t shape, frame;
};

static_assert(sizeof(Shaded_object_record) == 32);

/*
 *  The rounded volume fitted to one painted object, for the shadow trace;
 *  one per object record (std430).  kind 0 (box): the record's box is used
 *  and the rest is 0.  Otherwise an ellipsoid at centre with radius (the
 *  crown for a tree), and for a tree also a vertical trunk capsule of
 *  trunk_radius under the same x, y, from the ground up to trunk_top.
 */
struct Shaded_object_volume {
	float   centre[3]    = {0.0f, 0.0f, 0.0f};    // World tiles, tiles, lifts.
	float   radius[3]    = {0.0f, 0.0f, 0.0f};    // Tiles (z radius in tiles too).
	float   trunk_radius = 0.0f;                  // Tiles; 0 = none.
	float   trunk_top    = 0.0f;                  // Lifts.
	int32_t kind         = 0;                     // Shaded_volume.
	int32_t pad[3]       = {0, 0, 0};
};

static_assert(sizeof(Shaded_object_volume) == 48);
static_assert(std::is_trivially_copyable_v<Shaded_object_volume> && std::is_standard_layout_v<Shaded_object_volume>);

// Object flags (Shaded_scene_buffers::object_flags).
constexpr uint8_t shaded_object_rounded = 1;    // Has a rounded volume.
constexpr uint8_t shaded_object_roof    = 2;    // A slab: occludes, a grid roof shape, or one lift high.

// Light flags (Shaded_light_record::flags).  Area and off are also the
// flags column of shaded_light.txt.
constexpr int32_t shaded_light_area    = 1;    // Even light, horizontal distance.
constexpr int32_t shaded_light_off     = 2;    // Gives no shaded light.
constexpr int32_t shaded_light_carried = 4;    // Readied by a party member.
// Reserved for the sun and moon (never set yet): x, y and z then hold the
// direction towards the light, in tiles, tiles and lifts per unit.
constexpr int32_t shaded_light_directional = 8;
// Most lights in one paint.
constexpr int shaded_max_lights = 256;

/*
 *  One light of the current paint, as uploaded to a storage buffer later
 *  (std430).  The sliders are not applied: radius and intensity are the
 *  table row's.
 */
struct Shaded_light_record {
	float   x, y, z;              // World position: tiles, tiles, lifts.
	float   radius;               // In tiles.
	float   red, green, blue;     // 0..1.
	float   intensity;            // Row brightness / 100 (1 = a torch).
	int32_t flags;                // shaded_light_*.
	int32_t object_id;            // Record id of the self-lit object (0 = none).
	int32_t shape, frame;         // Of the light object.
};

static_assert(sizeof(Shaded_light_record) == 48);
static_assert(std::is_trivially_copyable_v<Shaded_light_record> && std::is_standard_layout_v<Shaded_light_record>);

/*
 *  A world point: tiles, tiles, lifts.
 */
struct Shaded_point {
	float x, y, z;
};

// Shortest way round the wrapped world for a difference of two positions
// in [0, c_num_tiles) plus a screen or two.
constexpr float Shaded_wrap_delta(float d) {
	const auto tiles = static_cast<float>(c_num_tiles);
	if (d > tiles * 0.5f) {
		return d - tiles;
	}
	if (d < -tiles * 0.5f) {
		return d + tiles;
	}
	return d;
}

/*
 *  Where the shadow traces from p end: a point light's position, in p's
 *  frame of the wrapped world; for a directional light (sun, moon) a point
 *  64 units along its direction, to be gated by Shaded_grid open_to_sky.
 */
constexpr Shaded_point Shaded_shadow_target(const Shaded_light_record& l, const Shaded_point& p) {
	switch (l.flags & shaded_light_directional) {
	case shaded_light_directional:
		return Shaded_point{p.x + l.x * 64.0f, p.y + l.y * 64.0f, p.z + l.z * 64.0f};
	default:
		return Shaded_point{p.x + Shaded_wrap_delta(l.x - p.x), p.y + Shaded_wrap_delta(l.y - p.y), l.z};
	}
}

/*
 *  One row of shaded_light.txt.
 */
struct Shaded_light_entry {
	int radius;        // Tiles.
	int brightness;    // Percent; 100 = a torch.
	int red, green, blue;    // 0-255.
	int height;        // Lifts above the base; -1 = top of the 3D box.
	int flags;         // shaded_light_area, shaded_light_off.
};

/*
 *  The row a light without its own row gets, from its light_data
 *  brightness b (candle 1, torch 5, lamp 7).
 */
constexpr Shaded_light_entry Shaded_default_light(int b) {
	return Shaded_light_entry{3 + b, 50 + 10 * b, 255, 214, 170, -1, 0};
}

// Where a light's row came from.
enum class Shaded_light_row : uint8_t {
	exact,       // Table row for the shape and frame.
	wildcard,    // Table row for the shape, frame -1.
	formula      // No row: Shaded_default_light.
};

/*
 *  A rectangle in game-area pixels.
 */
struct Shaded_rect {
	int x = 0, y = 0, w = 0, h = 0;
};

/*
 *  The scene record.
 */
struct Shaded_scene_buffers {
	int                               width  = 0;    // Of the draw surface.
	int                               height = 0;
	int                               stride = 0;    // Elements per row.
	int                               origin = 0;    // Index of game pixel (0, 0).
	std::vector<uint16_t>             height_map;    // 1/16 lift above lift 0.
	std::vector<uint8_t>              facing_map;    // shaded_face_* (+ clamped, merged bits).
	std::vector<uint16_t>             object_ids;    // shaded_id_*.
	std::vector<uint8_t>              light_levels;    // 0..level_count - 1.
	std::vector<uint8_t>              shadow_levels;    // 255 * shadowed / unshadowed light.
	// Weather outputs and inputs, GPU format R8: sky factor (0 = covered,
	// else 1 + 254 * factor), fog code, flash level, and 1 where a rain
	// or snow drop was painted.
	std::vector<uint8_t>              sky_levels;
	std::vector<uint8_t>              fog_codes;
	std::vector<uint8_t>              flash_levels;
	std::vector<uint8_t>              particle_mask;
	// Octahedral unit normal (Shaded_encode_normal), where facing is
	// shaded_face_rounded; 0 elsewhere.  GPU format RG8_SNORM.
	std::vector<uint16_t>             normal_map;
	std::vector<Shaded_object_record> objects;
	std::vector<uint8_t>              object_opacity;    // Per object: screen-space shadow opacity.
	std::vector<Shaded_object_volume> object_volumes;    // Per object: fitted rounded volume.
	std::vector<uint8_t>              object_flags;      // Per object: shaded_object_*.
	// Light bins: the lights reaching bin b are
	// bin_lights[bin_offsets[b] .. bin_offsets[b + 1]).
	std::vector<uint32_t> bin_offsets;
	std::vector<uint16_t> bin_lights;
	// Game-area rectangle the buffers cover.
	Shaded_rect bounds() const;

	int index(int x, int y) const {
		return origin + y * stride + x;
	}
};

/*
 *  Where a pixel lands on an object's box.
 */
struct Shaded_box_hit {
	uint8_t facing;    // Face, maybe with shaded_face_clamped.
	int     dz_px;     // Height above the box bottom, in pixels (0..hpx).
};

/*
 *  Classify a pixel against a box projected by Exult's view: one tile is
 *  8 px, one lift moves 4 px up and 4 px left.  (u, v) is the pixel minus
 *  the box's origin pixel (its SE bottom corner, from get_shape_location),
 *  so the box footprint is u in (-xpx, 0], v in (-ypx, 0].  xpx, ypx and
 *  hpx are the box size in pixels (xtiles * 8, ytiles * 8, ztiles * 4).
 *  The view ray moves +1 px in world x, y and height per pixel of depth, so
 *  the top is met first, then the east face (u >= v) or the south face.
 *  Mirrored (bit 5) frames need nothing extra: Shape_info already swaps
 *  their x and y sizes, which swaps the faces.
 */
constexpr Shaded_box_hit Shaded_classify_box(int u, int v, int xpx, int ypx, int hpx) {
	auto in_range = [](int val, int size) {
		return val > -size && val <= 0;
	};
	if (hpx <= 0) {    // Flat: everything is ground.
		const bool inside = in_range(u, xpx) && in_range(v, ypx);
		return {static_cast<uint8_t>(shaded_face_ground | (inside ? 0 : shaded_face_clamped)), 0};
	}
	if (in_range(u + hpx, xpx) && in_range(v + hpx, ypx)) {
		return {shaded_face_top, hpx};
	}
	const bool    east   = u >= v;
	const uint8_t face   = east ? shaded_face_east : shaded_face_south;
	int           dz     = east ? -u : -v;
	bool          inside = east ? in_range(v - u, ypx) : in_range(u - v, xpx);
	if (dz < 0 || dz > hpx) {
		inside = false;
		dz     = dz < 0 ? 0 : hpx;
	}
	return {static_cast<uint8_t>(face | (inside ? 0 : shaded_face_clamped)), dz};
}

/*
 *  Light falloff over t = d^2 / R^2: 1 at the light, 0 from the radius on.
 */
constexpr float Shaded_falloff(float t) {
	if (t >= 1.0f) {
		return 0.0f;
	}
	const float u = 1.0f - t;
	return u * u;
}

/*
 *  4x4 ordered dither threshold in (0, 1) for a world pixel.
 */
constexpr float Shaded_bayer4(int x, int y) {
	constexpr int order[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
	return (static_cast<float>(order[((y & 3) << 2) | (x & 3)]) + 0.5f) / 16.0f;
}

/*
 *  Surface normal of a face, in world axes (x east, y south, z up).  Tops,
 *  ground and rounded surfaces face up.
 */
struct Shaded_normal {
	int x, y, z;
};

constexpr Shaded_normal Shaded_face_normal(uint8_t facing) {
	switch (facing & shaded_face_mask) {
	case shaded_face_south:
		return Shaded_normal{0, 1, 0};
	case shaded_face_east:
		return Shaded_normal{1, 0, 0};
	default:
		return Shaded_normal{0, 0, 1};
	}
}

// Whether a face can see a light at (dx, dy, dz) from it.
constexpr bool Shaded_faces_light(uint8_t facing, float dx, float dy, float dz) {
	const Shaded_normal n = Shaded_face_normal(facing);
	return static_cast<float>(n.x) * dx + static_cast<float>(n.y) * dy + static_cast<float>(n.z) * dz > 0.0f;
}

// Square root usable in constant expressions (Newton's method there,
// std::sqrt at run time).
constexpr float Shaded_sqrt(float x) {
	if (__builtin_is_constant_evaluated()) {
		if (x <= 0.0f) {
			return 0.0f;
		}
		float r = x > 1.0f ? x : 1.0f;
		for (int i = 0; i < 40; i++) {
			r = 0.5f * (r + x / r);
		}
		return r;
	}
	return std::sqrt(x);
}

/*
 *  Compact unit normals (Shaded_scene_buffers::normal_map): octahedral,
 *  low byte x and high byte y, each a signed byte read as value / 127
 *  (GPU RG8_SNORM).  Decode: n = (x, y, 1 - |x| - |y|); if n.z < 0, then
 *  (x, y) = ((1 - |y|) * sign(x), (1 - |x|) * sign(y)); normalise.
 */
constexpr int8_t Shaded_snorm8(float v) {
	const float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
	const float s = c * 127.0f;
	return static_cast<int8_t>(s < 0.0f ? s - 0.5f : s + 0.5f);
}

constexpr uint16_t Shaded_encode_normal(float x, float y, float z) {
	const float ax  = x < 0.0f ? -x : x;
	const float ay  = y < 0.0f ? -y : y;
	const float az  = z < 0.0f ? -z : z;
	const float sum = ax + ay + az;
	if (sum <= 0.0f) {
		return Shaded_encode_normal(0.0f, 0.0f, 1.0f);
	}
	float px = x / sum;
	float py = y / sum;
	if (z < 0.0f) {
		const float qx = (1.0f - (py < 0.0f ? -py : py)) * (px < 0.0f ? -1.0f : 1.0f);
		const float qy = (1.0f - (px < 0.0f ? -px : px)) * (py < 0.0f ? -1.0f : 1.0f);
		px             = qx;
		py             = qy;
	}
	const auto lo = static_cast<uint8_t>(Shaded_snorm8(px));
	const auto hi = static_cast<uint8_t>(Shaded_snorm8(py));
	return static_cast<uint16_t>(lo | (hi << 8));
}

/*
 *  A surface normal in world axes (x east, y south, z up), unit length.
 */
struct Shaded_normalf {
	float x, y, z;
};

constexpr Shaded_normalf Shaded_decode_normal(uint16_t code) {
	const float x  = static_cast<float>(static_cast<int8_t>(code & 0xff)) / 127.0f;
	const float y  = static_cast<float>(static_cast<int8_t>(code >> 8)) / 127.0f;
	const float ax = x < 0.0f ? -x : x;
	const float ay = y < 0.0f ? -y : y;
	float       nx = x;
	float       ny = y;
	const float nz = 1.0f - ax - ay;
	if (nz < 0.0f) {
		nx = (1.0f - ay) * (x < 0.0f ? -1.0f : 1.0f);
		ny = (1.0f - ax) * (y < 0.0f ? -1.0f : 1.0f);
	}
	const float len = Shaded_sqrt(nx * nx + ny * ny + nz * nz);
	if (len <= 0.0f) {
		return Shaded_normalf{0.0f, 0.0f, 1.0f};
	}
	return Shaded_normalf{nx / len, ny / len, nz / len};
}

/*
 *  Signed distance in tiles (negative inside) from q to a fitted rounded
 *  volume, q relative to v.centre with its z already in tiles: iq's
 *  ellipsoid bound k0 * (k0 - 1) / k1, and for a tree the nearer of that
 *  crown and the trunk capsule.
 */
constexpr float Shaded_rounded_sd(const float q[3], const Shaded_object_volume& v, float tiles_per_lift) {
	float k0 = 0.0f;
	float k1 = 0.0f;
	float rmin = 1e30f;
	for (int i = 0; i < 3; i++) {
		const float r = v.radius[i] > 1.0f / 64.0f ? v.radius[i] : 1.0f / 64.0f;
		k0 += (q[i] / r) * (q[i] / r);
		k1 += (q[i] / (r * r)) * (q[i] / (r * r));
		rmin = r < rmin ? r : rmin;
	}
	k0             = Shaded_sqrt(k0);
	k1             = Shaded_sqrt(k1);
	float sd       = k1 > 1e-9f ? k0 * (k0 - 1.0f) / k1 : -rmin;
	if (v.kind == static_cast<int32_t>(Shaded_volume::tree) && v.trunk_radius > 0.0f) {
		// Trunk: the vertical axis through the centre, up to trunk_top.
		const float top   = (v.trunk_top - v.centre[2]) * tiles_per_lift;
		const float above = q[2] > top ? q[2] - top : 0.0f;
		const float trunk = Shaded_sqrt(q[0] * q[0] + q[1] * q[1] + above * above) - v.trunk_radius;
		sd                = trunk < sd ? trunk : sd;
	}
	return sd;
}

// The normal the lighting uses at buffer index idx with face face (the
// facing value masked): the stored normal for rounded pixels, else the
// face's (0/1 valued, so box faces light exactly as with integers).
inline Shaded_normalf Shaded_pixel_normal(const Shaded_scene_buffers& buf, int idx, uint8_t face) {
	if (face == shaded_face_rounded) {
		return Shaded_decode_normal(buf.normal_map[idx]);
	}
	const Shaded_normal n = Shaded_face_normal(face);
	return Shaded_normalf{static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)};
}

/*
 *  Colour ramp of the remap tables: the palettes it passes through, each at
 *  its light level (0 = black, 1 = DAY).
 */
struct Shaded_ramp {
	static constexpr int black   = -1;    // Palette code of the black stop.
	static constexpr int ambient = -2;    // Palette code of the ambient stop.
	int                  count   = 0;
	int                  palette[6]{};    // palettes.flx number, black or ambient.
	float                level[6]{};
};

// Remap tables: shaded_light_levels rows of 256 palette indices.
constexpr int shaded_remap_size = shaded_light_levels * 256;
// Last palette index the remap relights; 0xe0 on are the cycling colours.
constexpr int shaded_last_lit_index = 0xdf;
// Night tint weight: full at an ambient luminance (relative to DAY) of
// night_y or below, none at dusk_y or above.
constexpr float shaded_night_tint_night_y = 0.50f;
constexpr float shaded_night_tint_dusk_y  = 0.72f;
// The cycling ranges flames, embers and lava are drawn with (gamewin.cc
// rotates 0xe8-0xef, 0xf8-0xfb and 0xfc-0xfe).  The night palettes turn
// them green, and Exult only ever showed them through a light palette at
// night, since a flame in view is a light.  B' takes them from the first
// light stop above ambient.  The water and magic ranges (0xe0-0xe7,
// 0xf0-0xf7) keep the ambient colours.
constexpr bool Shaded_glow_index(int i) {
	return (i >= 0xe8 && i <= 0xef) || (i >= 0xf8 && i <= 0xfe);
}
// Reference palettes of the ramp, in Shaded_cpu::build_remap's refs order.
constexpr int shaded_ref_palettes[4] = {0, 7, 11, 12};    // DAY, CANDLE, SINGLE_LIGHT, MANY_LIGHTS.

/*
 *  Relative luminance of a 6-bit colour, times 1000.
 */
constexpr int Shaded_luminance(int r, int g, int b) {
	return 299 * r + 587 * g + 114 * b;
}

/*
 *  What the per-pixel steps need to know about the object being painted.
 */
struct Shaded_object_context {
	int           ox = 0, oy = 0;    // Origin pixel of the box.
	int           xpx = 0, ypx = 0, hpx = 0;
	int           base_height = 0;    // Box bottom, in height units.
	uint16_t      id          = shaded_id_none;
	Shaded_volume volume      = Shaded_volume::box;
	// Rounded volumes: the vertical axis through the footprint centre, at
	// the box bottom, in pixels from (ox, oy).
	float ax_px = 0.0f, ay_px = 0.0f;
};

/*
 *  Where a rounded volume's pixels lie, in world pixels from the axis at
 *  the box bottom (a east, b south, z up), for fitting its shadow volume.
 *  Body is everything but a tree's crown.
 */
struct Shaded_rounded_extent {
	struct Range {
		float lo[3] = {1e30f, 1e30f, 1e30f};
		float hi[3] = {-1e30f, -1e30f, -1e30f};

		bool empty() const {
			return lo[0] > hi[0];
		}

		void add(float a, float b, float z) {
			const float p[3] = {a, b, z};
			for (int i = 0; i < 3; i++) {
				lo[i] = p[i] < lo[i] ? p[i] : lo[i];
				hi[i] = p[i] > hi[i] ? p[i] : hi[i];
			}
		}

		void add(const Range& r, float da, float db) {
			if (!r.empty()) {
				add(r.lo[0] + da, r.lo[1] + db, r.lo[2]);
				add(r.hi[0] + da, r.hi[1] + db, r.hi[2]);
			}
		}
	};

	Range body;
	Range crown;
	float trunk_radius = 0.0f;    // Pixels; trees.
};

/*
 *  A rounded volume's geometry for one frame, as offsets from the art's
 *  top-left pixel (mask pixel (x, y) is screen pixel (xoff - xleft + x,
 *  yoff - yabove + y)): per opaque pixel its height above the box bottom
 *  (height units) and its normal; z_units 0xffff = not part of the art.
 */
struct Shaded_silhouette {
	int                   xleft = 0, yabove = 0, w = 0, h = 0;
	std::vector<uint16_t> z_units;
	std::vector<uint16_t> normal;
	Shaded_rounded_extent extent;    // Relative to the axis of the shape's own origin.
};

/*
 *  Game pixel coordinate (x, or y) of a world x (or y) at z lifts, and
 *  back: one tile is ppt pixels, one lift moves ppl pixels up and left.
 *  Unproject is Shaded_light::world_position; project is its inverse.
 */
constexpr float Shaded_project(float w, float z, int scroll_tile, int scroll_px, float ppt, float ppl) {
	return (w - static_cast<float>(scroll_tile)) * ppt - static_cast<float>(scroll_px) - z * ppl - 0.5f;
}

constexpr float Shaded_unproject(int pixel, float z, int scroll_tile, int scroll_px, float ppt, float ppl) {
	return static_cast<float>(scroll_tile) + (static_cast<float>(pixel + scroll_px) + 0.5f + z * ppl) / ppt;
}

/*
 *  Signed distance from a point to a box, in two parts: the squared
 *  distance outside it (0 inside) and, inside, minus the distance to the
 *  nearest face (0 outside).  The distance is sqrt(outside2) or inside.
 */
struct Shaded_box_distance {
	float outside2;
	float inside;
};

constexpr Shaded_box_distance Shaded_box_sd(const float q[3], const float lo[3], const float hi[3]) {
	float outside2 = 0.0f;
	float inside   = -1e30f;
	for (int i = 0; i < 3; i++) {
		const float below = lo[i] - q[i];
		const float above = q[i] - hi[i];
		const float d     = below > above ? below : above;
		if (d > 0.0f) {
			outside2 += d * d;
		}
		inside = d > inside ? d : inside;
	}
	return Shaded_box_distance{outside2, outside2 > 0.0f ? 0.0f : inside};
}

/*
 *  Share of a soft light seen past an edge sd tiles away (negative:
 *  inside the occluder) by a cone cone tiles wide there.
 */
constexpr float Shaded_cone_visibility(float sd, float cone, bool hard) {
	if (hard) {
		return sd > 0.0f ? 1.0f : 0.0f;
	}
	const float c = cone > 1.0f / 16.0f ? cone : 1.0f / 16.0f;
	const float v = 0.5f + sd / (2.0f * c);
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/*
 *  The 3D box of a light's own object (world tiles, tiles, lifts), which
 *  must not shadow its light in the grid.  valid = 0: none (carried).
 */
struct Shaded_light_box {
	float   lo[3] = {0.0f, 0.0f, 0.0f};
	float   hi[3] = {0.0f, 0.0f, 0.0f};
	int32_t valid = 0;
	int32_t pad   = 0;
};

/*
 *  End of a grid ray from p towards t that stops just before the ray
 *  enters box [lo, hi] (in p's frame): margin past the entry face, along
 *  the entry axis.  t when the ray misses the box; p when p is inside.
 */
constexpr Shaded_point Shaded_clip_to_box(
		const Shaded_point& p, const Shaded_point& t, const float lo[3], const float hi[3], float margin) {
	const float a[3]   = {p.x, p.y, p.z};
	const float d[3]   = {t.x - p.x, t.y - p.y, t.z - p.z};
	float       enter  = 0.0f;
	float       leave  = 1.0f;
	int         axis   = -1;
	for (int i = 0; i < 3; i++) {
		if (d[i] == 0.0f) {
			if (a[i] < lo[i] || a[i] > hi[i]) {
				return t;
			}
			continue;
		}
		float t0 = (lo[i] - a[i]) / d[i];
		float t1 = (hi[i] - a[i]) / d[i];
		if (t0 > t1) {
			const float s = t0;
			t0            = t1;
			t1            = s;
		}
		if (t0 > enter) {
			enter = t0;
			axis  = i;
		}
		leave = t1 < leave ? t1 : leave;
	}
	if (enter > leave) {
		return t;    // Misses the box.
	}
	if (axis < 0) {
		return p;    // Starts inside.
	}
	const float back = margin / (d[axis] < 0.0f ? -d[axis] : d[axis]);
	const float s    = enter - back > 0.0f ? enter - back : 0.0f;
	return Shaded_point{p.x + d[0] * s, p.y + d[1] * s, p.z + d[2] * s};
}

// Grid field values: transmittance * shaded_field_scale, or solid.
constexpr uint8_t shaded_field_scale = 254;
constexpr uint8_t shaded_field_solid = 255;

/*
 *  Trilinear blend of 8 grid field values (index bit 0 = x + 1, bit 1 =
 *  y + 1, bit 2 = z + 1) at (fx, fy, fz) in [0, 1], leaving solid cells
 *  out and renormalising: light does not leak along the inside of walls.
 *  All solid gives 1.
 */
constexpr float Shaded_solid_trilinear(const uint8_t v[8], float fx, float fy, float fz) {
	float wsum = 0.0f;
	float vsum = 0.0f;
	for (int i = 0; i < 8; i++) {
		if (v[i] == shaded_field_solid) {
			continue;
		}
		const float w = ((i & 1) ? fx : 1.0f - fx) * ((i & 2) ? fy : 1.0f - fy) * ((i & 4) ? fz : 1.0f - fz);
		wsum += w;
		vsum += w * static_cast<float>(v[i]);
	}
	if (wsum <= 1e-6f) {
		return 1.0f;
	}
	return vsum / (wsum * static_cast<float>(shaded_field_scale));
}

/*
 *  The traced shadows of one paint, as plain data.  Screen maps hold the
 *  screen-space transmittance Ts (times the grid's when traced by rays)
 *  of one light at one receiver per resolution x resolution block: cell
 *  (cx, cy) is game pixel (cx * res + res / 2, cy * res + res / 2), value
 *  Ts * 255.  Grid fields hold the grid transmittance Tg at the centres
 *  of the grid cells around one light (shaded_field_*), index
 *  (z * h + y) * w + x from grid-local cell (x, y, z) of the box.
 */
struct Shaded_shadow_map {
	int32_t  x = 0, y = 0, w = 0, h = 0;    // Cells.
	int32_t  light  = 0;
	int32_t  build  = 0;    // Traced this paint (else kept from the cache).
	uint32_t offset = 0;    // Into map_data.
	int32_t  pad    = 0;
};

struct Shaded_shadow_field {
	int32_t  x = 0, y = 0, z = 0, w = 0, h = 0, d = 0;    // Grid-local cells.
	int32_t  light  = 0;
	int32_t  build  = 0;
	uint32_t offset = 0;    // Into field_data.
	int32_t  pad[3] = {0, 0, 0};
};

struct Shaded_shadow_set {
	int32_t resolution = 2;
	int32_t area_w = 0, area_h = 0;      // Game area.
	int32_t grid_tx = 0, grid_ty = 0;    // World tile of grid cell (0, 0).
	int32_t screen   = 1;    // Maps hold Ts.
	int32_t ray_grid = 0;    // Maps also hold Tg, traced per cell.
	float   strength = 1.0f;
	// Per light: its own object's box, and index into maps or fields, -1
	// for none (T = 1).
	std::vector<Shaded_light_box>    light_boxes;
	std::vector<int32_t>             light_map;
	std::vector<int32_t>             light_field;
	std::vector<Shaded_shadow_map>   maps;
	std::vector<Shaded_shadow_field> fields;
	std::vector<uint8_t>             map_data;
	std::vector<uint8_t>             field_data;
};

struct Shaded_grid_volume;

/*
 *  CPU backend: each step is a plain function over the buffers.  Rectangles
 *  must already be clipped to the buffers' bounds.
 */
namespace Shaded_cpu {
	// Reset a rectangle to facing none, height 0, id 0.
	void clear_rect(Shaded_scene_buffers& buf, const Shaded_rect& r);
	// Fill a rectangle with one flat plane.
	void fill_plane(Shaded_scene_buffers& buf, const Shaded_rect& r, uint16_t height, uint8_t facing, uint16_t id);
	// Record the pixels an RLE shape paints at (xoff, yoff), clipped to clip.
	// Pixel values in [skip_from, 0xfe] are skipped (translucent).
	void record_spans(
			Shaded_scene_buffers& buf, const Shaded_rect& clip, const Shaded_object_context& obj, const unsigned char* rle,
			int xoff, int yoff, int skip_from);
	// The rounded geometry of an RLE frame (xleft, yabove, w x h) painted
	// for a kind != box volume of an xpx x ypx box whose axis is at (axu,
	// axv) pixels from the shape's origin: its silhouette (pixels in
	// [skip_from, 0xfe] left out) is given a bulge from a 3-4 chamfer
	// distance transform, which gives each pixel a depth towards the viewer
	// and so a height and a normal (rounded-report.md).
	void build_silhouette(
			const unsigned char* rle, int xleft, int yabove, int w, int h, int skip_from, Shaded_volume kind, int xpx,
			int ypx, float axu, float axv, Shaded_silhouette& out);
	// Record an RLE shape as record_spans does, with the heights and
	// normals of its silhouette (facing shaded_face_rounded).  Adds the
	// pixels written to written.
	void record_rounded(
			Shaded_scene_buffers& buf, const Shaded_rect& clip, const Shaded_object_context& obj,
			const Shaded_silhouette& sil, const unsigned char* rle, int xoff, int yoff, int skip_from, long& written);
	// Merge the thin south and east strips between stacked roof slabs
	// whose tops meet at one height into those tops (facing top, the slab's
	// top height), over r; reads outside r too.  Adds the pixels changed.
	void merge_roof_seams(Shaded_scene_buffers& buf, const Shaded_rect& r, long& merged);
	// Overwrite a rectangle of an 8-bit frame (indexed like the buffers) with
	// the debug view; lut maps a false colour code to a palette index.
	void resolve_view(
			const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_rect& r, const uint8_t* lut,
			unsigned char* frame);
	// False colour code (index into the lut) for one pixel of a view.
	int view_code(const Shaded_scene_buffers& buf, const Shaded_light_params& params, int index);
	// Game pixels a light's reach may cover, unclipped and inclusive (x0 to
	// x1, y0 to y1); false if it reaches nothing.
	bool light_screen_span(
			const Shaded_light_params& params, const Shaded_light_record& light, float& x0, float& y0, float& x1,
			float& y1);
	// The same as a rectangle clipped to a w x h game area (maybe empty).
	Shaded_rect light_screen_rect(const Shaded_light_params& params, const Shaded_light_record& light, int w, int h);
	// Sort the lights into params.bins_x x params.bins_y screen bins, each
	// light into every bin its reach may cover.
	void build_bins(
			const Shaded_light_params& params, const std::vector<Shaded_light_record>& lights,
			std::vector<uint32_t>& offsets, std::vector<uint16_t>& list);
	// Grid transmittance from p to a light whose own object is box: the
	// ray stops before that box, and is blocked if the cell it stops in is
	// solid (the end cells are otherwise skipped).
	float grid_transmittance(
			const Shaded_grid_volume& vol, const Shaded_light_record& light, const Shaded_light_box& box,
			const Shaded_point& p);
	// Rows [y0, y1) of a grid field: the grid's transmittance from each
	// cell centre to the light into data + field.offset.
	void grid_field_rows(
			const Shaded_grid_volume& vol, const Shaded_light_params& params, const Shaded_light_record& light,
			const Shaded_light_box& box, const Shaded_shadow_field& field, int y0, int y1, uint8_t* data);
	// Cell rows [y0, y1) of the screen maps being built, over the cells
	// rectangle: per cell the lights are ranked by unshadowed light and the
	// top params.shadow_lights traced in screen space (others keep 1).
	// With ray_grid, every light's map also gets the grid's transmittance.
	// Adds the number of screen traces to traces.
	void screen_shadow_rows(
			const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_light_record* lights,
			const uint32_t* bin_offsets, const uint16_t* bin_lights, const Shaded_grid_volume* ray_grid,
			Shaded_shadow_set& set, const Shaded_rect& cells, int y0, int y1, long& traces);
	// Cell rows [y0, y1) of the lightning flash's screen map (set.maps[0],
	// over the cells rectangle): the transmittance from each recorded
	// receiver along the flash's direction (flash.x, y, z, as a
	// directional light) up to the top of the grid, in screen space times,
	// with vol, the grid's.  Unrecorded receivers get 255.  Adds the number
	// of screen traces to traces.
	void flash_shadow_rows(
			const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_grid_volume* vol,
			const Shaded_light_record& flash, Shaded_shadow_set& set, const Shaded_rect& cells, int y0, int y1,
			long& traces);
	// Rows [y0, y1) (of the map) of a depth-aware 3x3 blur of a screen map
	// into out + map.offset.
	void blur_shadow_rows(
			const Shaded_scene_buffers& buf, const Shaded_shadow_set& set, const Shaded_shadow_map& map, int y0, int y1,
			uint8_t* out);
	// Ts at a game pixel of the given height (height units): bilinear over
	// the 4 nearest cells, those of another height weighted down.
	float shadow_screen_at(
			const Shaded_scene_buffers& buf, const Shaded_shadow_set& set, const Shaded_shadow_map& map, int x, int y,
			int height);
	// Tg at a world point (unwrapped is fine) from a grid field.
	float shadow_grid_at(const Shaded_shadow_set& set, const Shaded_shadow_field& field, const Shaded_point& p);
	// Light level of every pixel of rows [y0, y1) of r into levels (indexed
	// like the buffers).  Bins as from build_bins.  With shadows, each
	// light but the self-light is scaled by 1 - strength * (1 - Ts * Tg)
	// and shadow_out (if not null) gets 255 * shadowed / unshadowed light.
	// With weather, the ambient (sky) term of a recorded pixel is dimmed
	// by the clouds over it as far as it is open to the sky, a lightning
	// flash lifts the faces it reaches, and wout's buffers (those not
	// null) get the weather outputs.
	void light_rows(
			const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_light_record* lights,
			const uint32_t* bin_offsets, const uint16_t* bin_lights, const Shaded_rect& r, int y0, int y1,
			uint8_t* levels, const Shaded_shadow_set* shadows = nullptr, uint8_t* shadow_out = nullptr,
			const Shaded_weather_set* weather = nullptr, const Shaded_weather_out* wout = nullptr);
	// Relight rows [y0, y1) of r of an 8-bit frame (indexed like the
	// buffers): frame[i] = remap[levels[i] * 256 + frame[i]].
	void apply_light_rows(
			const Shaded_scene_buffers& buf, const Shaded_rect& r, int y0, int y1, const uint8_t* levels,
			const uint8_t* remap, unsigned char* frame);
	// apply_light_rows, then the flash row of flash_table (8 rows of 256)
	// and the fog row of fog_table (64 rows of 256) for each pixel's codes;
	// a null table (or code 0) leaves the pixel as remapped.
	void apply_weather_rows(
			const Shaded_scene_buffers& buf, const Shaded_rect& r, int y0, int y1, const uint8_t* levels,
			const uint8_t* remap, const uint8_t* fog_codes, const uint8_t* fog_table, const uint8_t* flash_levels,
			const uint8_t* flash_table, unsigned char* frame);
	// Relight rows [y0, y1) of r of an 8-bit frame at one light level,
	// dithered against world pixels as light_rows does for unlit pixels.
	void apply_level_rows(
			const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_rect& r, int y0, int y1,
			float level, const uint8_t* remap, unsigned char* frame);
	// Mean luminance of indices 1..0xdf of a 6-bit palette, relative to day.
	float palette_luminance(const uint8_t* pal768, const uint8_t* day768);
	// Index in [first, last] of the colour of pal768 nearest (r, g, b).
	int nearest_colour(const uint8_t* pal768, int r, int g, int b, int first, int last);
	// CIELAB (D65) of a 6-bit sRGB colour.
	void colour_lab(int r, int g, int b, float lab[3]);
	// Index in [first, last] of the colour nearest lab, by CIELAB distance
	// with lightness weighted 4: pal_lab holds colour_lab of each index.
	int nearest_colour_lab(const float (*pal_lab)[3], const float lab[3], int first, int last);
	// Remap tables (shaded_remap_size bytes) from the ambient palette and the
	// reference palettes (DAY, CANDLE, SINGLE_LIGHT, MANY_LIGHTS), and the
	// palette to display with them.  Sets params.ambient_level; the stops
	// go into ramp if it is not null.
	void build_remap(
			const uint8_t* ambient768, const uint8_t* const refs[4], Shaded_light_params& params, uint8_t* remap,
			uint8_t* displayed768, Shaded_ramp* ramp);
}    // namespace Shaded_cpu

// Which parts of the shadows are traced (EXULT_SHADED_SHADOW_PARTS).
constexpr int shaded_shadow_parts_both   = 0;    // Screen space and grid.
constexpr int shaded_shadow_parts_screen = 1;
constexpr int shaded_shadow_parts_grid   = 2;

class Shaded_workers;

/*
 *  The scene recorder.  One instance (get()); recording() is non-null only
 *  while the world is being painted with recording on.
 */
class Shaded_light final : public Shape_paint_listener {
public:
	static Shaded_light& get();

	static Shaded_light* recording() {
		return active;
	}

	bool is_enabled() const {
		return record_on;
	}

	// The shaded light option (config, EXULT_SHADED_LIGHT, or a dump run) is
	// on.  The clock then picks palettes with lamps ignored.
	bool lighting_enabled() const {
		return lighting_on;
	}

	// Lighting relights the world this paint (night palettes).
	bool lighting_active() const {
		return lit_active;
	}

	// Lighting is on and so are traced shadows; the occlusion grid then
	// turns itself on.
	bool shadows_wanted() const {
		return lighting_on && params.shadows != 0;
	}

	// Lighting is on and so is weather lighting (the grid then runs too).
	bool weather_wanted() const {
		return lighting_on && params.weather != 0;
	}

	// Clouds show as patches of reduced sky light, not as sprites.
	bool cloud_shadows_shown() const {
		return weather_wanted() && params.cloud_shadows > 0.0f && lit_active;
	}

	// Lightning_effect's flash: on = the flash starts, false = it ends.
	// Starting returns true when shaded light shows it (a brief strong sky
	// light from a direction, with traced shadows), false when the caller
	// must show the LIGHTNING palette as before.  A flash is shown on at
	// least one painted frame, however soon it ends.
	bool lightning(bool on);
	// Around Rain_effect's drops (lamp_lit: rain or snow, not sparkles):
	// while weather lighting relights precipitation in this paint, the
	// pixels they paint are marked, so the light pass gives them the light
	// at their screen position instead of their surface's.
	void begin_particles(bool lamp_lit);
	void end_particles();

	// Palettes lighting works over: DUSK/DAWN, NIGHT and SPELL, and with
	// weather lighting OVERCAST, and DAY while there is weather.
	static bool lightable_palette(int n);
	// Called by Palette for the colours it is about to display: palnum is
	// its palettes.flx number (-1 = none), world is set when the palette was
	// loaded by number, transition when it is a clock transition into or out
	// of a lightable palette.  Returns true and puts the colours to display
	// in out768 when lighting takes over; false shows in768 unchanged.
	bool filter_palette(const unsigned char* in768, int palnum, bool world, bool transition, unsigned char* out768);

	// At the top of Game_window::paint, with a main actor: runs the debug
	// hooks and gathers the lights.  True if the paint must cover the
	// whole game area.
	bool prepare_paint();
	// Right after prepare_paint: while shadows are on, a moving occluder
	// shades beyond its own rectangle, so grow the paint rectangle (game
	// pixels) by the screen rectangles of the lights it touches.
	void widen_paint(int& x, int& y, int& w, int& h);
	// Around Game_render::paint_map in Game_window::paint.
	void begin_paint(Image_window8& win, bool full);
	void end_paint(Image_window8& win, bool full);
	// After the sprite effects: the light pass, debug views and dumps.
	void finish_paint(Image_window8& win, bool full);
	// After a world paint outside Game_window::paint (crystal balls, save
	// thumbnails): while lighting is active, relight that rectangle (game
	// pixels) at the ambient level, as the palette shown expects.
	void relight_ambient(Image_window8& win, int x, int y, int w, int h);
	// Around obj->paint() for a world object (ox, oy = its shape location).
	void begin_object(const Game_object& obj, int ox, int oy);
	void end_object();
	// Record a flat plane (chunk flats, dungeon blackness) at a lift.
	void record_plane(int x, int y, int w, int h, int lift, uint8_t face);
	// Next debug view; returns it.
	Shaded_view cycle_view();
	static const char* view_name(Shaded_view view);
	// World position (tiles, lifts) of a recorded game pixel.
	void world_position(int x, int y, float& wx, float& wy, float& wz) const;

	const Shaded_light_params& get_params() const {
		return params;
	}

	const Shaded_scene_buffers& get_buffers() const {
		return buffers;
	}

	const std::vector<Shaded_light_record>& get_lights() const {
		return lights;
	}

	void shape_painted(int xoff, int yoff, Shape_frame& shape, bool translucent, int xfcnt) override;
	~Shaded_light() override;

private:
	using Clock = std::chrono::steady_clock;

	static Shaded_light*  active;    // Set while recording a paint.
	Shaded_light_params   params;
	Shaded_scene_buffers  buffers;
	Image_buffer8*        target = nullptr;    // Buffer painted into.
	Shaded_rect           clip;                // Paint clip, within bounds.
	Shaded_object_context object;
	bool                  in_object    = false;
	bool                  in_particles = false;      // Marking rain or snow.
	Image_buffer8*        recorded     = nullptr;    // Recorded paint, until finish_paint.
	bool                  record_on    = false;      // Config or dump.
	std::unordered_map<const Game_object*, uint16_t> object_index;
	uint16_t                                         main_actor_id = shaded_id_none;
	bool                                             finish_due    = false;    // Recorded, finish_paint not run.
	// Config (config/video/shaded_light/...); sliders in params.
	bool lighting_on = false;
	// Shadow switches for headless runs (EXULT_SHADED_SHADOW_CACHE, _PARTS,
	// _GRID): caches on, which parts (shadow_parts_*), grid by rays.
	bool shadow_cache    = true;
	int  shadow_parts    = 0;
	bool shadow_grid_ray = false;
	// Light pass workers (params.threads - 1 of them, made when needed).
	std::unique_ptr<Shaded_workers> workers;
	std::string                     thread_check;    // For the dump info.
	// The light table (shaded_light.txt), keyed by (shape, frame or -1).
	std::map<std::pair<int, int>, Shaded_light_entry> light_table;
	std::string                                       table_game;    // Game and mod it was read for.
	bool                                              table_loaded = false;
	int                                               table_radius = 0;    // Largest radius, tiles.
	// The volumes section, keyed by (shape, frame or -1): Shaded_volume.
	std::map<std::pair<int, int>, uint8_t> volume_table;
	// Rounded silhouettes by frame, skip value, kind, box and offset of the
	// shape from the box origin; cleared with the game or when large.
	struct Silhouette_key {
		const void* frame;
		int         skip_from, kind, xtiles, ytiles, du, dv;

		bool operator==(const Silhouette_key& o) const {
			return frame == o.frame && skip_from == o.skip_from && kind == o.kind && xtiles == o.xtiles && ytiles == o.ytiles
				   && du == o.du && dv == o.dv;
		}
	};

	struct Silhouette_hash {
		size_t operator()(const Silhouette_key& k) const {
			size_t h = std::hash<const void*>()(k.frame);
			for (const int v : {k.skip_from, k.kind, k.xtiles, k.ytiles, k.du, k.dv}) {
				h = h * 1000003u ^ std::hash<int>()(v);
			}
			return h;
		}
	};

	std::unordered_map<Silhouette_key, Shaded_silhouette, Silhouette_hash> silhouettes;
	Shaded_rounded_extent              object_extent;     // Of the object being painted.
	std::vector<Shaded_rounded_extent> object_extents;    // Per object record, for the dump check.
	long                  rounded_pixels = 0;    // Written this paint.
	long                  seam_pixels    = 0;    // Merged this paint.
	// Lights of the current paint: the object lit by each (the bearer for
	// carried lights), the light object itself and where its row came from.
	std::vector<Shaded_light_record> lights;
	std::vector<const Game_object*>  light_objects;
	std::vector<const Game_object*>  light_sources;
	std::vector<Shaded_light_row>    light_rows;
	uint64_t                         light_hash    = 0;
	bool                             warned_lights = false;
	// Palette state (filter_palette): the ambient palette A, the remap
	// tables built from it, and the palette displayed while active.
	bool                               lit_active         = false;
	bool                               need_full          = false;    // Next paint must be full.
	bool                               seen_world_palette = false;
	bool                               applied_palette    = false;    // prepare_paint's one apply.
	bool                               have_ambient       = false;
	int                                clock_palette      = -1;    // Last palnum filtered.
	float                              ambient_y          = 0;     // Y of A, relative to DAY.
	float                              remap_slider       = -1;    // params.ambient of the remap.
	float                              remap_tint         = -1;    // params.night_tint of the remap.
	std::string                        remap_game;
	std::vector<unsigned char>         ambient_pal;    // 768: A.
	std::vector<unsigned char>         displayed_pal;    // 768: B'.
	std::vector<uint8_t>               remap;    // shaded_remap_size.
	Shaded_ramp                        ramp;
	std::vector<std::vector<uint8_t>>  ref_pals;    // shaded_ref_palettes, 768 each.
	std::string                        refs_game;    // Game the refs were read for.
	uint64_t                           lit_hash    = 0;    // Light hash of the last lit paint.
	std::array<float, 23>              lit_sliders{};    // Sliders of the last lit paint.
	int                                lit_grid    = -1;    // Grid generation of the last lit paint.
	std::vector<unsigned char>         unlit_frame;    // Frame before the pass, for dumps.
	long                               relit_pixels = 0;
	long long                          widened_need   = 0;    // prepare_paint's full paints, by cause.
	long long                          widened_lights = 0;
	long long                          widened_scroll = 0;
	long long                          widened_shadow = 0;    // widen_paint and grid changes.
	// Traced shadows of the current paint, and their caches: grid fields
	// and screen maps by everything they depend on.
	struct Shadow_stats {
		int    lights        = 0;    // With a map or field.
		int    maps          = 0;
		int    maps_reused   = 0;
		long   cells         = 0;    // Of all maps.
		long   traces        = 0;    // Screen traces run.
		int    fields_built  = 0;
		int    fields_reused = 0;
		double field_us      = 0;
		double screen_us     = 0;
	};

	struct Shadow_cache_entry {
		Shaded_shadow_map    map;
		Shaded_shadow_field  field;
		std::vector<uint8_t> data;
		long long            used = 0;    // shadow_paints when last used.
	};

	using Shadow_key = std::vector<int64_t>;
	Shaded_shadow_set                          shadow_set;
	bool                                       shadow_set_on = false;    // This paint's pass used it.
	Shadow_stats                               shadow_stats;
	std::map<Shadow_key, Shadow_cache_entry>   field_cache;
	std::map<Shadow_key, Shadow_cache_entry>   map_cache;
	std::vector<uint8_t>                       shadow_scratch;
	long long                                  shadow_paints = 0;
	int                                        field_generation = -1;    // Grid generation of field_cache.
	std::string                                shadow_info;    // For the dump info.
	// Weather: what the effects reported, the pass's inputs, and caches.
	// (Held by pointer: shadedweather.h includes this header.)
	std::unique_ptr<Shaded_weather_input> weather_input;
	std::unique_ptr<Shaded_weather_set>   weather_set;    // Sky map, clouds and masks.
	bool                 weather_set_on       = false;    // This paint's pass used it.
	bool                 weather_present      = false;    // Clouds or fog now (last gather).
	bool                 last_weather_present = false;
	float                overcast_target      = 0.0f;    // 1 while the world palette is OVERCAST.
	float                shadow_strength_config = 1.0f;    // Before the overcast softening.
	float                shadow_softness_config = 1.0f;
	uint64_t             cloud_hash      = 0;
	uint64_t             lit_cloud_hash  = 0;
	int                  open_generation = -1;    // What the sky map was built for.
	int                  open_origin_tx = 0, open_origin_ty = 0, open_size_x = 0, open_size_y = 0;
	float                open_fade = -1.0f;
	std::unordered_map<const Shape_frame*, int32_t> cloud_mask_cache;    // Frame to index in weather_set.masks.
	int                  cloud_mask_blur = -1;    // Blur of the cached masks.
	std::string          cloud_mask_game;
	// The weather palettes (FOG, LIGHTNING; raw 6-bit, 768 each) and the
	// fog tables fitted to FOG, built once per game.
	std::vector<std::vector<uint8_t>> weather_pals;
	std::string                       weather_refs_game;
	std::vector<uint8_t>              fog_table;    // shaded_fog_table_size.
	float                             fog_fit_mix   = 0.0f;
	float                             fog_colour[3] = {0.0f, 0.0f, 0.0f};
	// The Avatar's tile and the scroll tile of the last lit paint with fog.
	std::array<int, 5>   fog_track{};
	std::array<int, 5>   lit_fog_track{-1, -1, -1, -1, -1};
	bool                 weather_test       = false;    // EXULT_SHADED_WEATHER_TEST.
	bool                 weather_test_done  = false;
	// Lightning: the flash shown now, its direction (tiles, tiles, lifts
	// per unit, towards the flash), the flash tables fitted to LIGHTNING
	// and the flash dumps left to take (EXULT_SHADED_DUMP_FLASH).
	bool                 flash_on          = false;
	bool                 flash_shown       = false;    // Painted at least once.
	bool                 flash_end_pending = false;    // Ended before it was painted.
	int                  flash_count       = 0;
	float                flash_dir[3]      = {0.0f, 0.0f, 1.0f};
	std::vector<uint8_t> flash_table;    // shaded_flash_table_size; built, not applied.
	int                  flash_dumps_left  = 0;
	int                  flash_dumps_taken = 0;
	long                 flash_traces      = 0;    // Of the last flash pass.
	double               flash_pass_us     = 0;
	double               flash_us          = 0;    // Totals over all flash passes.
	std::string          weather_info;    // For the dump info.
	// Headless debug hooks (EXULT_SHADED_HOUR, EXULT_SHADED_TELEPORT).
	int  pending_hour     = -1;
	bool pending_teleport = false;
	int  teleport_to[3]   = {0, 0, 0};
	// Headless dumps (EXULT_SHADED_DUMP).
	std::string       dump_dir;
	std::vector<int>  dump_at;    // ms after the first recorded full paint.
	size_t            dump_next     = 0;
	bool              clock_started = false;
	Clock::time_point first_paint;
	// Timing (EXULT_SHADED_TIMING).
	bool              timing = false;
	Clock::time_point paint_start;
	long long         full_paints        = 0;
	double            paint_map_us       = 0;    // Totals over full paints.
	double            record_us          = 0;
	double            paint_record_us    = 0;    // In the current paint.
	long long         paints             = 0;    // All paints, full or not.
	double            paint_pixels       = 0;    // Total clip area.
	double            all_paint_us       = 0;
	double            all_record_us      = 0;
	long long         light_passes       = 0;    // Light pass, all and full paints.
	long long         full_light_passes  = 0;
	double            all_light_us       = 0;
	double            full_light_us      = 0;
	long long         shadow_passes      = 0;    // Shadow pass, all and full paints.
	long long         full_shadow_passes = 0;
	double            all_shadow_us      = 0;
	double            full_shadow_us     = 0;
	double            shadow_field_us    = 0;    // Totals over all passes.
	double            shadow_screen_us   = 0;
	double            weather_prep_us    = 0;    // Sky maps and cloud masks, all paints.
	long long         shadow_cache_hits  = 0;
	Clock::time_point last_report;
	int               game_width     = 0;    // Of the last full paint.
	int               game_height    = 0;
	bool              warned_backend = false;

	Shaded_light();
	Shaded_backend     backend();
	bool               setup_buffers(Image_window8& win);
	void               read_config();
	void               run_debug_hooks();
	void               load_table();
	Shaded_volume      volume_for(const Game_object& obj) const;
	const Shaded_silhouette& silhouette_for(Shape_frame& shape, int xoff, int yoff, int skip_from);
	Shaded_light_entry find_row(int shape, int frame, int brightness, Shaded_light_row& row) const;
	void               gather_lights();
	void               add_light(
						  const Game_object& source, const Game_object& owner, const Shaded_light_entry& entry,
						  Shaded_light_row row, bool carried);
	void               shift(int dx, int dy);
	std::array<float, 23> slider_values() const;
	bool               load_weather_refs();
	bool               prepare_weather();
	void               prepare_weather_pass();
	void               flash_pass(const Shaded_rect& r);
	void               end_flash();
	int                light_pass(
					   const Shaded_rect& r, uint8_t* levels, bool threaded, unsigned char* frame = nullptr,
					   const Shaded_shadow_set* shadows = nullptr, uint8_t* shadow_out = nullptr,
					   const Shaded_weather_set* weather = nullptr, const Shaded_weather_out* wout = nullptr);
	void               run_bands(int bands, const std::function<void(int)>& job);
	bool               shadow_pass(const Shaded_rect& r, Shaded_shadow_set& set, bool threaded, bool use_cache, Shadow_stats& stats);
	void               check_shadows(const Shaded_rect& r);
	void               check_weather(const Shaded_rect& r);
	bool               load_refs();
	void               set_lit(bool on);
	void               build_lut(uint8_t* lut) const;
	void               dump(Image_window8& win, int number, bool view, bool full);
	void               write_lighting_info(std::ostream& info);
	void               report_timing() const;
};

// Writes 8-bit RGB (or 16-bit big-endian grey) rows as a PNG; false on
// failure or when built without PNG support.
bool Shaded_write_png(
		const std::string& path, int w, int h, bool grey16, const std::vector<unsigned char>& data);

#endif
