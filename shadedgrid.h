/*
 *  shadedgrid.h - World occlusion grid for the "shaded light" renderer.
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
 *  A 3D grid over the map around the view, one cell per tile and lift,
 *  recording how much light each cell stops.  It answers "how much light
 *  gets from point A to point B?" for the shadow stage of shaded light.
 *
 *  The grid is built from the chunks' object lists: each object's 3D box
 *  (Shape_info sizes at its tile and lift), opaque when the shape is solid
 *  (the rule pathfinding uses), with per shape/frame overrides for openings
 *  (open doors, windows, bars, broken walls), canopies, roofs and floors.
 *
 *  Layout (also the GPU layout, see Shaded_grid_volume):
 *    cells  RG8, texel (x, y, z) at byte ((z * size_y + y) * size_x + x) * 2:
 *           [0] opacity (0 clear .. 255 opaque), [1] shaded_cell_* flags.
 *           It covers world tile ((origin_tx + x) mod 3072,
 *           (origin_ty + y) mod 3072) at lift z.
 *    sky    R8, size_x * size_y: 1 + the highest lift holding a covering
 *           cell (opacity >= 128, not canopy), 0 when open to the sky.
 *    params Shaded_grid_params as a std140 uniform block.
 *  Rays and hits are std430-friendly structs for a storage buffer.
 *
 *  Hooks (Map_chunk::add/remove, Game_object::change_frame, set_scrolls,
 *  clear_world) only mark chunks dirty; sync(), run by every query, moves
 *  the grid with the view and rebuilds the dirty chunks.  Off by default:
 *  when off nothing is allocated and each hook is one pointer test.
 */

#ifndef SHADEDGRID_H
#define SHADEDGRID_H 1

#include "exult_constants.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Game_object;
class Shaded_grid_driver;

constexpr int shaded_grid_lifts          = 16;    // Lifts 0..15; higher is clipped.
constexpr int shaded_grid_margin_chunks  = 1;     // Around the painted chunks.
constexpr int shaded_grid_max_chunks     = 48;    // Per axis.
constexpr int shaded_grid_sky_threshold  = 128;   // Opacity that counts as cover.
constexpr float shaded_grid_min_transmittance = 1.0f / 256.0f;

// Cell flags (byte 1 of a cell).
constexpr uint8_t shaded_cell_solid  = 0x01;    // Shape_info::is_solid().
constexpr uint8_t shaded_cell_passes = 0x02;    // light_passes_through row.
constexpr uint8_t shaded_cell_roof   = 0x04;    // roof_shapes.
constexpr uint8_t shaded_cell_floor  = 0x08;    // floor_shapes (upper storeys).
constexpr uint8_t shaded_cell_door   = 0x10;    // Shape_info::is_door().
constexpr uint8_t shaded_cell_canopy = 0x20;    // canopy_shapes (tree crowns).
constexpr uint8_t shaded_cell_object = 0x40;    // Any object was stamped here.

// Hit flags.
constexpr uint32_t shaded_hit_blocked = 1;    // Opaque cell or T < min.
constexpr uint32_t shaded_hit_outside = 2;    // Part of the ray left the grid.
constexpr uint32_t shaded_hit_passes  = 4;    // Crossed a passes cell.
constexpr uint32_t shaded_hit_canopy  = 8;    // Crossed a canopy cell.

// Ray options.
constexpr uint32_t shaded_ray_include_ends = 1;    // Count the end cells.

// Opacity for a "percent of light let through" value from the data.
constexpr uint8_t Shaded_grid_opacity_for(int percent) {
	const int p = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
	return static_cast<uint8_t>((255 * (100 - p) + 50) / 100);
}

static_assert(Shaded_grid_opacity_for(0) == 255);
static_assert(Shaded_grid_opacity_for(50) == 128);
static_assert(Shaded_grid_opacity_for(90) == 26);
static_assert(Shaded_grid_opacity_for(100) == 0);

/*
 *  Grid description, in vec4-sized groups of plain 32-bit values (std140).
 */
struct alignas(16) Shaded_grid_params {
	int32_t origin_tx = 0;    // World tile of cell (0, 0).
	int32_t origin_ty = 0;
	int32_t size_x    = 0;    // Cells.
	int32_t size_y    = 0;
	int32_t size_z         = shaded_grid_lifts;
	int32_t cells_per_tile = 1;
	int32_t cells_per_lift = 1;
	int32_t world_tiles    = c_num_tiles;    // Wrap.
	int32_t generation    = 0;    // Bumped on every change.
	int32_t map_num       = 0;
	int32_t sky_threshold = shaded_grid_sky_threshold;
	int32_t enabled       = 0;
	float   min_transmittance = shaded_grid_min_transmittance;
	float   reserved0         = 0.0f;
	float   reserved1         = 0.0f;
	float   reserved2         = 0.0f;
};

static_assert(sizeof(Shaded_grid_params) % 16 == 0);
static_assert(std::is_trivially_copyable_v<Shaded_grid_params> && std::is_standard_layout_v<Shaded_grid_params>);

/*
 *  A segment in world space: x, y in tiles, z in lifts (w unused).  Tile t
 *  covers [t, t + 1), so t + 0.5 is its centre.
 */
struct alignas(16) Shaded_grid_ray {
	float a[4];
	float b[4];
};

/*
 *  What a ray met.
 */
struct alignas(16) Shaded_grid_hit {
	float    transmittance = 1.0f;    // 0..1.
	uint32_t flags         = 0;       // shaded_hit_*.
	int32_t  hit_x = -1, hit_y = -1, hit_z = -1;    // First covering cell (world).
	int32_t  cells = 0;                              // Cells visited.
	int32_t  pad[2] = {0, 0};
};

static_assert(sizeof(Shaded_grid_ray) == 32);
static_assert(sizeof(Shaded_grid_hit) == 32);
static_assert(std::is_trivially_copyable_v<Shaded_grid_hit> && std::is_standard_layout_v<Shaded_grid_hit>);

/*
 *  The cells and the sky map.
 */
struct Shaded_grid_volume {
	int                  size_x = 0, size_y = 0, size_z = shaded_grid_lifts;
	int                  origin_tx = 0, origin_ty = 0;
	std::vector<uint8_t> cells;    // RG8: opacity, flags.
	std::vector<uint8_t> sky;      // R8.

	void resize(int sx, int sy, int sz);

	size_t index(int x, int y, int z) const {
		return ((static_cast<size_t>(z) * size_y + y) * size_x + x) * 2;
	}

	bool inside(int x, int y, int z) const {
		return x >= 0 && y >= 0 && z >= 0 && x < size_x && y < size_y && z < size_z;
	}

	uint8_t opacity(int x, int y, int z) const {
		return cells[index(x, y, z)];
	}

	uint8_t flags(int x, int y, int z) const {
		return cells[index(x, y, z) + 1];
	}

	// Local cell of a world tile (wrapping); false if outside.
	bool local(int world_tx, int world_ty, int& x, int& y) const;
	// Inclusive box, clipped; combines by max opacity and OR of flags.
	void stamp_box(int x0, int y0, int z0, int x1, int y1, int z1, uint8_t opa, uint8_t flg);
	// Clear a box of columns (all lifts).
	void clear_block(int x0, int y0, int w, int h);
	void rebuild_sky(int x0, int y0, int w, int h, int threshold);
};

/*
 *  CPU backend: the reference a shader mirrors.
 */
namespace Shaded_grid_cpu {
	// Amanatides-Woo 3D DDA from ray.a to ray.b.  Each cell crossed scales
	// the light by (1 - opacity / 255) ^ length, length in cells.  The cells
	// holding the end points are skipped unless shaded_ray_include_ends.
	// Cells outside the grid count as clear (shaded_hit_outside).
	Shaded_grid_hit trace(const Shaded_grid_volume& vol, const Shaded_grid_ray& ray, uint32_t options);
	void            trace_batch(
					   const Shaded_grid_volume& vol, const Shaded_grid_ray* rays, Shaded_grid_hit* hits, size_t count,
					   uint32_t options);
	// Local cell coordinates; outside the grid counts as open.
	bool open_to_sky(const Shaded_grid_volume& vol, int x, int y, int z);
}    // namespace Shaded_grid_cpu

/*
 *  An override for a shape (and frame).
 */
struct Shaded_grid_rule {
	int     percent = -1;    // Light let through, 0..100; -1: no change.
	uint8_t flags   = 0;     // shaded_cell_passes/canopy/roof/floor.
};

/*
 *  The override lists (shaded_grid.txt).
 */
class Shaded_grid_rules {
public:
	// Parse a file's text; patch rows replace rows with the same key.
	bool parse(std::string_view text, bool patch);
	// Embedded defaults for the game, then <PATCH>/shaded_grid.txt.
	void load();
	// An explicit (frame & 31) row first, then the all-frames row; plus
	// the shape's roof and floor flags.
	Shaded_grid_rule lookup(int shape, int frame) const;

	bool has_shape(int shape) const {
		return shapes.count(shape) != 0;
	}

	void clear();

	size_t rows_default = 0;
	size_t rows_patch   = 0;

private:
	std::unordered_map<int, Shaded_grid_rule> percent_rows;    // shape << 6 | frame.
	std::unordered_map<int, uint8_t>          shape_flags;     // Roof, floor.
	std::unordered_set<int>                   shapes;          // Any row.
};

// Synthetic tests over hand-built volumes; prints TEST PASS/FAIL lines.
// Returns the number of failures.
int Shaded_grid_selftest();

/*
 *  The world grid.  One instance (get()); active is non-null only when
 *  the grid is enabled.
 */
class Shaded_grid final {
public:
	static Shaded_grid* active;

	static Shaded_grid& get();

	void object_changed(const Game_object& obj, bool frame_change);
	void world_cleared();
	void view_changed();
	// Follow the view and rebuild dirty chunks; true if anything changed.
	bool sync();
	// World points (tiles, lifts).
	Shaded_grid_hit visibility(float ax, float ay, float az, float bx, float by, float bz, uint32_t options = 0);
	void trace_batch(const Shaded_grid_ray* rays, Shaded_grid_hit* hits, size_t count, uint32_t options = 0);
	bool open_to_sky(int tx, int ty, int lift);

	const Shaded_grid_volume& get_volume() const {
		return volume;
	}

	const Shaded_grid_params& get_params() const {
		return params;
	}

	const Shaded_grid_rules& get_rules() const {
		return rules;
	}

	// Box of cells changed since the last call (for partial uploads).
	bool take_dirty_box(int& x0, int& y0, int& z0, int& x1, int& y1, int& z1);
	// Dump and test driver (Time_sensitive), headless runs only.
	void driver_event();

private:
	using Clock = std::chrono::steady_clock;

	struct Slot {
		int  cx = -1, cy = -1;    // Map chunk in this slot.
		bool built = false;
		bool dirty = false;
	};

	Shaded_grid_volume volume;
	Shaded_grid_params params;
	Shaded_grid_rules  rules;
	std::vector<Slot>  slots;
	int                origin_cx = 0, origin_cy = 0;    // Chunk of slot (0, 0).
	int                chunks_x = 0, chunks_y = 0;
	int                map_num  = -1;
	bool               any_dirty = false;
	bool               warned_cap = false;
	int                dirty_x0 = 0, dirty_y0 = 0, dirty_x1 = -1, dirty_y1 = -1;
	std::vector<uint8_t> slot_scratch;    // Old cells of the slot being rebuilt.
	// Statistics.
	long long clipped_cells   = 0;    // Above lift 15, last full build.
	long long chunks_built    = 0;
	long long chunks_unchanged = 0;    // Rebuilt to the same cells.
	double    last_build_ms   = 0;    // Last sync that built something.
	double    last_recentre_ms = 0;
	int       last_built      = 0;
	// Headless dumps and tests.
	std::string                          dump_dir;
	std::vector<int>                     dump_at;    // ms after activation.
	size_t                               dump_next = 0;
	bool                                 test_mode = false;
	bool                                 scheduled = false;
	bool                                 in_cpp_toggle = false;
	unsigned                             activation_ticks = 0;
	int                                  dump_number = 0;
	std::unique_ptr<Shaded_grid_driver>  driver;
	std::vector<std::weak_ptr<const Game_object>> door_probes;

	Shaded_grid();
	void desired_extent(int& cx0, int& cy0, int& w, int& h) const;
	void recentre(int cx0, int cy0, int w, int h);
	bool build_slot(int sx, int sy);    // True when the cells changed.
	void stamp_object(const Game_object& obj, int sx, int sy);
	void mark_dirty(int world_tx, int world_ty);
	void schedule();
	void dump(int number);
	void run_world_tests(int number);
	void poll_door_probes();
	bool door_probe(const Game_object& door, float& transmittance, bool& blocked, int& cells);
	friend class Shaded_grid_driver;
};

inline void Shaded_grid_object_changed(const Game_object* obj, bool frame_change = false) {
	if (Shaded_grid::active && obj) {
		Shaded_grid::active->object_changed(*obj, frame_change);
	}
}

void Shaded_grid_world_cleared();
void Shaded_grid_view_changed();

#endif
