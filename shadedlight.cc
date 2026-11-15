/*
 *  shadedlight.cc - Height-aware "shaded light" scene record.
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

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include "shadedlight.h"

#include "Configuration.h"
#include "actors.h"
#include "chunks.h"
#include "data/exult_bg_flx.h"
#include "data/exult_si_flx.h"
#include "data_utils.h"
#include "effects.h"
#include "exceptions.h"
#include "exult.h"
#include "fnames.h"
#include "game.h"
#include "gameclk.h"
#include "gamemap.h"
#include "gamewin.h"
#include "ibuf8.h"
#include "ignore_unused_variable_warning.h"
#include "iwin8.h"
#include "palette.h"
#include "ready.h"
#include "shadedgrid.h"
#include "shadedweather.h"
#include "shapeinf.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef HAVE_PNG_H
#	include <png.h>
#endif

// The classifier, on a 1x1 tile, 4-lift box (8 x 8 px footprint, 16 px high).
static_assert(Shaded_classify_box(-20, -20, 8, 8, 16).facing == shaded_face_top);
static_assert(Shaded_classify_box(-20, -20, 8, 8, 16).dz_px == 16);
static_assert(Shaded_classify_box(0, -5, 8, 8, 16).facing == shaded_face_east);
static_assert(Shaded_classify_box(0, -5, 8, 8, 16).dz_px == 0);
static_assert(Shaded_classify_box(-3, 0, 8, 8, 16).facing == shaded_face_south);
static_assert(Shaded_classify_box(-3, 0, 8, 8, 16).dz_px == 0);
static_assert(Shaded_classify_box(-10, -6, 8, 8, 16).facing == shaded_face_south);
static_assert(Shaded_classify_box(-10, -6, 8, 8, 16).dz_px == 6);
static_assert(Shaded_classify_box(-40, -40, 8, 8, 16).facing == (shaded_face_east | shaded_face_clamped));
static_assert(Shaded_classify_box(-40, -40, 8, 8, 16).dz_px == 16);
// Flats are ground at the bottom of the box.
static_assert(Shaded_classify_box(-3, -3, 8, 8, 0).facing == shaded_face_ground);
static_assert(Shaded_classify_box(-3, -9, 8, 8, 0).facing == (shaded_face_ground | shaded_face_clamped));
// Mirrored frames swap x and y sizes, which swaps the south and east faces.
static_assert(Shaded_classify_box(-10, -6, 8, 16, 16).facing == shaded_face_south);
static_assert(Shaded_classify_box(-6, -10, 16, 8, 16).facing == shaded_face_east);
static_assert(Shaded_classify_box(-10, -6, 8, 16, 16).dz_px == Shaded_classify_box(-6, -10, 16, 8, 16).dz_px);
// The default rows: candle, torch and lamp.
static_assert(Shaded_default_light(1).radius == 4 && Shaded_default_light(1).brightness == 60);
static_assert(Shaded_default_light(5).radius == 8 && Shaded_default_light(5).brightness == 100);
static_assert(Shaded_default_light(7).radius == 10 && Shaded_default_light(7).brightness == 120);
// Falloff: full at the light, a quarter half way out (in d^2), none past R.
static_assert(Shaded_falloff(0.0f) == 1.0f);
static_assert(Shaded_falloff(0.5f) == 0.25f);
static_assert(Shaded_falloff(1.0f) == 0.0f && Shaded_falloff(2.0f) == 0.0f);
// Dither thresholds: inside (0, 1), repeating every 4 pixels.
static_assert(Shaded_bayer4(0, 0) == 0.5f / 16.0f && Shaded_bayer4(3, 3) == 5.5f / 16.0f);
static_assert(Shaded_bayer4(0, 3) == 15.5f / 16.0f && Shaded_bayer4(4, -4) == Shaded_bayer4(0, 0));
// Facing: a south face sees lights to its south, an east face lights to
// its east, and tops and ground only lights above them.
static_assert(Shaded_faces_light(shaded_face_south, 0.0f, 2.0f, 0.0f));
static_assert(!Shaded_faces_light(shaded_face_south, 0.0f, -2.0f, 0.0f));
static_assert(Shaded_faces_light(shaded_face_east, 2.0f, 0.0f, 0.0f));
static_assert(!Shaded_faces_light(shaded_face_east, -2.0f, 0.0f, 0.0f));
static_assert(Shaded_faces_light(shaded_face_top, 1.0f, 1.0f, 0.5f));
static_assert(!Shaded_faces_light(shaded_face_top, 1.0f, 1.0f, -0.5f));
static_assert(!Shaded_faces_light(shaded_face_ground | shaded_face_clamped, 0.0f, 0.0f, -1.0f));
// Luminance weights sum to 1: white is 63 * 1000.
static_assert(Shaded_luminance(63, 63, 63) == 63000 && Shaded_luminance(0, 0, 0) == 0);
static_assert(Shaded_luminance(0, 63, 0) > Shaded_luminance(63, 0, 0) && Shaded_luminance(63, 0, 0) > Shaded_luminance(0, 0, 63));
// The cycling colours are left alone.
static_assert(shaded_last_lit_index == 0xdf && shaded_remap_size == 32 * 256);
// Projecting a recorded pixel's world position gives the pixel back.
static_assert(Shaded_project(Shaded_unproject(37, 2.5f, 100, 3, 8.0f, 4.0f), 2.5f, 100, 3, 8.0f, 4.0f) == 37.0f);
static_assert(Shaded_project(Shaded_unproject(-6, 0.0f, 2000, 0, 8.0f, 4.0f), 0.0f, 2000, 0, 8.0f, 4.0f) == -6.0f);
// Shortest way round the world.
static_assert(Shaded_wrap_delta(3070.0f) == -2.0f && Shaded_wrap_delta(-3070.0f) == 2.0f && Shaded_wrap_delta(5.0f) == 5.0f);
// A point light's target is the light, a directional one 64 units on.
static_assert(Shaded_shadow_target(Shaded_light_record{1.0f, 2.0f, 3.0f, 4, 1, 1, 1, 1, 0, 0, 0, 0}, Shaded_point{3071.0f, 2.0f, 0.0f}).x
			  == 3073.0f);
static_assert(Shaded_shadow_target(Shaded_light_record{0.5f, 0.0f, 0.25f, 4, 1, 1, 1, 1, shaded_light_directional, 0, 0, 0},
								   Shaded_point{10.0f, 10.0f, 0.0f})
					  .z
			  == 16.0f);
namespace {
	constexpr float box_lo[3] = {0.0f, 0.0f, 0.0f};
	constexpr float box_hi[3] = {2.0f, 2.0f, 2.0f};
	constexpr float q_out[3]  = {4.0f, 1.0f, 1.0f};
	constexpr float q_in[3]   = {1.0f, 1.0f, 1.5f};
	constexpr float q_edge[3] = {3.0f, 3.0f, 1.0f};
}    // namespace

// A grid ray to a lamp stops a margin before the lamp's own box, misses
// it unchanged, and is empty from inside it.
static_assert(Shaded_clip_to_box(Shaded_point{4.0f, 1.0f, 1.0f}, Shaded_point{0.0f, 1.0f, 1.0f}, box_lo, box_hi, 0.5f).x == 2.5f);
static_assert(Shaded_clip_to_box(Shaded_point{4.0f, 5.0f, 1.0f}, Shaded_point{1.0f, 5.0f, 1.0f}, box_lo, box_hi, 0.5f).x == 1.0f);
static_assert(Shaded_clip_to_box(Shaded_point{1.0f, 1.0f, 1.0f}, Shaded_point{1.5f, 1.0f, 1.0f}, box_lo, box_hi, 0.5f).x == 1.0f);
// Signed box distance: 2 tiles outside, half a tile inside, a corner.
static_assert(Shaded_box_sd(q_out, box_lo, box_hi).outside2 == 4.0f && Shaded_box_sd(q_out, box_lo, box_hi).inside == 0.0f);
static_assert(Shaded_box_sd(q_in, box_lo, box_hi).outside2 == 0.0f && Shaded_box_sd(q_in, box_lo, box_hi).inside == -0.5f);
static_assert(Shaded_box_sd(q_edge, box_lo, box_hi).outside2 == 2.0f);
// Cone visibility: half at the edge, full a cone radius out, none as far in.
static_assert(Shaded_cone_visibility(0.0f, 0.5f, false) == 0.5f);
static_assert(Shaded_cone_visibility(0.5f, 0.5f, false) == 1.0f && Shaded_cone_visibility(-0.5f, 0.5f, false) == 0.0f);
static_assert(Shaded_cone_visibility(0.03125f, 0.0f, false) == 0.75f);    // Cone at least 1/16.
static_assert(Shaded_cone_visibility(0.01f, 0.5f, true) == 1.0f && Shaded_cone_visibility(-0.01f, 0.5f, true) == 0.0f);
namespace {
	// Octahedral normals round-trip within 2/127 per component.
	constexpr bool Normal_round_trip(float x, float y, float z) {
		const float          len = Shaded_sqrt(x * x + y * y + z * z);
		const Shaded_normalf n   = Shaded_decode_normal(Shaded_encode_normal(x / len, y / len, z / len));
		auto                 off = [](float a, float b) {
			return (a > b ? a - b : b - a) > 2.0f / 127.0f;
		};
		return !off(n.x, x / len) && !off(n.y, y / len) && !off(n.z, z / len);
	}

	constexpr Shaded_object_volume vol_bush{
			{10.0f, 10.0f, 1.0f},
			{1.0f, 1.5f, 0.5f},
			0.0f, 0.0f, static_cast<int32_t>(Shaded_volume::round), {0, 0, 0}
	};
	constexpr Shaded_object_volume vol_tree{
			{10.0f, 10.0f, 12.0f},
			{2.0f, 2.0f, 2.0f},
			0.25f, 12.0f, static_cast<int32_t>(Shaded_volume::tree), {0, 0, 0}
	};
	constexpr float q_centre[3] = {0.0f, 0.0f, 0.0f};
	constexpr float q_beyond[3] = {2.0f, 0.0f, 0.0f};    // 1 tile past the x radius.
	constexpr float q_trunk[3]  = {0.1f, 0.0f, -4.0f};    // Under the crown, in the trunk.
	constexpr float q_beside[3] = {1.0f, 0.0f, -4.0f};    // Beside the trunk.

	constexpr bool Near(float a, float b, float e) {
		return (a > b ? a - b : b - a) <= e;
	}
}    // namespace

static_assert(Shaded_sqrt(16.0f) == 4.0f && Near(Shaded_sqrt(2.0f), 1.41421356f, 1e-6f));
static_assert(Normal_round_trip(0.0f, 0.0f, 1.0f));     // Up.
static_assert(Normal_round_trip(0.0f, 1.0f, 0.0f));     // South.
static_assert(Normal_round_trip(1.0f, 0.0f, 0.0f));     // East.
static_assert(Normal_round_trip(0.3f, -0.5f, -0.2f));    // Below the horizon.
static_assert(Normal_round_trip(-0.6f, 0.6f, 0.5f));
static_assert(Shaded_encode_normal(0.0f, 0.0f, 1.0f) == 0);    // Up is 0, as are box pixels.
// Rounded shadow volumes: inside is negative, a tile out about +1, and a
// tree's trunk is solid under its crown.
static_assert(Shaded_rounded_sd(q_centre, vol_bush, 0.5f) < 0.0f);
static_assert(Near(Shaded_rounded_sd(q_beyond, vol_bush, 0.5f), 1.0f, 0.1f));
static_assert(Shaded_rounded_sd(q_trunk, vol_tree, 0.5f) < 0.0f);
static_assert(Near(Shaded_rounded_sd(q_beside, vol_tree, 0.5f), 0.75f, 0.01f));

namespace {
	constexpr uint8_t field_clear[8] = {254, 254, 254, 254, 254, 254, 254, 254};
	constexpr uint8_t field_solid[8] = {255, 255, 255, 255, 255, 255, 255, 255};
	constexpr uint8_t field_wall[8]  = {255, 0, 255, 0, 255, 0, 255, 0};      // Solid at x, dark at x + 1.
	constexpr uint8_t field_half[8]  = {254, 0, 254, 0, 254, 0, 254, 0};
	constexpr uint8_t field_lit[8]   = {255, 254, 255, 254, 255, 254, 255, 254};
}    // namespace

// Solid-aware trilinear: solid cells are left out, all solid is open.
static_assert(Shaded_solid_trilinear(field_clear, 0.25f, 0.5f, 0.75f) == 1.0f);
static_assert(Shaded_solid_trilinear(field_solid, 0.5f, 0.5f, 0.5f) == 1.0f);
static_assert(Shaded_solid_trilinear(field_wall, 0.5f, 0.5f, 0.5f) == 0.0f);
static_assert(Shaded_solid_trilinear(field_half, 0.5f, 0.5f, 0.5f) == 0.5f);
static_assert(Shaded_solid_trilinear(field_lit, 0.25f, 0.5f, 0.5f) == 1.0f);

/*
 *  A small persistent thread pool for the light pass.  run() hands bands
 *  out to the workers and the calling thread, and returns when all are
 *  done.
 */
class Shaded_workers {
public:
	explicit Shaded_workers(int threads) {
		for (int i = 1; i < threads; i++) {
			workers.emplace_back([this]() {
				work();
			});
		}
	}

	~Shaded_workers() {
		{
			const std::lock_guard<std::mutex> lock(mutex);
			stop = true;
		}
		wake.notify_all();
		for (auto& worker : workers) {
			worker.join();
		}
	}

	Shaded_workers(const Shaded_workers&)            = delete;
	Shaded_workers& operator=(const Shaded_workers&) = delete;

	int size() const {
		return static_cast<int>(workers.size()) + 1;
	}

	void run(int bands, const std::function<void(int)>& job) {
		{
			const std::lock_guard<std::mutex> lock(mutex);
			current    = &job;
			band_count = bands;
			next_band  = 0;
			remaining  = bands;
			generation++;
		}
		wake.notify_all();
		std::unique_lock<std::mutex> lock(mutex);
		take_bands(lock);
		done.wait(lock, [this]() {
			return remaining == 0;
		});
		current = nullptr;
	}

private:
	std::vector<std::thread>          workers;
	std::mutex                        mutex;
	std::condition_variable           wake;
	std::condition_variable           done;
	const std::function<void(int)>*   current    = nullptr;
	int                               band_count = 0;
	int                               next_band  = 0;
	int                               remaining  = 0;
	uint64_t                          generation = 0;
	bool                              stop       = false;

	// Run bands until none are left; the lock is held on entry and exit.
	void take_bands(std::unique_lock<std::mutex>& lock) {
		while (next_band < band_count) {
			const int                       band = next_band++;
			const std::function<void(int)>* job  = current;
			lock.unlock();
			(*job)(band);
			lock.lock();
			if (--remaining == 0) {
				done.notify_all();
			}
		}
	}

	void work() {
		uint64_t                     seen = 0;
		std::unique_lock<std::mutex> lock(mutex);
		while (true) {
			wake.wait(lock, [this, seen]() {
				return stop || generation != seen;
			});
			if (stop) {
				return;
			}
			seen = generation;
			take_bands(lock);
		}
	}
};

namespace {
	constexpr int lut_size = 64;    // False colour codes per view.

	struct Rgb {
		int r, g, b;
	};

	Shaded_rect Intersect(const Shaded_rect& a, const Shaded_rect& b) {
		const int x0 = std::max(a.x, b.x);
		const int y0 = std::max(a.y, b.y);
		const int x1 = std::min(a.x + a.w, b.x + b.w);
		const int y1 = std::min(a.y + a.h, b.y + b.h);
		if (x1 <= x0 || y1 <= y0) {
			return Shaded_rect{};
		}
		return Shaded_rect{x0, y0, x1 - x0, y1 - y0};
	}

	// Blue (low) through cyan, green and yellow to red (high); t in [0, 1].
	Rgb Height_colour(float t) {
		static const Rgb stops[] = {
				{30, 40, 150},
				{40, 170, 220},
				{60, 200, 80},
				{240, 220, 60},
				{230, 50, 40}
		};
		t               = std::clamp(t, 0.0f, 1.0f) * 4.0f;
		const int   i   = std::min(static_cast<int>(t), 3);
		const float f   = t - static_cast<float>(i);
		auto        mix = [f](int a, int b) {
			return static_cast<int>(static_cast<float>(a) + (static_cast<float>(b - a) * f));
		};
		return Rgb{mix(stops[i].r, stops[i + 1].r), mix(stops[i].g, stops[i + 1].g), mix(stops[i].b, stops[i + 1].b)};
	}

	Rgb Facing_colour(uint8_t facing) {
		Rgb col;
		switch (facing & shaded_face_mask) {
		case shaded_face_ground:
			col = Rgb{60, 140, 60};
			break;
		case shaded_face_top:
			col = Rgb{235, 215, 80};
			break;
		case shaded_face_south:
			col = Rgb{70, 110, 235};
			break;
		case shaded_face_east:
			col = Rgb{225, 70, 60};
			break;
		case shaded_face_rounded:
			col = Rgb{200, 120, 220};
			break;
		default:
			return Rgb{0, 0, 0};
		}
		if (facing & shaded_face_clamped) {
			col = Rgb{col.r * 6 / 10, col.g * 6 / 10, col.b * 6 / 10};
		}
		return col;
	}

	// Well spread, fairly bright colour per object id; black for none.
	Rgb Id_colour(unsigned id) {
		if (id == shaded_id_none) {
			return Rgb{0, 0, 0};
		}
		const uint32_t h = static_cast<uint32_t>(id) * 2654435761u;
		return Rgb{static_cast<int>(64 + ((h >> 24) % 192)), static_cast<int>(64 + ((h >> 16) % 192)),
				   static_cast<int>(64 + ((h >> 8) % 192))};
	}

	// Face and height of one pixel of an object; (u, v) from its origin.
	// Rounded volumes are recorded by record_rounded; their non-RLE frames
	// and every box object come here.
	inline Shaded_box_hit Classify(const Shaded_object_context& obj, int u, int v) {
		return Shaded_classify_box(u, v, obj.xpx, obj.ypx, obj.hpx);
	}

	// Raw row pointers into the three buffers (fast even unoptimised).
	struct Shaded_row {
		uint16_t* height;
		uint8_t*  facing;
		uint16_t* ids;

		Shaded_row(Shaded_scene_buffers& buf, int x, int y) {
			const int idx = buf.index(x, y);
			height        = buf.height_map.data() + idx;
			facing        = buf.facing_map.data() + idx;
			ids           = buf.object_ids.data() + idx;
		}
	};


	using Light_table = std::map<std::pair<int, int>, Shaded_light_entry>;

	/*
	 *  Reads the lights section of shaded_light.txt:
	 *      shape/frame/radius/brightness/red/green/blue/height/flags
	 *  Patch rows replace static rows with the same shape and frame.
	 */
	class Shaded_light_reader final : public Base_reader {
		Light_table& table;

	protected:
		void read_data(std::istream& in, size_t index, int version, bool patch, Exult_Game game, bool binary) override {
			ignore_unused_variable_warning(index, version, patch, game, binary);
			const int shape = ReadInt(in, -1);
			int       frame = ReadInt(in, -1);
			if (shape < 0) {
				return;
			}
			frame = frame < 0 ? -1 : (frame & 0xff);
			// Missing fields take a torch's row.
			const Shaded_light_entry def = Shaded_default_light(5);
			Shaded_light_entry       entry;
			entry.radius     = std::max(ReadInt(in, def.radius), 0);
			entry.brightness = std::max(ReadInt(in, def.brightness), 0);
			entry.red        = std::clamp(ReadInt(in, def.red), 0, 255);
			entry.green      = std::clamp(ReadInt(in, def.green), 0, 255);
			entry.blue       = std::clamp(ReadInt(in, def.blue), 0, 255);
			entry.height     = ReadInt(in, def.height);
			entry.flags      = ReadInt(in, def.flags) & (shaded_light_area | shaded_light_off);

			table[std::make_pair(shape, frame)] = entry;
		}

	public:
		explicit Shaded_light_reader(Light_table& t) : Base_reader(false), table(t) {}
	};

	using Volume_table = std::map<std::pair<int, int>, uint8_t>;

	/*
	 *  Reads the volumes section of shaded_light.txt:
	 *      shape/frame/kind
	 *  Patch rows replace static rows with the same shape and frame.
	 */
	class Shaded_volume_reader final : public Base_reader {
		Volume_table& table;

	protected:
		void read_data(std::istream& in, size_t index, int version, bool patch, Exult_Game game, bool binary) override {
			ignore_unused_variable_warning(index, version, patch, game, binary);
			const int shape = ReadInt(in, -1);
			int       frame = ReadInt(in, -1);
			const int kind  = ReadInt(in, -1);
			if (shape < 0 || kind < 0) {
				return;
			}
			frame = frame < 0 ? -1 : (frame & 31);

			table[std::make_pair(shape, frame)] = static_cast<uint8_t>(std::min(kind, shaded_volume_kinds - 1));
		}

	public:
		explicit Shaded_volume_reader(Volume_table& t) : Base_reader(false), table(t) {}
	};

	// FNV-1a over 32-bit values.
	void Hash_add(uint64_t& hash, int32_t value) {
		auto v = static_cast<uint32_t>(value);
		for (int i = 0; i < 4; i++) {
			hash ^= v & 0xffu;
			hash *= 1099511628211ull;
			v >>= 8;
		}
	}

	// The game and mod, to notice when data must be read again.
	std::string Game_key() {
		return std::to_string(static_cast<int>(Game::get_game_type())) + "/" + Game::get_gametitle() + "/"
			   + Game::get_modtitle();
	}

	const char* Row_name(Shaded_light_row row) {
		switch (row) {
		case Shaded_light_row::exact:
			return "table exact";
		case Shaded_light_row::wildcard:
			return "table wildcard";
		case Shaded_light_row::formula:
			break;
		}
		return "default";
	}

	// Screen map cell size: 1, 2 or 4 pixels.
	int Shadow_resolution(int value) {
		return value >= 4 ? 4 : (value >= 2 ? 2 : 1);
	}

	// How much an object stops light in the screen-space shadow trace:
	// people fully (they are not in the grid); shapes the grid lets light
	// through (glass, bars, canopy, open doors) not at all, as the grid
	// already applies their share, except rounded ones (a tree's canopy)
	// stop the rest of it in the shape of their volume; other solid shapes
	// with height fully.
	uint8_t Screen_opacity(const Game_object& obj, const Shape_info& info, int frame, Shaded_volume volume) {
		if (obj.as_actor()) {
			return 255;
		}
		const Shaded_grid* grid  = Shaded_grid::active;
		const int          shape = obj.get_shapenum();
		if (grid && grid->get_rules().has_shape(shape)) {
			const int percent = grid->get_rules().lookup(shape, frame).percent;
			if (percent > 0) {
				return volume == Shaded_volume::box ? 0 : static_cast<uint8_t>(255 * (100 - std::min(percent, 100)) / 100);
			}
		}
		return info.is_solid() && info.get_3d_height() > 0 ? 255 : 0;
	}
}    // namespace

/*
 *  Writes rows of 8-bit RGB (or 16-bit big-endian grey) as a PNG.  Returns
 *  false on failure or without PNG support.
 */
bool Shaded_write_png(
		const std::string& path, int w, int h, bool grey16, const std::vector<unsigned char>& data) {
#ifdef HAVE_PNG_H
	FILE* fp = fopen(path.c_str(), "wb");
	if (!fp) {
		return false;
	}
	png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	png_infop   info = png ? png_create_info_struct(png) : nullptr;
	if (!png || !info) {
		png_destroy_write_struct(&png, &info);
		fclose(fp);
		return false;
	}
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		fclose(fp);
		return false;
	}
	png_init_io(png, fp);
	png_set_IHDR(
			png, info, static_cast<png_uint_32>(w), static_cast<png_uint_32>(h), grey16 ? 16 : 8,
			grey16 ? PNG_COLOR_TYPE_GRAY : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
			PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	const size_t                row_bytes = static_cast<size_t>(w) * (grey16 ? 2 : 3);
	std::vector<png_const_bytep> rows(static_cast<size_t>(h));
	for (int y = 0; y < h; y++) {
		rows[static_cast<size_t>(y)] = data.data() + static_cast<size_t>(y) * row_bytes;
	}
	png_write_rows(png, const_cast<png_bytepp>(rows.data()), static_cast<png_uint_32>(h));
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
	fclose(fp);
	return true;
#else
	ignore_unused_variable_warning(path, w, h, grey16, data);
	return false;
#endif
}

Shaded_rect Shaded_scene_buffers::bounds() const {
	if (stride <= 0) {
		return Shaded_rect{};
	}
	const int row0 = origin / stride;
	const int col0 = origin - row0 * stride;
	return Shaded_rect{-col0, -row0, width, height};
}

void Shaded_cpu::clear_rect(Shaded_scene_buffers& buf, const Shaded_rect& r) {
	fill_plane(buf, r, 0, shaded_face_none, shaded_id_none);
}

void Shaded_cpu::fill_plane(Shaded_scene_buffers& buf, const Shaded_rect& r, uint16_t height, uint8_t facing, uint16_t id) {
	for (int y = r.y; y < r.y + r.h; y++) {
		const Shaded_row row(buf, r.x, y);
		std::fill_n(row.height, r.w, height);
		std::memset(row.facing, facing, static_cast<size_t>(r.w));
		std::fill_n(row.ids, r.w, id);
	}
}

void Shaded_cpu::record_spans(
		Shaded_scene_buffers& buf, const Shaded_rect& clip, const Shaded_object_context& obj, const unsigned char* rle,
		int xoff, int yoff, int skip_from) {
	// Same format as Image_buffer8::paint_rle.
	const int    right  = clip.x + clip.w;
	const int    bottom = clip.y + clip.h;
	const uint8* in     = rle;
	int          scanlen;
	while ((scanlen = little_endian::Read2(in)) != 0) {
		const int encoded = scanlen & 1;
		scanlen           = scanlen >> 1;
		const int  scanx  = xoff + static_cast<sint16>(little_endian::Read2(in));
		const int  scany  = yoff + static_cast<sint16>(little_endian::Read2(in));
		const bool row_on = scany >= clip.y && scany < bottom;
		// Record count pixels from x; pix = their values (nullptr: all pixv).
		auto run = [&](int x, int count, const uint8* pix, int pixv) {
			if (!row_on) {
				return;
			}
			const int x0 = std::max(x, clip.x);
			const int x1 = std::min(x + count, right);
			if (x0 >= x1) {
				return;
			}
			const Shaded_row row(buf, x0, scany);
			const int        v = scany - obj.oy;
			for (int px = x0; px < x1; px++) {
				const int val = pix ? pix[px - x] : pixv;
				if (val >= skip_from && val <= 0xfe) {
					continue;    // Blended, so what is beneath shows.
				}
				const Shaded_box_hit hit    = Classify(obj, px - obj.ox, v);
				const int            height = obj.base_height + hit.dz_px * (shaded_height_units_per_lift / 4);
				const int            i      = px - x0;
				row.height[i]               = static_cast<uint16_t>(std::min(height, 0xffff));
				row.facing[i]               = hit.facing;
				row.ids[i]                  = obj.id;
			}
		};
		if (!encoded) {
			run(scanx, scanlen, in, 0);
			in += scanlen;
			continue;
		}
		for (int b = 0; b < scanlen;) {
			unsigned char bcnt   = Read1(in);
			const int     repeat = bcnt & 1;
			bcnt                 = bcnt >> 1;
			if (repeat) {
				run(scanx + b, bcnt, nullptr, Read1(in));
			} else {
				run(scanx + b, bcnt, in, 0);
				in += bcnt;
			}
			b += bcnt;
		}
	}
}

namespace {
	// Visit the pixels of an RLE frame painted at (xoff, yoff), as
	// Image_buffer8::paint_rle reads it: visit(x, y, value) per pixel.
	template <typename Visit>
	void For_rle_pixels(const unsigned char* rle, int xoff, int yoff, Visit&& visit) {
		const uint8* in = rle;
		int          scanlen;
		while ((scanlen = little_endian::Read2(in)) != 0) {
			const int encoded = scanlen & 1;
			scanlen           = scanlen >> 1;
			const int scanx   = xoff + static_cast<sint16>(little_endian::Read2(in));
			const int scany   = yoff + static_cast<sint16>(little_endian::Read2(in));
			if (!encoded) {
				for (int i = 0; i < scanlen; i++) {
					visit(scanx + i, scany, static_cast<int>(in[i]));
				}
				in += scanlen;
				continue;
			}
			for (int b = 0; b < scanlen;) {
				unsigned char bcnt   = Read1(in);
				const int     repeat = bcnt & 1;
				bcnt                 = bcnt >> 1;
				if (repeat) {
					const int pixv = Read1(in);
					for (int i = 0; i < bcnt; i++) {
						visit(scanx + b + i, scany, pixv);
					}
				} else {
					for (int i = 0; i < bcnt; i++) {
						visit(scanx + b + i, scany, static_cast<int>(in[i]));
					}
					in += bcnt;
				}
				b += bcnt;
			}
		}
	}

	// 3-4 chamfer distance transform in place: inside cells (non-zero on
	// entry) get 3 x their distance in pixels to the nearest outside cell;
	// cells beyond the grid are outside.
	void Chamfer_distance(std::vector<int>& d, int w, int h) {
		auto at = [&](int x, int y) {
			return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : d[static_cast<size_t>(y) * w + x];
		};
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				int& v = d[static_cast<size_t>(y) * w + x];
				if (v != 0) {
					v = std::min({v, at(x - 1, y) + 3, at(x, y - 1) + 3, at(x - 1, y - 1) + 4, at(x + 1, y - 1) + 4});
				}
			}
		}
		for (int y = h - 1; y >= 0; y--) {
			for (int x = w - 1; x >= 0; x--) {
				int& v = d[static_cast<size_t>(y) * w + x];
				if (v != 0) {
					v = std::min({v, at(x + 1, y) + 3, at(x, y + 1) + 3, at(x + 1, y + 1) + 4, at(x - 1, y + 1) + 4});
				}
			}
		}
	}
}    // namespace

void Shaded_cpu::build_silhouette(
		const unsigned char* rle, int xleft, int yabove, int w, int h, int skip_from, Shaded_volume kind, int xpx, int ypx,
		float axu, float axv, Shaded_silhouette& out) {
	out        = Shaded_silhouette{};
	out.xleft  = xleft;
	out.yabove = yabove;
	out.w      = std::max(w, 0);
	out.h      = std::max(h, 0);
	const auto size = static_cast<size_t>(out.w) * out.h;
	out.z_units.assign(size, 0xffff);
	out.normal.assign(size, 0);
	if (size == 0) {
		return;
	}
	// The opaque pixels, with the same skip rule as record_spans.
	std::vector<uint8_t> mask(size, 0);
	int                  top = h, bottom = -1;
	For_rle_pixels(rle, xleft, yabove, [&](int x, int y, int val) {
		if (x < 0 || y < 0 || x >= w || y >= h || (val >= skip_from && val <= 0xfe)) {
			return;
		}
		mask[static_cast<size_t>(y) * w + x] = 1;
		top                                  = std::min(top, y);
		bottom                               = std::max(bottom, y);
	});
	if (bottom < 0) {
		return;
	}
	auto drawn = [&](int x, int y) {
		return x >= 0 && y >= 0 && x < w && y < h && mask[static_cast<size_t>(y) * w + x] != 0;
	};
	// Close the gaps in the art (leaves, hair, armour) with a 2 pixel
	// closing (dilate, then erode), so the bulge follows the outline and
	// not every hole in it.  The margin keeps the grid's edge (which the
	// transform counts as a source) away from the art.
	std::vector<uint8_t> closed(size, 0);
	{
		constexpr int    m  = 6;
		const int        cw = w + 2 * m;
		const int        ch = h + 2 * m;
		std::vector<int> grow(static_cast<size_t>(cw) * ch);
		for (int y = 0; y < ch; y++) {
			for (int x = 0; x < cw; x++) {
				grow[static_cast<size_t>(y) * cw + x] = drawn(x - m, y - m) ? 0 : 1 << 28;
			}
		}
		Chamfer_distance(grow, cw, ch);    // 3 x each cell's distance to the art.
		for (int& v : grow) {
			v = v <= 6 ? 1 << 28 : 0;    // Dilated: within 2 px of the art.
		}
		Chamfer_distance(grow, cw, ch);    // 3 x distance out of the dilated set.
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				closed[static_cast<size_t>(y) * w + x]
						= drawn(x, y) || grow[static_cast<size_t>(y + m) * cw + x + m] > 6 ? 1 : 0;
			}
		}
	}
	auto opaque = [&](int x, int y) {
		return x >= 0 && y >= 0 && x < w && y < h && closed[static_cast<size_t>(y) * w + x] != 0;
	};
	// A tree's crown is the top 60 % of its rows; the rest is its trunk.
	const bool tree  = kind == Shaded_volume::tree;
	const int  split = tree ? top + (6 * (bottom - top + 1) + 9) / 10 : top;
	// Grids pad rows above and below the art: a column whose art reaches
	// the bottom row goes on down (feet and trunks stand on the ground, so
	// they get no bottom edge), and a trunk goes on up into its crown.
	constexpr int pad   = 32;
	const int     gh    = h + 2 * pad;
	const auto    gsize = static_cast<size_t>(w) * gh;
	std::vector<uint8_t> grounded(static_cast<size_t>(w), 0);
	for (int x = 0; x < w; x++) {
		grounded[static_cast<size_t>(x)] = opaque(x, bottom) ? 1 : 0;
	}
	// One part (body or crown): its distance grid as depths towards the
	// viewer, w = Rw * sqrt(1 - (1 - min(d / R, 1))^2).
	struct Part {
		std::vector<float> depth;
		float              r = 2.0f;    // Pixels: the part's largest inner radius, clamped.
	};

	auto make_part = [&](auto&& inside, float min_rw, int dmax_from) {
		Part             part;
		std::vector<int> d(gsize, 0);
		for (int gy = 0; gy < gh; gy++) {
			for (int x = 0; x < w; x++) {
				d[static_cast<size_t>(gy) * w + x] = inside(x, gy - pad) ? 1 << 28 : 0;
			}
		}
		Chamfer_distance(d, w, gh);
		int dmax = 0;
		for (int y = std::max(dmax_from, 0); y < h; y++) {
			for (int x = 0; x < w; x++) {
				if (inside(x, y) && opaque(x, y)) {
					dmax = std::max(dmax, d[static_cast<size_t>(y + pad) * w + x]);
				}
			}
		}
		part.r         = std::clamp(static_cast<float>(dmax) / 3.0f, 2.0f, 32.0f);
		const float rw = std::max(part.r, min_rw);
		part.depth.assign(gsize, 0.0f);
		for (size_t i = 0; i < gsize; i++) {
			if (d[i] != 0) {
				const float t = 1.0f - std::min(static_cast<float>(d[i]) / 3.0f / part.r, 1.0f);
				part.depth[i] = rw * std::sqrt(std::max(1.0f - t * t, 0.0f));
			}
		}
		return part;
	};
	const float box_rw = 0.5f * static_cast<float>(std::min(xpx, ypx));
	// The body: everything but a tree's crown.
	const Part body = make_part(
			[&](int x, int y) {
				if (y > bottom) {
					return grounded[static_cast<size_t>(x)] != 0;
				}
				if (tree && y < split) {
					return opaque(x, split);    // The trunk goes on up.
				}
				return opaque(x, y);
			},
			tree ? 0.0f : box_rw, tree ? bottom - (bottom - top + 1) / 5 : top);
	Part crown;
	if (tree) {
		crown = make_part(
				[&](int x, int y) {
					return y < split && opaque(x, y);
				},
				box_rw, top);
		out.extent.trunk_radius = body.r;
	}
	// Screen right and down, and the view direction, in world axes.
	const float  inv6 = 1.0f / std::sqrt(6.0f);
	const float  inv3 = 1.0f / std::sqrt(3.0f);
	const float  rv[3] = {2.0f * inv6, -inv6, -inv6};
	const float  dv3[3] = {-inv6, 2.0f * inv6, -inv6};
	const float  view[3] = {inv3, inv3, inv3};
	const float  root2   = std::sqrt(2.0f);
	for (int y = top; y <= bottom; y++) {
		for (int x = 0; x < w; x++) {
			if (!drawn(x, y)) {
				continue;
			}
			const bool  in_crown = tree && y < split;
			const Part& part     = in_crown ? crown : body;
			auto        depth    = [&](int px, int py) {
                const int gy = py + pad;
                return px < 0 || px >= w || gy < 0 || gy >= gh ? 0.0f : part.depth[static_cast<size_t>(gy) * w + px];
			};
			const float wd = depth(x, y);
			// Height from the depth: the point wd towards the viewer (along
			// (1, 1, 0)) that projects to this pixel.
			const float du = static_cast<float>(x - xleft) - axu;
			const float dv = static_cast<float>(y - yabove) - axv;
			const float z  = std::max((root2 * wd - (du + dv)) * 0.5f, 0.0f);
			const auto  i  = static_cast<size_t>(y) * w + x;
			out.z_units[i] = static_cast<uint16_t>(std::min(std::lround(z * 4.0f), 0xfffeL));
			(in_crown ? out.extent.crown : out.extent.body).add(du + z, dv + z, z);
			// Normal from the depth's slope across the screen: in the middle
			// it faces the viewer, at the silhouette's edge it turns sideways.
			const float sx  = -(depth(x + 1, y) - depth(x - 1, y)) * 0.5f;
			const float sy  = -(depth(x, y + 1) - depth(x, y - 1)) * 0.5f;
			const float f   = 1.0f / std::sqrt(1.0f + sx * sx + sy * sy);
			const float gx  = sx * f;
			const float gy  = sy * f;
			const float vz  = std::sqrt(std::max(1.0f - gx * gx - gy * gy, 0.0f));
			float       n[3];
			for (int k = 0; k < 3; k++) {
				n[k] = view[k] * vz + rv[k] * gx + dv3[k] * gy;
			}
			// People, pillars and trunks stand upright: their sides face out
			// more than up.
			if (!in_crown && kind != Shaded_volume::round) {
				n[2] *= 0.5f;
			}
			float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			n[2]      = std::max(n[2] / len, -0.2f);
			n[0] /= len;
			n[1] /= len;
			len           = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			out.normal[i] = Shaded_encode_normal(n[0] / len, n[1] / len, n[2] / len);
		}
	}
}

void Shaded_cpu::record_rounded(
		Shaded_scene_buffers& buf, const Shaded_rect& clip, const Shaded_object_context& obj, const Shaded_silhouette& sil,
		const unsigned char* rle, int xoff, int yoff, int skip_from, long& written) {
	const int right  = clip.x + clip.w;
	const int bottom = clip.y + clip.h;
	const int mx0    = sil.xleft - xoff;    // Mask x of screen x 0.
	const int my0    = sil.yabove - yoff;
	For_rle_pixels(rle, xoff, yoff, [&](int x, int y, int val) {
		if (x < clip.x || x >= right || y < clip.y || y >= bottom || (val >= skip_from && val <= 0xfe)) {
			return;
		}
		const int mx = x + mx0;
		const int my = y + my0;
		if (mx < 0 || my < 0 || mx >= sil.w || my >= sil.h) {
			return;
		}
		const auto     m  = static_cast<size_t>(my) * sil.w + mx;
		const uint16_t zu = sil.z_units[m];
		if (zu == 0xffff) {
			return;
		}
		const int idx          = buf.index(x, y);
		buf.height_map[idx]    = static_cast<uint16_t>(std::min(obj.base_height + zu, 0xffff));
		buf.facing_map[idx]    = shaded_face_rounded;
		buf.normal_map[idx]    = sil.normal[m];
		buf.object_ids[idx]    = obj.id;
		written++;
	});
}

void Shaded_cpu::merge_roof_seams(Shaded_scene_buffers& buf, const Shaded_rect& r, long& merged) {
	const Shaded_rect area  = buf.bounds();
	const int         count = static_cast<int>(std::min(buf.objects.size(), buf.object_flags.size()));
	auto              roof  = [&](int id) {
        return id != shaded_id_none && id != shaded_id_overflow && id <= count
               && (buf.object_flags[id - 1] & shaded_object_roof) != 0;
	};
	auto inside = [&](int x, int y) {
		return x >= area.x && y >= area.y && x < area.x + area.w && y < area.y + area.h;
	};
	// Decided on the record as painted, then written.
	std::vector<std::pair<int, uint16_t>> seams;
	for (int y = r.y; y < r.y + r.h; y++) {
		for (int x = r.x; x < r.x + r.w; x++) {
			const int     idx  = buf.index(x, y);
			const uint8_t face = buf.facing_map[idx] & shaded_face_mask;
			const int     id   = buf.object_ids[idx];
			if ((face != shaded_face_south && face != shaded_face_east) || !roof(id)) {
				continue;
			}
			// A south face runs down the screen (+y) from the slab's top
			// edge, an east face across it (+x).
			const int sx = face == shaded_face_east ? 1 : 0;
			const int sy = 1 - sx;
			auto      same = [&](int k) {
                const int px = x + sx * k;
                const int py = y + sy * k;
                if (!inside(px, py)) {
                    return false;
                }
                const int i = buf.index(px, py);
                return (buf.facing_map[i] & shaded_face_mask) == face && buf.object_ids[i] == id;
			};
			int before = 0, after = 0;
			while (before < 5 && same(-(before + 1))) {
				before++;
			}
			while (after < 5 && same(after + 1)) {
				after++;
			}
			if (before + after + 1 > 4) {
				continue;    // A real wall of the slab, not a seam.
			}
			const int px = x + sx * (after + 1);
			const int py = y + sy * (after + 1);
			if (!inside(px, py)) {
				continue;
			}
			const int                   next  = buf.index(px, py);
			const int                   other = buf.object_ids[next];
			const Shaded_object_record& o     = buf.objects[id - 1];
			const int                   top   = std::min((o.tz + o.ztiles) * shaded_height_units_per_lift, 0xffff);
			if ((buf.facing_map[next] & shaded_face_mask) == shaded_face_top && other != id && roof(other)
				&& std::abs(buf.height_map[next] - top) <= 4) {
				seams.emplace_back(idx, static_cast<uint16_t>(top));
			}
		}
	}
	for (const auto& [idx, top] : seams) {
		buf.facing_map[idx] = shaded_face_top | shaded_face_merged;
		buf.height_map[idx] = top;
	}
	merged += static_cast<long>(seams.size());
}

int Shaded_cpu::view_code(const Shaded_scene_buffers& buf, const Shaded_light_params& params, int index) {
	const uint8_t facing = buf.facing_map[index];
	switch (static_cast<Shaded_view>(params.view)) {
	case Shaded_view::height: {
		if ((facing & shaded_face_mask) == shaded_face_none) {
			return 0;
		}
		const float lifts = static_cast<float>(buf.height_map[index]) / params.height_units_per_lift;
		const int   code  = 1 + static_cast<int>(lifts / params.debug_height_range * (lut_size - 1));
		return std::min(code, lut_size - 1);
	}
	case Shaded_view::facing:
		return facing & (shaded_face_mask | shaded_face_clamped);
	case Shaded_view::object_id: {
		const unsigned id = buf.object_ids[index];
		if (id == shaded_id_none) {
			return 0;
		}
		return 1 + static_cast<int>((id * 2654435761u >> 16) % (lut_size - 1));
	}
	case Shaded_view::light:
		return 1 + buf.light_levels[index] * (lut_size - 2) / std::max(params.level_count - 1, 1);
	case Shaded_view::shadow:
		return 1 + buf.shadow_levels[index] * (lut_size - 2) / 255;
	case Shaded_view::normals: {
		// 4 levels per channel: code = 16 r + 4 g + b.
		const uint8_t face = facing & shaded_face_mask;
		if (face == shaded_face_none) {
			return 0;
		}
		const Shaded_normalf n = Shaded_pixel_normal(buf, index, face);
		auto                 q = [](float c) {
            return std::clamp(static_cast<int>((c * 0.5f + 0.5f) * 4.0f), 0, 3);
		};
		return std::max(16 * q(n.x) + 4 * q(n.y) + q(n.z), 1);
	}
	case Shaded_view::weather: {
		// 0 covered, 1..47 sky factor, 48..62 fog, 63 flashed.
		if (buf.flash_levels.size() > static_cast<size_t>(index) && buf.flash_levels[index] > 3) {
			return lut_size - 1;
		}
		if (buf.fog_codes.size() > static_cast<size_t>(index) && (buf.fog_codes[index] & 7) != 0) {
			return 48 + std::min((buf.fog_codes[index] & 7) * 2 - 1, 14);
		}
		if (buf.sky_levels.size() <= static_cast<size_t>(index) || buf.sky_levels[index] == 0) {
			return 0;
		}
		return 1 + (buf.sky_levels[index] - 1) * 46 / 254;
	}
	case Shaded_view::normal:
	case Shaded_view::unlit:
	case Shaded_view::count:
		break;
	}
	return 0;
}

bool Shaded_cpu::light_screen_span(
		const Shaded_light_params& params, const Shaded_light_record& light, float& x0, float& y0, float& x1, float& y1) {
	const float r = light.radius * params.radius_scale;
	if (r <= 0.0f || (light.flags & (shaded_light_off | shaded_light_directional))) {
		return false;
	}
	const auto tiles = static_cast<float>(c_num_tiles);
	auto       wrap  = [tiles](float d) {
		d = std::fmod(d, tiles);
		if (d > tiles * 0.5f) {
			d -= tiles;
		} else if (d < -tiles * 0.5f) {
			d += tiles;
		}
		return d;
	};
	// Heights the light can reach: within R in 3D, or the area band.
	float zlo, zhi;
	if (light.flags & shaded_light_area) {
		zlo = light.z - 1.0f;
		zhi = light.z + params.area_height;
	} else {
		zlo = light.z - r / params.tiles_per_lift;
		zhi = light.z + r / params.tiles_per_lift;
	}
	zlo = std::max(zlo, 0.0f);
	zhi = std::max(zhi, zlo);
	// Pixel range covered by world x (or y) in [lo, hi] tiles from the
	// scroll tile, at heights zlo..zhi lifts: px = 8 * w - scroll_px - 0.5 - 4 * z.
	auto cover = [&](float lo, float hi, int scroll_px, float& p0, float& p1) {
		p0 = lo * params.pixels_per_tile - static_cast<float>(scroll_px) - 0.5f - zhi * params.pixels_per_lift;
		p1 = hi * params.pixels_per_tile - static_cast<float>(scroll_px) - 0.5f - zlo * params.pixels_per_lift;
	};
	const float dx = wrap(light.x - static_cast<float>(params.scroll_tx));
	const float dy = wrap(light.y - static_cast<float>(params.scroll_ty));
	cover(dx - r, dx + r, params.scroll_px, x0, x1);
	cover(dy - r, dy + r, params.scroll_py, y0, y1);
	return true;
}

Shaded_rect Shaded_cpu::light_screen_rect(const Shaded_light_params& params, const Shaded_light_record& light, int w, int h) {
	float x0, y0, x1, y1;
	if (!light_screen_span(params, light, x0, y0, x1, y1)) {
		return Shaded_rect{};
	}
	// Whole pixels, clamped first so far lights cannot overflow an int.
	const auto lo_x = static_cast<int>(std::floor(std::clamp(x0, -1.0f, static_cast<float>(w))));
	const auto lo_y = static_cast<int>(std::floor(std::clamp(y0, -1.0f, static_cast<float>(h))));
	const auto hi_x = static_cast<int>(std::floor(std::clamp(x1, -1.0f, static_cast<float>(w))));
	const auto hi_y = static_cast<int>(std::floor(std::clamp(y1, -1.0f, static_cast<float>(h))));
	return Intersect(Shaded_rect{lo_x, lo_y, hi_x - lo_x + 1, hi_y - lo_y + 1}, Shaded_rect{0, 0, w, h});
}

void Shaded_cpu::build_bins(
		const Shaded_light_params& params, const std::vector<Shaded_light_record>& lights, std::vector<uint32_t>& offsets,
		std::vector<uint16_t>& list) {
	const int cells = std::max(params.bins_x, 0) * std::max(params.bins_y, 0);
	offsets.assign(static_cast<size_t>(cells) + 1, 0);
	list.clear();
	if (cells <= 0) {
		return;
	}
	// Bins each light covers: x0, y0, x1, y1 inclusive (x1 < x0: none).
	struct Span {
		int x0, y0, x1, y1;
	};

	std::vector<Span> spans(lights.size(), Span{0, 0, -1, -1});
	const auto        size  = static_cast<float>(params.bin_size);
	auto              cover = [size](float p0, float p1, int count, int& c0, int& c1) {
		c0 = std::max(static_cast<int>(std::floor(p0 / size)), 0);
		c1 = std::min(static_cast<int>(std::floor(p1 / size)), count - 1);
	};
	for (size_t i = 0; i < lights.size(); i++) {
		float px0, py0, px1, py1;
		if (!light_screen_span(params, lights[i], px0, py0, px1, py1)) {
			continue;
		}
		Span& span = spans[i];
		cover(px0, px1, params.bins_x, span.x0, span.x1);
		cover(py0, py1, params.bins_y, span.y0, span.y1);
		for (int y = span.y0; y <= span.y1; y++) {
			for (int x = span.x0; x <= span.x1; x++) {
				offsets[static_cast<size_t>(y * params.bins_x + x) + 1]++;
			}
		}
	}
	for (int c = 0; c < cells; c++) {
		offsets[static_cast<size_t>(c) + 1] += offsets[static_cast<size_t>(c)];
	}
	list.resize(offsets[static_cast<size_t>(cells)]);
	std::vector<uint32_t> fill(offsets.begin(), offsets.end() - 1);
	for (size_t i = 0; i < lights.size(); i++) {
		const Span& span = spans[i];
		for (int y = span.y0; y <= span.y1; y++) {
			for (int x = span.x0; x <= span.x1; x++) {
				list[fill[static_cast<size_t>(y * params.bins_x + x)]++] = static_cast<uint16_t>(i);
			}
		}
	}
}

namespace {
	int Floor_div(int a, int b) {
		const int q = a / b;
		return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
	}

	// Buffer index of the receiver pixel of screen map cell (cx, cy).
	inline int Cell_pixel(const Shaded_scene_buffers& buf, const Shaded_shadow_set& set, int cx, int cy) {
		const int half = set.resolution / 2;
		const int x    = std::clamp(cx * set.resolution + half, 0, std::max(set.area_w - 1, 0));
		const int y    = std::clamp(cy * set.resolution + half, 0, std::max(set.area_h - 1, 0));
		return buf.index(x, y);
	}

	// World position of a recorded pixel as Shaded_cpu::light_rows has it
	// (unwrapped, from the scroll tile).
	inline Shaded_point Pixel_world(const Shaded_scene_buffers& buf, const Shaded_light_params& params, int x, int y) {
		const float wz = static_cast<float>(buf.height_map[buf.index(x, y)]) / params.height_units_per_lift;
		return Shaded_point{Shaded_unproject(x, wz, params.scroll_tx, params.scroll_px, params.pixels_per_tile, params.pixels_per_lift),
							Shaded_unproject(y, wz, params.scroll_ty, params.scroll_py, params.pixels_per_tile, params.pixels_per_lift),
							wz};
	}

	// Unshadowed light of l at p on a face with normal n (no self-light),
	// as Shaded_cpu::light_rows adds it.
	float Unshadowed(const Shaded_light_params& params, const Shaded_light_record& l, const Shaded_point& p, const Shaded_normalf& n) {
		const float radius = l.radius * params.radius_scale;
		const float r2     = radius * radius;
		const float dx     = Shaded_wrap_delta(l.x - p.x);
		const float dy     = Shaded_wrap_delta(l.y - p.y);
		const float h2     = dx * dx + dy * dy;
		if ((l.flags & (shaded_light_off | shaded_light_directional)) || h2 >= r2) {
			return 0.0f;
		}
		if (l.flags & shaded_light_area) {
			return p.z >= l.z - 1.0f && p.z <= l.z + params.area_height ? l.intensity * Shaded_falloff(h2 / r2) : 0.0f;
		}
		const float dz = (l.z - p.z) * params.tiles_per_lift;
		const float d2 = h2 + dz * dz;
		if (d2 >= r2) {
			return 0.0f;
		}
		const float dot   = n.x * dx + n.y * dy + n.z * dz;
		const float ndotl = d2 > 1e-6f ? dot / std::sqrt(d2) : 1.0f;
		if (ndotl <= 0.0f) {
			return 0.0f;
		}
		return l.intensity * Shaded_falloff(d2 / r2) * (params.facing_min + (1.0f - params.facing_min) * ndotl);
	}

	/*
	 *  Screen-space transmittance from receiver p (record id own) to target:
	 *  samples along the way are projected into the frame, and the box of
	 *  the object drawn there (not the receiver's or the light's own) is
	 *  tested with a cone as wide as the light seen from p.
	 */
	float Screen_transmittance(
			const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_shadow_set& set,
			const Shaded_point& p, const Shaded_point& target, int own, int light_id) {
		const float dx = target.x - p.x;
		const float dy = target.y - p.y;
		const float dz = target.z - p.z;    // Lifts.
		const float dt = dz * params.tiles_per_lift;
		const float dist = std::sqrt(dx * dx + dy * dy + dt * dt);
		const float bias = params.shadow_bias;
		if (dist <= bias) {
			return 1.0f;
		}
		// Samples: one per 2 screen pixels, within [4, shadow_steps].
		const float sx    = dx * params.pixels_per_tile - dz * params.pixels_per_lift;
		const float sy    = dy * params.pixels_per_tile - dz * params.pixels_per_lift;
		const int   steps = std::clamp(static_cast<int>(std::sqrt(sx * sx + sy * sy) * 0.5f), 4, std::max(params.shadow_steps, 4));
		const float size  = 0.5f * params.shadow_softness;    // Light size, tiles.
		const bool  hard  = params.shadow_softness <= 0.0f;
		const int   count = static_cast<int>(buf.objects.size());
		float       ts    = 1.0f;
		for (int k = 0; k < steps; k++) {
			const float s  = bias + (dist - bias) * (static_cast<float>(k) + 0.5f) / static_cast<float>(steps);
			const float t  = s / dist;
			const float qx = p.x + dx * t;
			const float qy = p.y + dy * t;
			const float qz = p.z + dz * t;
			const float fx = Shaded_project(qx, qz, params.scroll_tx, params.scroll_px, params.pixels_per_tile, params.pixels_per_lift);
			const float fy = Shaded_project(qy, qz, params.scroll_ty, params.scroll_py, params.pixels_per_tile, params.pixels_per_lift);
			const int   px = static_cast<int>(std::floor(fx + 0.5f));
			const int   py = static_cast<int>(std::floor(fy + 0.5f));
			if (px < 0 || py < 0 || px >= set.area_w || py >= set.area_h) {
				break;    // Off screen: the grid covers the rest.
			}
			const int id = buf.object_ids[buf.index(px, py)];
			if (id == shaded_id_none || id == shaded_id_overflow || id == own || id == light_id || id > count) {
				continue;
			}
			const uint8_t opacity = buf.object_opacity[id - 1];
			if (opacity == 0) {
				continue;
			}
			// A rounded object's fitted volume, relative to its centre.
			const Shaded_object_volume& rv = buf.object_volumes[id - 1];
			if (params.rounded && rv.kind != 0) {
				const float rq[3] = {Shaded_wrap_delta(qx - rv.centre[0]), Shaded_wrap_delta(qy - rv.centre[1]),
									 (qz - rv.centre[2]) * params.tiles_per_lift};
				const float v     = Shaded_cone_visibility(Shaded_rounded_sd(rq, rv, params.tiles_per_lift), size * s / dist, hard);
				ts                = std::min(ts, 1.0f - static_cast<float>(opacity) / 255.0f * (1.0f - v));
				if (ts <= 0.0f) {
					break;
				}
				continue;
			}
			// The object's box, relative to its SE bottom corner, in tiles.
			const Shaded_object_record& o     = buf.objects[id - 1];
			const float                 q[3]  = {Shaded_wrap_delta(qx - static_cast<float>(o.tx + 1)),
												 Shaded_wrap_delta(qy - static_cast<float>(o.ty + 1)),
												 (qz - static_cast<float>(o.tz)) * params.tiles_per_lift};
			const float                 lo[3] = {-static_cast<float>(o.xtiles), -static_cast<float>(o.ytiles), 0.0f};
			const float                 hi[3] = {0.0f, 0.0f, static_cast<float>(o.ztiles) * params.tiles_per_lift};
			const Shaded_box_distance   bd    = Shaded_box_sd(q, lo, hi);
			const float                 sd    = bd.outside2 > 0.0f ? std::sqrt(bd.outside2) : bd.inside;
			const float                 v     = Shaded_cone_visibility(sd, size * s / dist, hard);
			ts = std::min(ts, 1.0f - static_cast<float>(opacity) / 255.0f * (1.0f - v));
			if (ts <= 0.0f) {
				break;
			}
		}
		return ts;
	}
}    // namespace

float Shaded_cpu::grid_transmittance(
		const Shaded_grid_volume& vol, const Shaded_light_record& light, const Shaded_light_box& box, const Shaded_point& p) {
	Shaded_point t = Shaded_shadow_target(light, p);
	if (box.valid) {
		const float lo[3] = {p.x + Shaded_wrap_delta(box.lo[0] - p.x), p.y + Shaded_wrap_delta(box.lo[1] - p.y), box.lo[2]};
		const float hi[3] = {lo[0] + (box.hi[0] - box.lo[0]), lo[1] + (box.hi[1] - box.lo[1]), box.hi[2]};
		t                 = Shaded_clip_to_box(p, t, lo, hi, 0.01f);
		if (t.x == p.x && t.y == p.y && t.z == p.z) {
			return 1.0f;    // In the light's own box.
		}
		// The end cell is skipped below, but it is in front of the light
		// now: a solid one (the wall behind a sconce) stops the light.
		int       x, y;
		const int z = static_cast<int>(std::floor(t.z));
		const bool own_cell = std::floor(t.x) == std::floor(p.x) && std::floor(t.y) == std::floor(p.y)
							  && std::floor(t.z) == std::floor(p.z);
		if (!own_cell && vol.local(static_cast<int>(std::floor(t.x)), static_cast<int>(std::floor(t.y)), x, y)
			&& vol.inside(x, y, z) && vol.opacity(x, y, z) == 255) {
			return 0.0f;
		}
	}
	// The end cells are skipped: a receiver's pixel centre can lie just
	// inside its own box.
	const Shaded_grid_ray ray{
			{p.x, p.y, p.z, 0.0f},
			{t.x, t.y, t.z, 0.0f}
	};
	return Shaded_grid_cpu::trace(vol, ray, 0).transmittance;
}

void Shaded_cpu::grid_field_rows(
		const Shaded_grid_volume& vol, const Shaded_light_params& params, const Shaded_light_record& light,
		const Shaded_light_box& box, const Shaded_shadow_field& field, int y0, int y1, uint8_t* data) {
	ignore_unused_variable_warning(params);
	for (int y = y0; y < y1; y++) {
		const int ly = field.y + y;
		for (int z = 0; z < field.d; z++) {
			const int lz  = field.z + z;
			uint8_t*  out = data + field.offset + (static_cast<size_t>(z) * field.h + y) * field.w;
			for (int x = 0; x < field.w; x++) {
				const int lx = field.x + x;
				if (vol.opacity(lx, ly, lz) == 255) {
					out[x] = shaded_field_solid;
					continue;
				}
				const Shaded_point c{static_cast<float>(vol.origin_tx + lx) + 0.5f, static_cast<float>(vol.origin_ty + ly) + 0.5f,
									 static_cast<float>(lz) + 0.5f};
				const float        tr = grid_transmittance(vol, light, box, c);
				out[x]                = static_cast<uint8_t>(std::lround(std::clamp(tr, 0.0f, 1.0f) * shaded_field_scale));
			}
		}
	}
}

void Shaded_cpu::screen_shadow_rows(
		const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_light_record* lights,
		const uint32_t* bin_offsets, const uint16_t* bin_lights, const Shaded_grid_volume* ray_grid, Shaded_shadow_set& set,
		const Shaded_rect& cells, int y0, int y1, long& traces) {
	if (params.bins_x <= 0 || params.bins_y <= 0) {
		return;
	}
	struct Candidate {
		float   light;    // Unshadowed.
		float   value;    // Transmittance so far.
		int     map;
		bool    taken;
	};

	Candidate  cand[shaded_max_lights];
	const int  top_k = std::clamp(params.shadow_lights, 1, 16);
	for (int cy = y0; cy < y1; cy++) {
		for (int cx = cells.x; cx < cells.x + cells.w; cx++) {
			const int     idx  = Cell_pixel(buf, set, cx, cy);
			const uint8_t face = buf.facing_map[idx] & shaded_face_mask;
			if (face == shaded_face_none) {
				continue;    // Maps hold 1.
			}
			const int          row = (idx - buf.origin) / buf.stride;
			const int          col = idx - buf.origin - row * buf.stride;
			const Shaded_point p   = Pixel_world(buf, params, col, row);
			const int          own = buf.object_ids[idx];
			const Shaded_normalf n = Shaded_pixel_normal(buf, idx, face);
			const int          bin = std::clamp(row / params.bin_size, 0, params.bins_y - 1) * params.bins_x
							+ std::clamp(col / params.bin_size, 0, params.bins_x - 1);
			int   count = 0;
			float best  = 0.0f;
			for (uint32_t k = bin_offsets[bin]; k < bin_offsets[bin + 1]; k++) {
				const int li = bin_lights[k];
				const int m  = set.light_map[li];
				if (m < 0) {
					continue;
				}
				const Shaded_shadow_map&   map = set.maps[m];
				const Shaded_light_record& l   = lights[li];
				if (cx < map.x || cy < map.y || cx >= map.x + map.w || cy >= map.y + map.h
					|| (own != shaded_id_none && l.object_id == own)) {
					continue;    // Self-light is never shadowed.
				}
				const float c = Unshadowed(params, l, p, n);
				if (c <= 0.0f) {
					continue;
				}
				float value = 1.0f;
				if (ray_grid && map.build) {    // Tg for every light.
					value = grid_transmittance(*ray_grid, l, set.light_boxes[li], p);
				}
				cand[count++] = Candidate{c, value, m, false};
				best          = std::max(best, c);
			}
			// The strongest lights get the screen-space trace.
			for (int round = 0; set.screen && round < top_k; round++) {
				int pick = -1;
				for (int i = 0; i < count; i++) {
					if (!cand[i].taken && (pick < 0 || cand[i].light > cand[pick].light)) {
						pick = i;
					}
				}
				if (pick < 0 || cand[pick].light < best / 64.0f) {
					break;
				}
				cand[pick].taken = true;
				const Shaded_shadow_map& map = set.maps[cand[pick].map];
				if (!map.build) {
					continue;
				}
				const Shaded_light_record& l = lights[map.light];
				cand[pick].value *= Screen_transmittance(buf, params, set, p, Shaded_shadow_target(l, p), own, l.object_id);
				traces++;
			}
			for (int i = 0; i < count; i++) {
				const Shaded_shadow_map& map = set.maps[cand[i].map];
				if (map.build) {
					set.map_data[map.offset + static_cast<size_t>(cy - map.y) * map.w + (cx - map.x)]
							= static_cast<uint8_t>(std::lround(std::clamp(cand[i].value, 0.0f, 1.0f) * 255.0f));
				}
			}
		}
	}
}

void Shaded_cpu::flash_shadow_rows(
		const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_grid_volume* vol,
		const Shaded_light_record& flash, Shaded_shadow_set& set, const Shaded_rect& cells, int y0, int y1, long& traces) {
	if (set.maps.empty() || flash.z <= 0.0f) {
		return;
	}
	const Shaded_shadow_map& map = set.maps[0];
	for (int cy = y0; cy < y1; cy++) {
		uint8_t* out = set.map_data.data() + map.offset + static_cast<size_t>(cy - map.y) * map.w;
		for (int cx = cells.x; cx < cells.x + cells.w; cx++) {
			const int idx = Cell_pixel(buf, set, cx, cy);
			if ((buf.facing_map[idx] & shaded_face_mask) == shaded_face_none) {
				out[cx - map.x] = 255;
				continue;
			}
			const int          row = (idx - buf.origin) / buf.stride;
			const int          col = idx - buf.origin - row * buf.stride;
			const Shaded_point p   = Pixel_world(buf, params, col, row);
			float              t   = 1.0f;
			if (p.z < shaded_flash_top) {
				// Along the flash's direction up to the top of the grid.
				const float        s = (shaded_flash_top - p.z) / flash.z;
				const Shaded_point target{p.x + flash.x * s, p.y + flash.y * s, shaded_flash_top};
				t = Screen_transmittance(buf, params, set, p, target, buf.object_ids[idx], 0);
				traces++;
				if (vol && t > 0.0f) {
					t *= flash_grid_transmittance(*vol, p, target);
				}
			}
			out[cx - map.x] = static_cast<uint8_t>(std::lround(std::clamp(t, 0.0f, 1.0f) * 255.0f));
		}
	}
}

void Shaded_cpu::blur_shadow_rows(
		const Shaded_scene_buffers& buf, const Shaded_shadow_set& set, const Shaded_shadow_map& map, int y0, int y1,
		uint8_t* out) {
	const uint8_t* in = set.map_data.data() + map.offset;
	out += map.offset;
	for (int j = y0; j < y1; j++) {
		for (int i = 0; i < map.w; i++) {
			const size_t at  = static_cast<size_t>(j) * map.w + i;
			const int    idx = Cell_pixel(buf, set, map.x + i, map.y + j);
			if ((buf.facing_map[idx] & shaded_face_mask) == shaded_face_none) {
				out[at] = in[at];
				continue;
			}
			// 1-2-1 weights, neighbours more than a lift away left out.
			const int h0   = buf.height_map[idx];
			int       wsum = 0;
			int       vsum = 0;
			for (int dj = -1; dj <= 1; dj++) {
				for (int di = -1; di <= 1; di++) {
					const int ni = i + di;
					const int nj = j + dj;
					if (ni < 0 || nj < 0 || ni >= map.w || nj >= map.h) {
						continue;
					}
					const int nidx = Cell_pixel(buf, set, map.x + ni, map.y + nj);
					if ((buf.facing_map[nidx] & shaded_face_mask) == shaded_face_none
						|| std::abs(buf.height_map[nidx] - h0) > shaded_height_units_per_lift) {
						continue;
					}
					const int w = (di ? 1 : 2) * (dj ? 1 : 2);
					wsum += w;
					vsum += w * in[static_cast<size_t>(nj) * map.w + ni];
				}
			}
			out[at] = static_cast<uint8_t>(wsum > 0 ? (vsum + wsum / 2) / wsum : in[at]);
		}
	}
}

float Shaded_cpu::shadow_screen_at(
		const Shaded_scene_buffers& buf, const Shaded_shadow_set& set, const Shaded_shadow_map& map, int x, int y, int height) {
	const int      res  = set.resolution;
	const int      nx   = x - res / 2;
	const int      ny   = y - res / 2;
	const int      i0   = Floor_div(nx, res);
	const int      j0   = Floor_div(ny, res);
	const float    fx   = static_cast<float>(nx - i0 * res) / static_cast<float>(res);
	const float    fy   = static_cast<float>(ny - j0 * res) / static_cast<float>(res);
	const uint8_t* data = set.map_data.data() + map.offset;
	float          wsum = 0.0f;
	float          vsum = 0.0f;
	for (int dj = 0; dj < 2; dj++) {
		for (int di = 0; di < 2; di++) {
			float w = (di ? fx : 1.0f - fx) * (dj ? fy : 1.0f - fy);
			if (w <= 0.0f) {
				continue;
			}
			const int ci = std::clamp(i0 + di, map.x, map.x + map.w - 1);
			const int cj = std::clamp(j0 + dj, map.y, map.y + map.h - 1);
			// Joint bilateral: cells on another surface count little.
			if (std::abs(buf.height_map[Cell_pixel(buf, set, ci, cj)] - height) > shaded_height_units_per_lift) {
				w *= 0.05f;
			}
			wsum += w;
			vsum += w * static_cast<float>(data[static_cast<size_t>(cj - map.y) * map.w + (ci - map.x)]);
		}
	}
	return wsum > 0.0f ? vsum / (wsum * 255.0f) : 1.0f;
}

float Shaded_cpu::shadow_grid_at(const Shaded_shadow_set& set, const Shaded_shadow_field& field, const Shaded_point& p) {
	const float u  = std::clamp(Shaded_wrap_delta(p.x - static_cast<float>(set.grid_tx)) - static_cast<float>(field.x) - 0.5f, 0.0f,
								static_cast<float>(field.w - 1));
	const float v  = std::clamp(Shaded_wrap_delta(p.y - static_cast<float>(set.grid_ty)) - static_cast<float>(field.y) - 0.5f, 0.0f,
								static_cast<float>(field.h - 1));
	const float w  = std::clamp(p.z - static_cast<float>(field.z) - 0.5f, 0.0f, static_cast<float>(field.d - 1));
	const int   i0 = static_cast<int>(u);
	const int   j0 = static_cast<int>(v);
	const int   k0 = static_cast<int>(w);
	const int   i1 = std::min(i0 + 1, field.w - 1);
	const int   j1 = std::min(j0 + 1, field.h - 1);
	const int   k1 = std::min(k0 + 1, field.d - 1);
	const uint8_t* data = set.field_data.data() + field.offset;
	auto           at   = [&](int i, int j, int k) {
		return data[(static_cast<size_t>(k) * field.h + j) * field.w + i];
	};
	const uint8_t corners[8] = {at(i0, j0, k0), at(i1, j0, k0), at(i0, j1, k0), at(i1, j1, k0),
								at(i0, j0, k1), at(i1, j0, k1), at(i0, j1, k1), at(i1, j1, k1)};
	return Shaded_solid_trilinear(corners, u - static_cast<float>(i0), v - static_cast<float>(j0), w - static_cast<float>(k0));
}

void Shaded_cpu::light_rows(
		const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_light_record* lights,
		const uint32_t* bin_offsets, const uint16_t* bin_lights, const Shaded_rect& r, int y0, int y1, uint8_t* levels,
		const Shaded_shadow_set* shadows, uint8_t* shadow_out, const Shaded_weather_set* weather,
		const Shaded_weather_out* wout) {
	if (params.bins_x <= 0 || params.bins_y <= 0) {
		return;
	}
	const int   top_level = std::max(params.level_count - 1, 1);
	const auto  tiles     = static_cast<float>(c_num_tiles);
	const float ambient   = params.ambient_level;
	const float inv_tile  = 1.0f / params.pixels_per_tile;
	const float inv_units = 1.0f / params.height_units_per_lift;
	const float lift_tile = params.pixels_per_lift * inv_tile;    // Tiles per lift of screen shift.
	const float inv_soft  = 1.0f / std::max(params.softness, 0.01f);
	const float facing    = params.facing_min;
	uint8_t*    sky_out   = weather && wout ? wout->sky : nullptr;
	uint8_t*    fog_out   = weather && wout ? wout->fog : nullptr;    // Codes 0 without fog.
	uint8_t*    flash_out = weather && wout ? wout->flash : nullptr;    // Levels 0 without a flash.
	// A lightning flash: its shadow map and unit direction in tiles.
	const Shaded_shadow_map* flash_map = weather && params.flash > 0.0f && weather->flash_map >= 0
														 && static_cast<size_t>(weather->flash_map) < weather->flash.maps.size()
												 ? &weather->flash.maps[static_cast<size_t>(weather->flash_map)]
												 : nullptr;
	// Rain and snow painted over the frame (1 per marked pixel).
	const uint8_t* particles = weather && params.precipitation ? weather->particles : nullptr;
	float flash_l[3] = {params.flash_x, params.flash_y, params.flash_z * params.tiles_per_lift};
	if (const float len = std::sqrt(flash_l[0] * flash_l[0] + flash_l[1] * flash_l[1] + flash_l[2] * flash_l[2]); len > 0.0f) {
		for (float& c : flash_l) {
			c /= len;
		}
	}
	// Shortest way round the wrapped world.
	auto wrap = [tiles](float d) {
		if (d > tiles * 0.5f) {
			d -= tiles;
		} else if (d < -tiles * 0.5f) {
			d += tiles;
		}
		return d;
	};
	// Level for light l, dithered against world pixels so it does not crawl.
	auto quantise = [&](float l, int x, int y) {
		const float threshold = params.dither ? Shaded_bayer4(x + params.scroll_px, y + params.scroll_py) : 0.5f;
		const int   q         = static_cast<int>(std::floor(l * static_cast<float>(top_level) + threshold));
		return static_cast<uint8_t>(std::clamp(q, 0, top_level));
	};
	// v (0..7) to a whole 0..7, dithered at its own phase (x, y).
	auto quantise7 = [&](float v, int x, int y) {
		const float threshold = params.dither ? Shaded_bayer4(x + params.scroll_px, y + params.scroll_py) : 0.5f;
		return std::clamp(static_cast<int>(std::floor(v + threshold)), 0, 7);
	};
	// The light of bin's lights at a point, as the fog scatters it: point
	// lights by 3D distance, area lights over their band, the light's own
	// object in full; no facing and no shadows.
	auto scatter_sum = [&](int bin, int id, float px, float py, float wz) {
		float s = 0.0f;
		for (uint32_t k = bin_offsets[bin]; k < bin_offsets[bin + 1]; k++) {
			const Shaded_light_record& l = lights[bin_lights[k]];
			if (l.flags & (shaded_light_off | shaded_light_directional)) {
				continue;
			}
			if (id != shaded_id_none && l.object_id == id) {
				s += l.intensity;
				continue;
			}
			const float radius = l.radius * params.radius_scale;
			const float r2     = radius * radius;
			const float dx     = wrap(l.x - px);
			const float dy     = wrap(l.y - py);
			const float h2     = dx * dx + dy * dy;
			if (h2 >= r2) {
				continue;
			}
			if (l.flags & shaded_light_area) {
				if (wz >= l.z - 1.0f && wz <= l.z + params.area_height) {
					s += l.intensity * Shaded_falloff(h2 / r2);
				}
				continue;
			}
			const float dz = (l.z - wz) * params.tiles_per_lift;
			const float d2 = h2 + dz * dz;
			if (d2 < r2) {
				s += l.intensity * Shaded_falloff(d2 / r2);
			}
		}
		return s;
	};
	for (int y = y0; y < y1; y++) {
		const int   row    = buf.index(0, y);
		const int   bin_y  = std::clamp(y / params.bin_size, 0, params.bins_y - 1);
		const float base_x = static_cast<float>(params.scroll_tx) + static_cast<float>(r.x + params.scroll_px) * inv_tile + 0.5f * inv_tile;
		const float base_y = static_cast<float>(params.scroll_ty) + (static_cast<float>(y + params.scroll_py) + 0.5f) * inv_tile;
		for (int x = r.x; x < r.x + r.w; x++) {
			const int     idx  = row + x;
			const uint8_t face = buf.facing_map[idx] & shaded_face_mask;
			if (face == shaded_face_none) {
				levels[idx] = quantise(ambient, x, y);
				if (shadow_out) {
					shadow_out[idx] = 255;
				}
				if (sky_out) {
					sky_out[idx] = 255;
				}
				if (fog_out) {
					fog_out[idx] = 0;
				}
				if (flash_out) {
					flash_out[idx] = 0;
				}
				continue;
			}
			// World position as in Shaded_light::world_position, unwrapped.
			const float         wz   = static_cast<float>(buf.height_map[idx]) * inv_units;
			const float         px   = base_x + static_cast<float>(x - r.x) * inv_tile + wz * lift_tile;
			const float         py   = base_y + wz * lift_tile;
			const Shaded_normalf n   = Shaded_pixel_normal(buf, idx, face);
			const int           id   = buf.object_ids[idx];
			const int           bin  = bin_y * params.bins_x + std::clamp(x / params.bin_size, 0, params.bins_x - 1);
			float               sum  = 0.0f;
			float               full = 0.0f;    // Unshadowed sum, with shadows.
			float               ambient_p = ambient;    // The sky term, after the weather.
			float               o         = 1.0f;    // Open to the sky.
			if (weather) {
				// Clouds take their share of the sky light where it is open.
				const Shaded_point p{px, py, wz};
				o = Shaded_weather_open(*weather, p, n);
				const float        c = o > 0.0f ? Shaded_cloud_cover(*weather, p) : 0.0f;
				const float        s = 1.0f - params.cloud_shadows * o * c;
				ambient_p            = ambient * s;
				if (sky_out) {
					sky_out[idx] = o <= 0.0f ? 0 : static_cast<uint8_t>(1 + std::lround(std::clamp(s, 0.0f, 1.0f) * 254.0f));
				}
			}
			// Light let through: 1 - strength * (1 - Ts * Tg).  With strength
			// 0 it is exactly 1, and the sums below are those without shadows.
			auto shadow_factor = [&](int li) {
				float     t = 1.0f;
				const int m = shadows->light_map[li];
				if (m >= 0) {
					t = shadow_screen_at(buf, *shadows, shadows->maps[m], x, y, buf.height_map[idx]);
				}
				const int f = shadows->light_field[li];
				if (f >= 0) {
					t *= shadow_grid_at(*shadows, shadows->fields[f], Shaded_point{px, py, wz});
				}
				return 1.0f - shadows->strength * (1.0f - t);
			};
			for (uint32_t k = bin_offsets[bin]; k < bin_offsets[bin + 1]; k++) {
				const Shaded_light_record& l = lights[bin_lights[k]];
				if (id != shaded_id_none && l.object_id == id) {
					sum += l.intensity;    // The light's own object.
					full += l.intensity;
					continue;
				}
				const float radius = l.radius * params.radius_scale;
				const float r2     = radius * radius;
				const float dx     = wrap(l.x - px);
				const float dy     = wrap(l.y - py);
				const float h2     = dx * dx + dy * dy;
				if (h2 >= r2) {
					continue;
				}
				if (l.flags & shaded_light_area) {
					if (wz >= l.z - 1.0f && wz <= l.z + params.area_height) {
						if (!shadows) {
							sum += l.intensity * Shaded_falloff(h2 / r2);
							continue;
						}
						const float f = shadow_factor(bin_lights[k]);
						full += l.intensity * Shaded_falloff(h2 / r2);
						sum += l.intensity * f * Shaded_falloff(h2 / r2);
					}
					continue;
				}
				const float dz = (l.z - wz) * params.tiles_per_lift;
				const float d2 = h2 + dz * dz;
				if (d2 >= r2) {
					continue;
				}
				const float dot   = n.x * dx + n.y * dy + n.z * dz;
				const float ndotl = d2 > 1e-6f ? dot / std::sqrt(d2) : 1.0f;
				if (ndotl <= 0.0f) {
					continue;
				}
				if (!shadows) {
					sum += l.intensity * Shaded_falloff(d2 / r2) * (facing + (1.0f - facing) * ndotl);
					continue;
				}
				const float f = shadow_factor(bin_lights[k]);
				full += l.intensity * Shaded_falloff(d2 / r2) * (facing + (1.0f - facing) * ndotl);
				sum += l.intensity * f * Shaded_falloff(d2 / r2) * (facing + (1.0f - facing) * ndotl);
			}
			float level = ambient_p + (1.0f - ambient_p) * (1.0f - std::exp(-sum * params.brightness * inv_soft));
			if (flash_map) {
				// The flash lifts faces turned to it as far as its shadows
				// let it through; a trace through a penumbra's last 1/32 is
				// left out, so every pixel it lifts has a flash level.
				float       f     = 0.0f;
				const float ndotl = n.x * flash_l[0] + n.y * flash_l[1] + n.z * flash_l[2];
				if (ndotl > 0.0f) {
					const float t = shadow_screen_at(buf, weather->flash, *flash_map, x, y, buf.height_map[idx]);
					f             = std::min(params.flash * t * (facing + (1.0f - facing) * ndotl), 1.0f);
					f             = f < 1.0f / 32.0f ? 0.0f : f;
				}
				level += (1.0f - level) * f;
				if (flash_out) {
					flash_out[idx] = static_cast<uint8_t>(f > 0.0f ? std::max(quantise7(f * 7.0f, x + 3, y + 2), 1) : 0);
				}
			} else if (flash_out) {
				flash_out[idx] = 0;
			}
			if (particles && particles[idx] && o > 0.0f) {
				// A drop or flake in the open takes the light where it is:
				// the sky and the lamps' light without the surface's facing
				// or shadows, and the flash as far as its shadows let it.
				const float scatter   = scatter_sum(bin, id, px, py, wz);
				float       scattered = ambient_p + (1.0f - ambient_p) * (1.0f - std::exp(-scatter * params.brightness * inv_soft));
				if (flash_map) {
					const float t = shadow_screen_at(buf, weather->flash, *flash_map, x, y, buf.height_map[idx]);
					const float f = std::min(params.flash * t, 1.0f);
					scattered += f < 1.0f / 32.0f ? 0.0f : (1.0f - scattered) * f;
				}
				level = Shaded_particle_level(level, scattered, o);
			}
			levels[idx] = quantise(level, x, y);
			if (fog_out) {
				// Fog in front of the pixel, lit by the sky and by the lamps
				// whose light it scatters (no facing, no shadows).
				const float m    = params.fog ? Shaded_fog_factor(params, Shaded_point{px, py, wz}, o) : 0.0f;
				const int   fk   = quantise7(m * 8.0f, x + 2, y + 1);
				uint8_t     code = 0;
				if (fk > 0) {
					const float scatter = scatter_sum(bin, id, px, py, wz);
					const float lit     = ambient_p
									  + (1.0f - ambient_p)
												* (1.0f - std::exp(-scatter * params.brightness * params.fog_scatter * inv_soft));
					code = static_cast<uint8_t>(fk + 8 * quantise7(lit * 7.0f, x + 1, y + 3));
				}
				fog_out[idx] = code;
			}
			if (shadow_out) {
				shadow_out[idx] = full > 0.0f ? static_cast<uint8_t>(std::lround(std::clamp(sum / full, 0.0f, 1.0f) * 255.0f)) : 255;
			}
		}
	}
}

void Shaded_cpu::apply_light_rows(
		const Shaded_scene_buffers& buf, const Shaded_rect& r, int y0, int y1, const uint8_t* levels, const uint8_t* remap,
		unsigned char* frame) {
	for (int y = y0; y < y1; y++) {
		const int idx = buf.index(r.x, y);
		for (int i = idx; i < idx + r.w; i++) {
			frame[i] = remap[levels[i] * 256 + frame[i]];
		}
	}
}

void Shaded_cpu::apply_weather_rows(
		const Shaded_scene_buffers& buf, const Shaded_rect& r, int y0, int y1, const uint8_t* levels, const uint8_t* remap,
		const uint8_t* fog_codes, const uint8_t* fog_table, const uint8_t* flash_levels, const uint8_t* flash_table,
		unsigned char* frame) {
	for (int y = y0; y < y1; y++) {
		const int idx = buf.index(r.x, y);
		for (int i = idx; i < idx + r.w; i++) {
			int pix = remap[levels[i] * 256 + frame[i]];
			if (flash_table && flash_levels && flash_levels[i]) {
				pix = flash_table[flash_levels[i] * 256 + pix];
			}
			if (fog_table && fog_codes && fog_codes[i]) {
				pix = fog_table[fog_codes[i] * 256 + pix];
			}
			frame[i] = static_cast<unsigned char>(pix);
		}
	}
}

void Shaded_cpu::apply_level_rows(
		const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_rect& r, int y0, int y1,
		float level, const uint8_t* remap, unsigned char* frame) {
	const int top_level = std::max(params.level_count - 1, 1);
	for (int y = y0; y < y1; y++) {
		const int row = buf.index(0, y);
		for (int x = r.x; x < r.x + r.w; x++) {
			// As light_rows quantises the level of unlit pixels.
			const float threshold = params.dither ? Shaded_bayer4(x + params.scroll_px, y + params.scroll_py) : 0.5f;
			const int   q         = static_cast<int>(std::floor(level * static_cast<float>(top_level) + threshold));
			frame[row + x]        = remap[std::clamp(q, 0, top_level) * 256 + frame[row + x]];
		}
	}
}

float Shaded_cpu::palette_luminance(const uint8_t* pal768, const uint8_t* day768) {
	long sum = 0, day = 0;
	for (int i = 1; i <= shaded_last_lit_index; i++) {
		sum += Shaded_luminance(pal768[3 * i], pal768[3 * i + 1], pal768[3 * i + 2]);
		day += Shaded_luminance(day768[3 * i], day768[3 * i + 1], day768[3 * i + 2]);
	}
	return day > 0 ? static_cast<float>(sum) / static_cast<float>(day) : 0.0f;
}

int Shaded_cpu::nearest_colour(const uint8_t* pal768, int r, int g, int b, int first, int last) {
	int best      = first;
	int best_dist = std::numeric_limits<int>::max();
	for (int i = first; i <= last; i++) {
		const int dr   = r - pal768[3 * i];
		const int dg   = g - pal768[3 * i + 1];
		const int db   = b - pal768[3 * i + 2];
		const int dist = dr * dr + dg * dg + db * db;
		if (dist < best_dist) {
			best      = i;
			best_dist = dist;
			if (dist == 0) {
				break;
			}
		}
	}
	return best;
}

void Shaded_cpu::colour_lab(int r, int g, int b, float lab[3]) {
	auto lin = [](int v) {
		const float c = static_cast<float>(v) / 63.0f;
		return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
	};
	const float lr = lin(r), lg = lin(g), lb = lin(b);
	const float x  = (0.4124f * lr + 0.3576f * lg + 0.1805f * lb) / 0.95047f;
	const float y  = 0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
	const float z  = (0.0193f * lr + 0.1192f * lg + 0.9505f * lb) / 1.08883f;
	auto        f  = [](float t) {
        return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116.0f;
	};
	const float fx = f(x), fy = f(y), fz = f(z);
	lab[0]         = 116.0f * fy - 16.0f;
	lab[1]         = 500.0f * (fx - fy);
	lab[2]         = 200.0f * (fy - fz);
}

int Shaded_cpu::nearest_colour_lab(const float (*pal_lab)[3], const float lab[3], int first, int last) {
	int   best      = first;
	float best_dist = std::numeric_limits<float>::max();
	for (int i = first; i <= last; i++) {
		const float dl   = lab[0] - pal_lab[i][0];
		const float da   = lab[1] - pal_lab[i][1];
		const float db   = lab[2] - pal_lab[i][2];
		// Lightness weighted 4: hue stays close, and the ramp no more often
		// steps darker as the light rises than with RGB distance.
		const float dist = 4.0f * dl * dl + da * da + db * db;
		if (dist < best_dist) {
			best      = i;
			best_dist = dist;
			if (dist == 0.0f) {
				break;
			}
		}
	}
	return best;
}

// How much of the night tint applies for an ambient palette of luminance y
// (relative to DAY): full in the dark, none by dusk and dawn.
static float Night_tint_weight(float y, float tint) {
	if (tint <= 0.0f) {
		return 0.0f;
	}
	const float dark = std::clamp((shaded_night_tint_dusk_y - y) / (shaded_night_tint_dusk_y - shaded_night_tint_night_y), 0.0f, 1.0f);
	return std::min(tint, 1.0f) * dark;
}

// Colours i = 1..shaded_last_lit_index of in768 keep their luminance and lose
// (weight 0.5) or turn blue (weight 1) in their chroma; the rest copy through.
static void Apply_night_tint(const uint8_t* in768, float weight, uint8_t* out768) {
	std::memcpy(out768, in768, 768);
	// Cool blue gain per channel, scaled to keep luminance.
	constexpr float gain_r = 0.75f, gain_g = 0.93f, gain_b = 1.35f;
	constexpr float norm   = 0.299f * gain_r + 0.587f * gain_g + 0.114f * gain_b;
	const float     to_grey = std::min(weight / 0.5f, 1.0f);
	const float     to_blue = std::clamp((weight - 0.5f) / 0.5f, 0.0f, 1.0f);
	const float     gains[3] = {gain_r / norm, gain_g / norm, gain_b / norm};
	for (int i = 1; i <= shaded_last_lit_index; i++) {
		const float rgb[3] = {static_cast<float>(in768[3 * i]), static_cast<float>(in768[3 * i + 1]), static_cast<float>(in768[3 * i + 2])};
		const float y      = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
		for (int k = 0; k < 3; k++) {
			const float grey = rgb[k] + (y - rgb[k]) * to_grey;
			const float blue = grey * (1.0f + (gains[k] - 1.0f) * to_blue);
			out768[3 * i + k] = static_cast<uint8_t>(std::clamp(std::lround(blue), 0L, 63L));
		}
	}
}

void Shaded_cpu::build_remap(
		const uint8_t* ambient768, const uint8_t* const refs[4], Shaded_light_params& params, uint8_t* remap,
		uint8_t* displayed768, Shaded_ramp* ramp) {
	const uint8_t* day = refs[0];
	// The stops: black, A at its (slider scaled) luminance, then the brighter
	// reference palettes by luminance, DAY last at 1.
	const float    ambient = std::clamp(palette_luminance(ambient768, day) * params.ambient, 0.0f, 1.0f);
	Shaded_ramp    stops;
	const uint8_t* colours[6];
	auto           add_stop = [&](int palette, float level, const uint8_t* pal) {
        stops.palette[stops.count] = palette;
        stops.level[stops.count]   = level;
        colours[stops.count]       = pal;
        stops.count++;
	};
	add_stop(Shaded_ramp::black, 0.0f, nullptr);
	// The night tint recolours the ambient stop only (same luminance per
	// colour), so lamp and fire light keep their own colours.
	uint8_t tinted[768];
	const uint8_t* ambient_colours = ambient768;
	{
		const float weight = Night_tint_weight(palette_luminance(ambient768, day), params.night_tint);
		if (weight > 0.0f) {
			Apply_night_tint(ambient768, weight, tinted);
			ambient_colours = tinted;
		}
	}
	add_stop(Shaded_ramp::ambient, ambient, ambient_colours);
	std::pair<float, int> brighter[3];
	int                   nbrighter = 0;
	for (int k = 1; k < 4; k++) {
		const float y = palette_luminance(refs[k], day);
		if (y > ambient + 0.02f && y < 0.98f) {
			brighter[nbrighter++] = std::make_pair(y, k);
		}
	}
	std::sort(brighter, brighter + nbrighter);
	for (int k = 0; k < nbrighter; k++) {
		add_stop(shaded_ref_palettes[brighter[k].second], brighter[k].first, refs[brighter[k].second]);
	}
	add_stop(shaded_ref_palettes[0], 1.0f, day);
	// Ramp colours are matched to DAY in CIELAB, lightness weighted 4: plain
	// RGB distance turned DUSK/NIGHT olives and sea-greens and lit reds into
	// DAY's greens and yellow-greens (green-tint.md).
	float day_lab[shaded_last_lit_index + 1][3] = {};
	for (int i = 1; i <= shaded_last_lit_index; i++) {
		colour_lab(day[3 * i], day[3 * i + 1], day[3 * i + 2], day_lab[i]);
	}
	const int top = std::max(params.level_count - 1, 1);
	for (int q = 0; q < shaded_light_levels; q++) {
		const float l   = std::min(static_cast<float>(q) / static_cast<float>(top), 1.0f);
		int         seg = 0;    // Stops seg and seg + 1 bracket l.
		while (seg + 2 < stops.count && l > stops.level[seg + 1]) {
			seg++;
		}
		const float span = stops.level[seg + 1] - stops.level[seg];
		const float t    = span > 1e-6f ? std::clamp((l - stops.level[seg]) / span, 0.0f, 1.0f) : 1.0f;
		uint8_t*    row  = remap + q * 256;
		for (int i = 0; i < 256; i++) {
			row[i] = static_cast<uint8_t>(i);
		}
		for (int i = 1; i <= shaded_last_lit_index; i++) {
			int c[3];
			for (int k = 0; k < 3; k++) {
				const float lo = colours[seg] ? colours[seg][3 * i + k] : 0.0f;
				const float hi = colours[seg + 1] ? colours[seg + 1][3 * i + k] : 0.0f;
				c[k]           = static_cast<int>(std::lround(lo + (hi - lo) * t));
			}
			float lab[3];
			colour_lab(c[0], c[1], c[2], lab);
			row[i] = static_cast<uint8_t>(nearest_colour_lab(day_lab, lab, 1, shaded_last_lit_index));
		}
	}
	// B': DAY for the relit indices, A's own cycling colours, DAY's 0xff.
	// The flame ranges take the first stop above ambient instead (CANDLE at
	// night; DAY when no light palette is brighter than A), as the light
	// palette Exult shows at night with a flame in view.
	const uint8_t* glow = colours[2];
	std::memcpy(displayed768, day, 768);
	for (int i = shaded_last_lit_index + 1; i < 0xff; i++) {
		std::memcpy(displayed768 + 3 * i, (Shaded_glow_index(i) ? glow : ambient768) + 3 * i, 3);
	}
	params.ambient_level = ambient;
	if (ramp) {
		*ramp = stops;
	}
}

void Shaded_cpu::resolve_view(
		const Shaded_scene_buffers& buf, const Shaded_light_params& params, const Shaded_rect& r, const uint8_t* lut,
		unsigned char* frame) {
	for (int y = r.y; y < r.y + r.h; y++) {
		const int idx = buf.index(r.x, y);
		for (int i = idx; i < idx + r.w; i++) {
			frame[i] = lut[view_code(buf, params, i)];
		}
	}
}

Shaded_light* Shaded_light::active = nullptr;

Shaded_light& Shaded_light::get() {
	static Shaded_light instance;
	return instance;
}

Shaded_light::Shaded_light()
		: weather_input(std::make_unique<Shaded_weather_input>()), weather_set(std::make_unique<Shaded_weather_set>()) {
	read_config();
	if (const char* hour = std::getenv("EXULT_SHADED_HOUR"); hour && *hour) {
		pending_hour = std::clamp(std::atoi(hour), 0, 23);
	}
	if (const char* tele = std::getenv("EXULT_SHADED_TELEPORT"); tele && *tele) {
		pending_teleport = std::sscanf(tele, "%d,%d,%d", &teleport_to[0], &teleport_to[1], &teleport_to[2]) == 3;
		if (!pending_teleport) {
			std::cout << "SHADED: EXULT_SHADED_TELEPORT wants x,y,z, not " << tele << std::endl;
		}
	}
	if (const char* dir = std::getenv("EXULT_SHADED_DUMP"); dir && *dir) {
		dump_dir = dir;
		std::error_code ec;
		std::filesystem::create_directories(dump_dir, ec);
		if (ec) {
			std::cout << "SHADED: could not create dump folder " << dump_dir << ": " << ec.message() << std::endl;
			dump_dir.clear();
		} else {
			lighting_on = true;    // Unless EXULT_SHADED_LIGHT=0.
		}
	}
	// EXULT_SHADED_LIGHT=0|1 overrides the option, for comparison runs.
	if (const char* light = std::getenv("EXULT_SHADED_LIGHT"); light && *light) {
		lighting_on = *light != '0';
	}
	const bool enabled = lighting_on || !dump_dir.empty();
	const char* at = std::getenv("EXULT_SHADED_DUMP_AT");
	std::stringstream times(at && *at ? at : "6000");
	std::string       item;
	while (std::getline(times, item, ',')) {
		dump_at.push_back(std::atoi(item.c_str()));
	}
	std::sort(dump_at.begin(), dump_at.end());
	if (const char* view = std::getenv("EXULT_SHADED_VIEW")) {
		for (int v = 0; v < static_cast<int>(Shaded_view::count); v++) {
			if (std::string(view) == view_name(static_cast<Shaded_view>(v))) {
				params.view = v;
			}
		}
		if (std::string(view) == "lit") {    // The lit frame is the normal view.
			params.view = static_cast<int32_t>(Shaded_view::normal);
		}
	}
	// Light pass threads: config or EXULT_SHADED_THREADS, 0 = auto.
	if (const char* thr = std::getenv("EXULT_SHADED_THREADS"); thr && *thr) {
		params.threads = std::max(std::atoi(thr), 0);
	}
	if (params.threads <= 0) {
		params.threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
	}
	// Shadow overrides for comparison and timing runs.
	if (const char* sh = std::getenv("EXULT_SHADED_SHADOWS"); sh && *sh) {
		params.shadows = *sh != '0' ? 1 : 0;
	}
	if (const char* res = std::getenv("EXULT_SHADED_SHADOW_RES"); res && *res) {
		params.shadow_resolution = Shadow_resolution(std::atoi(res));
	}
	if (const char* sl = std::getenv("EXULT_SHADED_SHADOW_LIGHTS"); sl && *sl) {
		params.shadow_lights = std::clamp(std::atoi(sl), 1, 16);
	}
	if (const char* cache = std::getenv("EXULT_SHADED_SHADOW_CACHE"); cache && *cache) {
		shadow_cache = *cache != '0';
	}
	if (const char* parts = std::getenv("EXULT_SHADED_SHADOW_PARTS"); parts && *parts) {
		const std::string p(parts);
		shadow_parts = p == "screen" ? shaded_shadow_parts_screen
							   : (p == "grid" ? shaded_shadow_parts_grid : shaded_shadow_parts_both);
	}
	if (const char* grid = std::getenv("EXULT_SHADED_SHADOW_GRID"); grid && *grid) {
		shadow_grid_ray = std::string(grid) == "ray";
	}
	// Rounded volume overrides, for the twins (EXULT_SHADED_ROUNDED=0 is the
	// box-only look of the stage 3 renderer).
	if (const char* rnd = std::getenv("EXULT_SHADED_ROUNDED"); rnd && *rnd) {
		params.rounded = *rnd != '0' ? 1 : 0;
	}
	if (const char* seams = std::getenv("EXULT_SHADED_ROOF_SEAMS"); seams && *seams) {
		params.roof_seams = *seams != '0' ? 1 : 0;
	}
	// Weather overrides, for the twins (EXULT_SHADED_WEATHER=0 is the look
	// without weather lighting); percentages as in the config.
	auto env_percent = [](const char* name, float& value) {
		if (const char* v = std::getenv(name); v && *v) {
			value = static_cast<float>(std::max(std::atoi(v), 0)) / 100.0f;
		}
	};
	auto env_switch = [](const char* name, int32_t& value) {
		if (const char* v = std::getenv(name); v && *v) {
			value = *v != '0' ? 1 : 0;
		}
	};
	env_percent("EXULT_SHADED_NIGHT_TINT", params.night_tint);
	params.night_tint = std::min(params.night_tint, 1.0f);
	env_switch("EXULT_SHADED_WEATHER", params.weather);
	env_percent("EXULT_SHADED_CLOUD_SHADOWS", params.cloud_shadows);
	params.cloud_shadows = std::min(params.cloud_shadows, 1.0f);
	env_percent("EXULT_SHADED_OVERCAST_SOFTEN", params.overcast_soften);
	params.overcast_soften = std::min(params.overcast_soften, 1.0f);
	env_percent("EXULT_SHADED_FOG", params.fog_density);
	env_switch("EXULT_SHADED_LIGHTNING_SHADOWS", params.lightning_shadows);
	env_switch("EXULT_SHADED_PRECIPITATION", params.precipitation);
	if (const char* flash = std::getenv("EXULT_SHADED_DUMP_FLASH"); flash && *flash) {
		flash_dumps_left = std::max(std::atoi(flash), 0);
	}
	if (const char* test = std::getenv("EXULT_SHADED_WEATHER_TEST"); test && *test && *test != '0') {
		weather_test = true;
	}
	const char* tim = std::getenv("EXULT_SHADED_TIMING");
	timing          = tim && *tim && *tim != '0';
	record_on       = enabled;
	params.enabled  = enabled ? 1 : 0;
	if (record_on) {
		std::cout << "SHADED: scene record on" << (dump_dir.empty() ? "" : ", dumps in ") << dump_dir << std::endl;
	}
}

Shaded_light::~Shaded_light() {
	if (timing) {
		report_timing();
	}
}

Shaded_backend Shaded_light::backend() {
	switch (static_cast<Shaded_backend>(params.backend)) {
	case Shaded_backend::gpu:
		if (!warned_backend) {
			std::cout << "SHADED: GPU backend not built yet; using CPU" << std::endl;
			warned_backend = true;
		}
		params.backend = static_cast<int32_t>(Shaded_backend::cpu);
		break;
	case Shaded_backend::cpu:
		break;
	}
	return Shaded_backend::cpu;
}

const char* Shaded_light::view_name(Shaded_view view) {
	switch (view) {
	case Shaded_view::height:
		return "height";
	case Shaded_view::facing:
		return "facing";
	case Shaded_view::object_id:
		return "id";
	case Shaded_view::light:
		return "light";
	case Shaded_view::unlit:
		return "unlit";
	case Shaded_view::shadow:
		return "shadow";
	case Shaded_view::normals:
		return "normals";
	case Shaded_view::weather:
		return "weather";
	case Shaded_view::normal:
	case Shaded_view::count:
		break;
	}
	return "normal";
}

Shaded_view Shaded_light::cycle_view() {
	if (!record_on) {
		return Shaded_view::normal;
	}
	params.view = (params.view + 1) % static_cast<int>(Shaded_view::count);
	return static_cast<Shaded_view>(params.view);
}

/*
 *  Read config/video/shaded_light/...; percentages become fractions.
 */

void Shaded_light::read_config() {
	if (!config) {
		return;
	}
	const std::string base = "config/video/shaded_light/";
	bool              enabled;
	bool              old_enabled;    // Stage 1 form: config/video/shaded_light = yes.
	config->value(base + "enabled", enabled, false);
	config->value("config/video/shaded_light", old_enabled, false);
	lighting_on  = enabled || old_enabled;
	auto percent = [&](const char* key, int def) {
		int value;
		config->value(base + key, value, def);
		return static_cast<float>(std::max(value, 0)) / 100.0f;
	};
	params.brightness   = percent("brightness", 100);
	params.radius_scale = percent("radius", 100);
	params.ambient      = percent("ambient", 100);
	params.softness     = std::max(percent("softness", 100), 0.01f);
	params.facing_min   = std::min(percent("facing", 50), 1.0f);
	bool dither;
	config->value(base + "dither", dither, true);
	params.dither = dither ? 1 : 0;
	int threads;
	config->value(base + "threads", threads, 0);
	params.threads = std::max(threads, 0);    // 0 = auto, resolved later.
	bool shadows;
	config->value(base + "shadows", shadows, true);
	params.shadows         = shadows ? 1 : 0;
	params.shadow_strength = std::min(percent("shadow_strength", 100), 1.0f);
	params.shadow_softness = percent("shadow_softness", 100);
	shadow_strength_config = params.shadow_strength;
	shadow_softness_config = params.shadow_softness;
	int shadow_lights;
	config->value(base + "shadow_lights", shadow_lights, 4);
	params.shadow_lights = std::clamp(shadow_lights, 1, 16);
	int resolution;
	config->value(base + "shadow_resolution", resolution, 2);
	params.shadow_resolution = Shadow_resolution(resolution);
	bool rounded;
	config->value(base + "rounded", rounded, true);
	params.rounded = rounded ? 1 : 0;
	bool roof_seams;
	config->value(base + "roof_seams", roof_seams, true);
	params.roof_seams = roof_seams ? 1 : 0;
	params.night_tint = std::min(percent("night_tint", 0), 1.0f);
	// Weather.
	bool weather;
	config->value(base + "weather/enabled", weather, true);
	params.weather         = weather ? 1 : 0;
	params.cloud_shadows   = std::min(percent("weather/cloud_shadows", 40), 1.0f);
	params.overcast_soften = std::min(percent("weather/overcast_soften", 50), 1.0f);
	params.fog_density     = percent("weather/fog", 100);
	params.fog_scatter     = percent("weather/fog_scatter", 100);
	int value;
	config->value(base + "weather/cloud_softness", value, 6);
	params.cloud_softness = static_cast<float>(std::clamp(value, 0, 32));
	config->value(base + "weather/fog_height", value, 4);
	params.fog_height = static_cast<float>(std::clamp(value, 1, 16));
	config->value(base + "weather/fog_distance", value, 6);
	params.fog_distance = static_cast<float>(std::clamp(value, 1, 64));
	config->value(base + "weather/fog_fade", value, 3);
	params.fog_fade = static_cast<float>(std::clamp(value, 0, 16));
	bool lightning_shadows;
	config->value(base + "weather/lightning_shadows", lightning_shadows, true);
	params.lightning_shadows = lightning_shadows ? 1 : 0;
	bool precipitation;
	config->value(base + "weather/precipitation", precipitation, true);
	params.precipitation = precipitation ? 1 : 0;
}

/*
 *  Headless debug hooks, once each.  The flags are cleared first, as both
 *  can paint again from inside.
 */

void Shaded_light::run_debug_hooks() {
	Game_window* gwin = Game_window::get_instance();
	if (pending_hour >= 0) {
		const int hour = pending_hour;
		pending_hour   = -1;
		Game_clock* clock = gwin->get_clock();
		clock->set_hour(hour);
		clock->set_minute(0);
		clock->set_ticks(0);
		clock->reset_palette();
		gwin->set_all_dirty();
		std::cout << "SHADED: hour set to " << hour << std::endl;
	}
	if (pending_teleport) {
		pending_teleport = false;
		gwin->teleport_party(Tile_coord(teleport_to[0], teleport_to[1], teleport_to[2]));
		std::cout << "SHADED: teleported to " << teleport_to[0] << "," << teleport_to[1] << "," << teleport_to[2] << std::endl;
	}
}

/*
 *  Read shaded_light.txt (bundled, then the patch folder's) when the game
 *  or mod has changed.  Without a file only the formula is used.
 */

void Shaded_light::load_table() {
	const Exult_Game  game = Game::get_game_type();
	const std::string key  = Game_key();
	if (table_loaded && key == table_game) {
		return;
	}
	table_loaded = true;
	table_game   = key;
	silhouettes.clear();    // Shape frames may be other ones now.
	light_table.clear();
	volume_table.clear();
	int resource = -1;
	if (game == BLACK_GATE) {
		resource = EXULT_BG_FLX_SHADED_LIGHT_TXT;
	} else if (game == SERPENT_ISLE) {
		resource = EXULT_SI_FLX_SHADED_LIGHT_TXT;
	}
	std::array<std::string_view, 2> sections{"lights", "volumes"};
	std::array                      readers = make_unique_array<Base_reader>(
            std::make_unique<Shaded_light_reader>(light_table), std::make_unique<Shaded_volume_reader>(volume_table));
	static_assert(sections.size() == readers.size());
	try {
		Read_text_data_file("shaded_light", readers, sections, false, game, resource);
	} catch (const file_open_exception& e) {
		std::cout << "SHADED: no light table (" << e.what() << "), using the default formula" << std::endl;
	}
	table_radius = 0;
	for (const auto& [id, entry] : light_table) {
		table_radius = std::max(table_radius, entry.radius);
	}
	std::cout << "SHADED: light table has " << light_table.size() << " rows" << std::endl;
	std::cout << "SHADED: volume rows " << volume_table.size() << " (rounded " << (params.rounded ? "on" : "off")
			  << ", roof seams " << (params.rounded && params.roof_seams ? "on" : "off") << ")" << std::endl;
}

/*
 *  The volume an object's pixels get: its exact (shape, frame & 31) row,
 *  then its (shape, -1) row, else figure for actors and box for the rest.
 *  Always box with rounded volumes off.
 */

Shaded_volume Shaded_light::volume_for(const Game_object& obj) const {
	if (!params.rounded) {
		return Shaded_volume::box;
	}
	const int shape = obj.get_shapenum();
	auto      it    = volume_table.find(std::make_pair(shape, obj.get_framenum() & 31));
	if (it == volume_table.end()) {
		it = volume_table.find(std::make_pair(shape, -1));
	}
	if (it != volume_table.end()) {
		return static_cast<Shaded_volume>(it->second);
	}
	return obj.as_actor() ? Shaded_volume::figure : Shaded_volume::box;
}

/*
 *  The row for a light: exact (shape, frame & 31), then (shape, -1), else
 *  the formula from its light_data brightness.
 */

Shaded_light_entry Shaded_light::find_row(int shape, int frame, int brightness, Shaded_light_row& row) const {
	auto it = light_table.find(std::make_pair(shape, frame & 31));
	if (it != light_table.end()) {
		row = Shaded_light_row::exact;
		return it->second;
	}
	it = light_table.find(std::make_pair(shape, -1));
	if (it != light_table.end()) {
		row = Shaded_light_row::wildcard;
		return it->second;
	}
	row = Shaded_light_row::formula;
	return Shaded_default_light(brightness);
}

/*
 *  Add a light from source (the light object) placed on owner (the same
 *  object, or the party member carrying it).
 */

void Shaded_light::add_light(
		const Game_object& source, const Game_object& owner, const Shaded_light_entry& entry, Shaded_light_row row,
		bool carried) {
	if (lights.size() >= static_cast<size_t>(shaded_max_lights)) {
		if (!warned_lights) {
			warned_lights = true;
			std::cout << "SHADED: over " << shaded_max_lights << " lights in view, the rest are left out" << std::endl;
		}
		return;
	}
	Game_window* gwin = Game_window::get_instance();
	int          ox, oy;
	gwin->get_shape_location(&owner, ox, oy);
	const Tile_coord  t     = owner.get_tile();
	const Shape_info& info  = owner.get_info();
	const int         frame = owner.get_framenum();
	// (ox, oy) is the pixel up and left of the box's SE bottom corner, with
	// the smooth-scroll and barge offsets the painter used; undoing
	// Get_shape_location gives the corner exactly (tile tx + 1, ty + 1).
	const float lift_px  = static_cast<float>(t.tz) * params.pixels_per_lift;
	const float corner_x = static_cast<float>(gwin->get_scrolltx())
						   + (static_cast<float>(ox + gwin->get_scrolltx_lo() + 1) + lift_px) / params.pixels_per_tile;
	const float corner_y = static_cast<float>(gwin->get_scrollty())
						   + (static_cast<float>(oy + gwin->get_scrollty_lo() + 1) + lift_px) / params.pixels_per_tile;
	const auto  tiles    = static_cast<float>(c_num_tiles);
	const auto  ztiles   = static_cast<float>(info.get_3d_height());
	Shaded_light_record rec;
	rec.x = std::fmod(corner_x - static_cast<float>(info.get_3d_xtiles(frame)) * 0.5f + tiles, tiles);
	rec.y = std::fmod(corner_y - static_cast<float>(info.get_3d_ytiles(frame)) * 0.5f + tiles, tiles);
	if (carried) {    // About hand height.
		rec.z = static_cast<float>(t.tz) + 0.75f * ztiles;
	} else {
		rec.z = static_cast<float>(t.tz) + (entry.height >= 0 ? static_cast<float>(entry.height) : ztiles);
	}
	rec.radius    = static_cast<float>(entry.radius);
	rec.red       = static_cast<float>(entry.red) / 255.0f;
	rec.green     = static_cast<float>(entry.green) / 255.0f;
	rec.blue      = static_cast<float>(entry.blue) / 255.0f;
	rec.intensity = static_cast<float>(entry.brightness) / 100.0f;
	rec.flags     = (entry.flags & (shaded_light_area | shaded_light_off)) | (carried ? shaded_light_carried : 0);
	rec.object_id = shaded_id_none;    // Resolved once the paint has recorded it.
	rec.shape     = source.get_shapenum();
	rec.frame     = source.get_framenum();
	lights.push_back(rec);
	light_objects.push_back(&owner);
	light_sources.push_back(&source);
	light_rows.push_back(row);
}

/*
 *  The lights of this paint: lights in the chunks on screen and as far
 *  around as the largest radius reaches, then the party's carried lights.
 */

void Shaded_light::gather_lights() {
	Game_window* gwin = Game_window::get_instance();
	Game_map*    map  = gwin->get_map();
	lights.clear();
	light_objects.clear();
	light_sources.clear();
	light_rows.clear();
	const float max_radius = static_cast<float>(std::max(table_radius, Shaded_default_light(8).radius)) * params.radius_scale;
	const int   reach      = std::min(static_cast<int>(std::ceil(max_radius)), 32);
	const int   margin     = (reach + c_tiles_per_chunk - 1) / c_tiles_per_chunk;
	// Chunks as Game_render::paint_map walks them, widened by margin.
	auto range = [margin](int scroll, int pixels, int& start, int& count) {
		start          = (scroll - 1 + c_num_tiles) / c_tiles_per_chunk - c_num_chunks - margin;
		const int stop = 2 + (scroll + (pixels + c_tilesize - 2) / c_tilesize + c_tiles_per_chunk / 2) / c_tiles_per_chunk + margin;
		count          = std::min(stop - start, c_num_chunks);
	};
	int start_x, count_x, start_y, count_y;
	range(gwin->get_scrolltx(), gwin->get_width(), start_x, count_x);
	range(gwin->get_scrollty(), gwin->get_height(), start_y, count_y);
	auto wrap = [](int c) {
		return ((c % c_num_chunks) + c_num_chunks) % c_num_chunks;
	};
	const bool dungeon   = gwin->is_in_dungeon() != 0;
	const int  skip_lift = gwin->get_render_skip_lift();
	for (int j = 0; j < count_y; j++) {
		for (int i = 0; i < count_x; i++) {
			// Never creates chunks: one not yet read has no lights.
			const Map_chunk* chunk = map->get_chunk_unsafe(wrap(start_x + i), wrap(start_y + j));
			if (!chunk) {
				continue;
			}
			const auto& set = dungeon ? chunk->get_dungeon_lights() : chunk->get_non_dungeon_lights();
			for (const Game_object* obj : set) {
				const int frame = obj->get_framenum();
				const int b     = obj->get_info().get_object_light(frame);
				if (b <= 0 || obj->get_lift() >= skip_lift) {
					continue;
				}
				Shaded_light_row         row;
				const Shaded_light_entry entry = find_row(obj->get_shapenum(), frame, b, row);
				if (!(entry.flags & shaded_light_off)) {
					add_light(*obj, *obj, entry, row, false);
				}
			}
		}
	}
	// Carried lights: each readied light Actor::refigure_gear counts, at
	// the bearer; a formula light if none is found.
	static const Ready_type_Exult spots[]
			= {head, belt, lhand, lfinger, legs, feet, rfinger, rhand, torso, amulet, earrings, cloak, gloves};
	Actor*    party[9];
	const int count = gwin->get_party(party, 1);
	for (int p = 0; p < count; p++) {
		const Actor* actor = party[p];
		if (!actor || actor->get_light_source() <= 0 || actor->get_lift() >= skip_lift) {
			continue;
		}
		bool found = false;
		for (const auto spot : spots) {
			const Game_object* worn = actor->get_readied(spot);
			if (!worn) {
				continue;
			}
			const Shape_info& info  = worn->get_info();
			const int         rdy   = info.get_ready_type();
			const int         frame = worn->get_framenum();
			const int         b     = info.get_object_light(frame);
			if (!info.is_light_source() || b <= 0 || (spot == belt && (rdy == lhand || rdy == rhand || rdy == both_hands))) {
				continue;
			}
			found = true;
			Shaded_light_row         row;
			const Shaded_light_entry entry = find_row(worn->get_shapenum(), frame, b, row);
			if (!(entry.flags & shaded_light_off)) {
				add_light(*worn, *actor, entry, row, true);
			}
		}
		if (!found) {
			add_light(*actor, *actor, Shaded_default_light(actor->get_light_source()), Shaded_light_row::formula, true);
		}
	}
	// Hash of what the lighting depends on, positions to 1/16 tile.
	uint64_t hash = 14695981039346656037ull;
	for (const Shaded_light_record& rec : lights) {
		Hash_add(hash, static_cast<int32_t>(std::lround(rec.x * 16.0f)));
		Hash_add(hash, static_cast<int32_t>(std::lround(rec.y * 16.0f)));
		Hash_add(hash, static_cast<int32_t>(std::lround(rec.z * 16.0f)));
		Hash_add(hash, static_cast<int32_t>(std::lround(rec.radius * 16.0f)));
		Hash_add(hash, static_cast<int32_t>(std::lround(rec.intensity * 256.0f)));
		Hash_add(hash, rec.flags);
	}
	Hash_add(hash, static_cast<int32_t>(lights.size()));
	light_hash = hash;
}

/*
 *  The sliders a lit frame depends on, stage 3's first, then the weather's.
 */

std::array<float, 23> Shaded_light::slider_values() const {
	return {params.ambient,
			params.brightness,
			params.radius_scale,
			params.softness,
			static_cast<float>(params.shadows),
			params.shadow_strength,
			params.shadow_softness,
			static_cast<float>(params.shadow_lights),
			static_cast<float>(params.shadow_resolution),
			static_cast<float>(params.rounded),
			static_cast<float>(params.roof_seams),
			static_cast<float>(params.weather),
			params.cloud_shadows,
			params.cloud_softness,
			params.overcast_soften,
			params.overcast,
			params.fog_density,
			params.fog_height,
			params.fog_distance,
			params.fog_fade,
			params.fog_scatter,
			static_cast<float>(params.lightning_shadows),
			static_cast<float>(params.precipitation)};
}

bool Shaded_light::prepare_paint() {
	if (!record_on && !lighting_on) {
		return false;
	}
	if (shadows_wanted() || weather_wanted()) {
		Shaded_grid::get();    // Turns itself on for the shadows and the weather.
	}
	// The self-test needs the game's palettes for its fog table checks.
	if (weather_test && !weather_test_done && load_weather_refs()) {
		weather_test_done = true;
		Shaded_weather_selftest(ref_pals[0].data(), weather_pals[0].data(), weather_pals[1].data());
	}
	run_debug_hooks();
	load_table();
	gather_lights();
	Game_window* gwin = Game_window::get_instance();
	// A game loaded at night set its palette before there was a main actor,
	// so apply it once more to let lighting take over.
	Palette* pal = gwin->get_pal();
	if (lighting_on && !seen_world_palette && !applied_palette && pal && !pal->is_faded_out()) {
		applied_palette = true;
		pal->apply(false);
	}
	const bool weather_changed = prepare_weather();
	// The lighting of a partial paint must match the frame around it, so
	// any change of what it depends on paints the whole game area.
	const std::array<float, 23> sliders = slider_values();
	const bool lights_changed = lit_active && (light_hash != lit_hash || sliders != lit_sliders || weather_changed);
	// A change of the occlusion grid (a door opened) relights more than
	// its own rectangle.
	bool grid_changed = false;
	if (lit_active && (shadows_wanted() || weather_wanted()) && Shaded_grid::active) {
		Shaded_grid::active->sync();
		grid_changed = Shaded_grid::active->get_params().generation != lit_grid;
	}
	widened_shadow += grid_changed ? 1 : 0;
	// A dump that is due is taken on a full paint, so its checks cover the
	// whole game area.
	bool dump_due = false;
	if (clock_started && dump_next < dump_at.size()) {
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - first_paint).count();
		dump_due      = ms >= dump_at[dump_next];
	}
	// A record made at a smooth-scroll pixel offset cannot be shifted by
	// whole tiles.
	const bool scrolled = (gwin->get_scrolltx() != params.scroll_tx || gwin->get_scrollty() != params.scroll_ty)
						  && (params.scroll_px != 0 || params.scroll_py != 0 || gwin->is_lerping_enabled());
	widened_need += need_full ? 1 : 0;
	widened_lights += lights_changed ? 1 : 0;
	widened_scroll += scrolled ? 1 : 0;
	return need_full || lights_changed || scrolled || grid_changed || dump_due;
}

void Shaded_light::widen_paint(int& x, int& y, int& w, int& h) {
	if (!lit_active || !shadows_wanted() || lights.empty()) {
		return;
	}
	Game_window* gwin = Game_window::get_instance();
	const int    gw   = gwin->get_width();
	const int    gh   = gwin->get_height();
	if (x <= 0 && y <= 0 && x + w >= gw && y + h >= gh) {
		return;    // Already the whole game area.
	}
	// The light rectangles at the scroll position about to be painted.
	Shaded_light_params at = params;
	at.scroll_tx           = gwin->get_scrolltx();
	at.scroll_ty           = gwin->get_scrollty();
	at.scroll_px           = gwin->get_scrolltx_lo();
	at.scroll_py           = gwin->get_scrollty_lo();
	const Shaded_rect paint{x, y, w, h};
	int               x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	for (const Shaded_light_record& l : lights) {
		const Shaded_rect lr = Shaded_cpu::light_screen_rect(at, l, gw, gh);
		if (lr.w <= 0 || Intersect(lr, paint).w <= 0) {
			continue;
		}
		x0 = std::min(x0, lr.x);
		y0 = std::min(y0, lr.y);
		x1 = std::max(x1, lr.x + lr.w);
		y1 = std::max(y1, lr.y + lr.h);
	}
	if (x0 != x || y0 != y || x1 != x + w || y1 != y + h) {
		widened_shadow++;
		x = x0;
		y = y0;
		w = x1 - x0;
		h = y1 - y0;
	}
}

/*
 *  Gather the weather from the effects for this paint: whether there is
 *  any (DAY is lightable while there is), the overcast softening of the
 *  shadows and the clouds.  True if a lit frame must be painted again in
 *  full.
 */

bool Shaded_light::prepare_weather() {
	Game_window*          gwin = Game_window::get_instance();
	Shaded_weather_input& in   = *weather_input;
	Shaded_weather_set&   set  = *weather_set;
	in.clouds.clear();
	in.clouds_effect = false;
	in.fog           = false;
	in.rain          = false;
	in.snow          = false;
	if (weather_wanted() && gwin->get_effects()) {
		gwin->get_effects()->get_shaded_weather(in);
	}
	weather_present = weather_wanted() && (in.clouds_effect || in.fog || flash_on);
	bool changed    = false;
	if (weather_present != last_weather_present) {
		// DAY became lightable, or stopped being so: filter the palette again.
		last_weather_present = weather_present;
		changed              = true;
		need_full            = true;
		Palette* pal         = gwin->get_pal();
		if (pal && !pal->is_faded_out()) {
			pal->apply(false);
		}
	}
	// Overcast weakens and softens the shadows; without it (or without
	// weather lighting) they are the configured ones exactly.
	params.overcast        = weather_wanted() ? overcast_target : 0.0f;
	const float soften     = params.overcast_soften * params.overcast;
	params.shadow_strength = shadow_strength_config * (1.0f - soften);
	params.shadow_softness = shadow_softness_config * (1.0f + 2.0f * soften);
	// The clouds, with their masks by frame.
	const int         blur = static_cast<int>(std::lround(params.cloud_softness));
	const std::string key  = Game_key();
	if (blur != cloud_mask_blur || key != cloud_mask_game) {
		cloud_mask_cache.clear();
		set.masks.clear();
		set.mask_data.clear();
		cloud_mask_blur = blur;
		cloud_mask_game = key;
	}
	set.clouds.clear();
	uint64_t hash = 14695981039346656037ull;
	if (cloud_shadows_shown()) {
		const Clock::time_point start = Clock::now();
		for (const Shaded_cloud_source& c : in.clouds) {
			if (!c.frame || !c.frame->is_rle()) {
				continue;
			}
			const auto [it, inserted] = cloud_mask_cache.try_emplace(c.frame, static_cast<int32_t>(set.masks.size()));
			if (inserted) {
				Shaded_cloud_mask mask;
				Shaded_cpu::build_cloud_mask(
						c.frame->get_data(), c.frame->get_xleft(), c.frame->get_yabove(), c.frame->get_width(),
						c.frame->get_height(), blur, set.mask_data, mask);
				set.masks.push_back(mask);
			}
			set.clouds.push_back(Shaded_cloud{static_cast<float>(c.wx), static_cast<float>(c.wy), it->second, 0});
			const auto frame = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(c.frame));
			Hash_add(hash, static_cast<int32_t>(c.wx));
			Hash_add(hash, static_cast<int32_t>(c.wy));
			Hash_add(hash, static_cast<int32_t>(frame & 0xffffffffu));
			Hash_add(hash, static_cast<int32_t>(frame >> 32));
		}
		weather_prep_us += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
	}
	Hash_add(hash, static_cast<int32_t>(set.clouds.size()));
	cloud_hash         = hash;
	params.cloud_count = static_cast<int32_t>(set.clouds.size());
	// Fog round the Avatar, through the tables fitted to FOG.
	params.fog     = weather_wanted() && in.fog && params.fog_density > 0.0f && load_weather_refs() ? 1 : 0;
	params.fog_mix = params.fog ? fog_fit_mix : 0.0f;
	if (params.fog) {
		const Actor* avatar = gwin->get_main_actor();
		if (avatar) {
			const Tile_coord t = avatar->get_tile();
			params.avatar_x    = static_cast<float>(t.tx) + 0.5f;
			params.avatar_y    = static_cast<float>(t.ty) + 0.5f;
			params.avatar_z    = static_cast<float>(t.tz);
			fog_track          = {t.tx, t.ty, t.tz, gwin->get_scrolltx(), gwin->get_scrollty()};
		}
	}
	// The fog follows the Avatar: a step or a scroll changes every pixel,
	// as does the fog setting in or lifting.
	const bool fog_moved
			= lit_active && (params.fog ? fog_track : std::array<int, 5>{-1, -1, -1, -1, -1}) != lit_fog_track;
	// Lightning: a sky light from the flash's direction.
	const bool flash  = weather_wanted() && flash_on;
	params.flash      = flash ? 1.0f : 0.0f;
	params.flash_x    = flash ? flash_dir[0] : 0.0f;
	params.flash_y    = flash ? flash_dir[1] : 0.0f;
	params.flash_z    = flash ? flash_dir[2] : 0.0f;
	// Rain and snow are relit as they are painted.
	const bool particles = params.precipitation && (in.rain || in.snow);
	const bool terms      = !set.clouds.empty() || params.overcast > 0.0f || params.fog || flash || particles;
	params.weather_active = weather_wanted() && terms ? 1 : 0;
	return changed || fog_moved || (lit_active && cloud_hash != lit_cloud_hash);
}

/*
 *  Before the light pass of a paint with weather terms: the grid and the
 *  sky map over it (rebuilt when the grid changed or moved).
 */

void Shaded_light::prepare_weather_pass() {
	Shaded_weather_set& set = *weather_set;
	weather_set_on          = false;
	if (!params.weather_active) {
		return;
	}
	Shaded_grid* grid = Shaded_grid::active;
	if (grid) {
		grid->sync();
	}
	const Shaded_grid_volume* vol = grid && grid->get_volume().size_x > 0 ? &grid->get_volume() : nullptr;
	set.grid                      = vol;
	if (vol) {
		const int generation = grid->get_params().generation;
		if (generation != open_generation || vol->origin_tx != open_origin_tx || vol->origin_ty != open_origin_ty
			|| vol->size_x != open_size_x || vol->size_y != open_size_y || params.fog_fade != open_fade
			|| set.open.size() != static_cast<size_t>(vol->size_x) * static_cast<size_t>(vol->size_y)) {
			const Clock::time_point start = Clock::now();
			Shaded_cpu::build_sky_open(*vol, params.fog_fade, set.open);
			weather_prep_us += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
			open_generation = generation;
			open_origin_tx  = vol->origin_tx;
			open_origin_ty  = vol->origin_ty;
			open_size_x     = vol->size_x;
			open_size_y     = vol->size_y;
			open_fade       = params.fog_fade;
		}
		set.open_w  = vol->size_x;
		set.open_h  = vol->size_y;
		set.open_tx = vol->origin_tx;
		set.open_ty = vol->origin_ty;
	} else {
		set.open.clear();
		set.open_w  = 0;
		set.open_h  = 0;
		open_generation = -1;
	}
	// The drops and flakes painted this paint.
	set.particles  = params.precipitation ? buffers.particle_mask.data() : nullptr;
	weather_set_on = true;
}

/*
 *  A lightning flash starts (on) or ends.  Each flash comes from its own
 *  direction (golden-ratio steps round the compass) at a fixed elevation.
 */

bool Shaded_light::lightning(bool on) {
	if (on) {
		if (!weather_wanted() || !params.lightning_shadows || !lit_active) {
			return false;    // The LIGHTNING palette, as without shaded light.
		}
		flash_count++;
		constexpr double golden    = 0.618034;
		constexpr double two_pi    = 6.283185307179586;
		const double     turn      = static_cast<double>(flash_count) * golden;
		const double     azimuth   = two_pi * (turn - std::floor(turn));
		const double     elevation = static_cast<double>(shaded_flash_elevation) * two_pi / 360.0;
		flash_dir[0]               = static_cast<float>(std::cos(elevation) * std::cos(azimuth));
		flash_dir[1]               = static_cast<float>(std::cos(elevation) * std::sin(azimuth));
		flash_dir[2]               = static_cast<float>(std::sin(elevation)) / params.tiles_per_lift;
		flash_on                   = true;
		flash_shown                = false;
		flash_end_pending          = false;
		need_full                  = true;
		Game_window::get_instance()->set_all_dirty();
		std::cout << "SHADED: flash " << flash_count << " dir " << flash_dir[0] << "," << flash_dir[1] << ","
				  << flash_dir[2] << std::endl;
		return true;
	}
	if (!flash_on) {
		return false;
	}
	if (flash_shown) {
		end_flash();
	} else {
		flash_end_pending = true;    // finish_paint ends it once painted.
	}
	return true;
}

/*
 *  The flash is over: the next paint is full and without it.  (From
 *  finish_paint the dirty area is cleared after the paint, so that paint
 *  comes with the next dirty area: the storm's clouds move every 100 ms.)
 */

void Shaded_light::end_flash() {
	flash_on          = false;
	flash_shown       = false;
	flash_end_pending = false;
	need_full         = true;
	Game_window::get_instance()->set_all_dirty();
}

/*
 *  Rain_effect's drops of a recorded paint, between end_paint and
 *  finish_paint: mark their pixels while weather lighting relights them.
 */

void Shaded_light::begin_particles(bool lamp_lit) {
	Image_window8* win = Game_window::get_instance()->get_win();
	if (!lamp_lit || !weather_wanted() || !params.precipitation || !lit_active || !params.weather_active || !win
		|| !recorded || win->get_ib8() != recorded || buffers.particle_mask.size() != buffers.height_map.size()) {
		return;
	}
	in_particles = true;
	Shape_manager::get_instance()->set_paint_listener(this);
}

void Shaded_light::end_particles() {
	if (!in_particles) {
		return;
	}
	Shape_manager::get_instance()->set_paint_listener(nullptr);
	in_particles = false;
}

/*
 *  The flash's screen map over rectangle r (grown for the upsampling):
 *  each receiver traced towards the flash in screen space and through the
 *  grid, in bands, then blurred as the lamps' maps are.
 */

void Shaded_light::flash_pass(const Shaded_rect& r) {
	Shaded_weather_set& ws  = *weather_set;
	Shaded_shadow_set&  set = ws.flash;
	ws.flash_map            = -1;
	flash_traces            = 0;
	flash_pass_us           = 0;
	if (params.flash <= 0.0f || r.w <= 0 || r.h <= 0) {
		return;
	}
	const Clock::time_point start = Clock::now();
	Game_window*            gwin  = Game_window::get_instance();
	const int               gw    = gwin->get_width();
	const int               gh    = gwin->get_height();
	Shaded_grid*            grid  = Shaded_grid::active;
	const Shaded_grid_volume* vol = grid && grid->get_volume().size_x > 0 ? &grid->get_volume() : nullptr;
	const int               res   = params.shadow_resolution;
	set.resolution                = res;
	set.area_w                    = gw;
	set.area_h                    = gh;
	set.grid_tx                   = vol ? vol->origin_tx : 0;
	set.grid_ty                   = vol ? vol->origin_ty : 0;
	set.screen                    = 1;
	set.ray_grid                  = vol ? 1 : 0;
	set.strength                  = 1.0f;
	set.light_boxes.clear();
	set.light_map.assign(1, 0);
	set.light_field.assign(1, -1);
	set.maps.clear();
	set.fields.clear();
	set.field_data.clear();
	const Shaded_rect need = Intersect(Shaded_rect{r.x - 2 * res, r.y - 2 * res, r.w + 4 * res, r.h + 4 * res},
									   Shaded_rect{0, 0, gw, gh});
	if (need.w <= 0 || need.h <= 0) {
		set.map_data.clear();
		return;
	}
	Shaded_shadow_map m;
	m.x     = Floor_div(need.x, res);
	m.y     = Floor_div(need.y, res);
	m.w     = Floor_div(need.x + need.w - 1, res) - m.x + 1;
	m.h     = Floor_div(need.y + need.h - 1, res) - m.y + 1;
	m.build = 1;
	set.maps.push_back(m);
	set.map_data.assign(static_cast<size_t>(m.w) * static_cast<size_t>(m.h), 255);
	// The flash as a directional light (towards it, tiles, tiles, lifts).
	Shaded_light_record flash{};
	flash.x     = params.flash_x;
	flash.y     = params.flash_y;
	flash.z     = params.flash_z;
	flash.flags = shaded_light_directional;
	const Shaded_rect cells{m.x, m.y, m.w, m.h};
	const int         bands = params.threads > 1 ? std::min(params.threads, m.h) : 1;
	std::vector<long> traces(static_cast<size_t>(std::max(bands, 1)), 0);
	run_bands(bands, [&](int b) {
		Shaded_cpu::flash_shadow_rows(
				buffers, params, vol, flash, set, cells, cells.y + cells.h * b / bands, cells.y + cells.h * (b + 1) / bands,
				traces[static_cast<size_t>(b)]);
	});
	for (const long n : traces) {
		flash_traces += n;
	}
	if (params.shadow_softness > 0.0f) {
		shadow_scratch.resize(set.map_data.size());
		run_bands(bands, [&](int b) {
			Shaded_cpu::blur_shadow_rows(buffers, set, m, m.h * b / bands, m.h * (b + 1) / bands, shadow_scratch.data());
		});
		std::copy_n(shadow_scratch.begin(), set.map_data.size(), set.map_data.begin());
	}
	ws.flash_map  = 0;
	flash_pass_us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
	flash_us += flash_pass_us;
}

bool Shaded_light::lightable_palette(int n) {
	if (n == PALETTE_DUSK || n == PALETTE_NIGHT || n == PALETTE_SPELL) {
		return true;
	}
	const Shaded_light& sl = get();
	return sl.weather_wanted() && (n == PALETTE_OVERCAST || (n == PALETTE_DAY && sl.weather_present));
}

/*
 *  Turn lighting on or off; either way every pixel must be painted again.
 */

void Shaded_light::set_lit(bool on) {
	if (on == lit_active) {
		return;
	}
	lit_active = on;
	need_full  = true;
	Game_window::get_instance()->set_all_dirty();
}

/*
 *  Read the reference palettes of the ramp (raw 6-bit colours) when the
 *  game or mod has changed.  False if palettes.flx is not there yet.
 */

bool Shaded_light::load_refs() {
	const std::string key = Game_key();
	if (ref_pals.size() == std::size(shaded_ref_palettes) && key == refs_game) {
		return true;
	}
	ref_pals.clear();
	refs_game.clear();
	try {
		for (const int n : shaded_ref_palettes) {
			Palette tmp;
			tmp.load(PALETTES_FLX, PATCH_PALETTES, n);
			std::vector<uint8_t> colours(768);
			for (int i = 0; i < 256; i++) {
				colours[3 * i]     = tmp.get_red(i);
				colours[3 * i + 1] = tmp.get_green(i);
				colours[3 * i + 2] = tmp.get_blue(i);
			}
			ref_pals.push_back(std::move(colours));
		}
	} catch (...) {
		ref_pals.clear();
		return false;
	}
	// An unreadable palette loads as black; without DAY there is no ramp.
	const uint8_t* day = ref_pals[0].data();
	if (std::all_of(day + 3, day + 3 * (shaded_last_lit_index + 1), [](uint8_t c) {
			return c == 0;
		})) {
		ref_pals.clear();
		return false;
	}
	refs_game = key;
	return true;
}

/*
 *  Read the weather palettes (FOG and LIGHTNING, raw 6-bit colours) and
 *  fit the fog tables to FOG and the flash tables to LIGHTNING, once per
 *  game or mod.  False if they (or the
 *  ramp's refs) cannot be read yet.
 */

bool Shaded_light::load_weather_refs() {
	if (!load_refs()) {
		return false;
	}
	const std::string key = Game_key();
	if (weather_pals.size() == 2 && key == weather_refs_game && fog_table.size() == shaded_fog_table_size
		&& flash_table.size() == shaded_flash_table_size) {
		return true;
	}
	weather_pals.clear();
	weather_refs_game.clear();
	fog_table.clear();
	flash_table.clear();
	try {
		for (const int n : {PALETTE_FOG, PALETTE_LIGHTNING}) {
			Palette tmp;
			tmp.load(PALETTES_FLX, PATCH_PALETTES, n);
			std::vector<uint8_t> colours(768);
			for (int i = 0; i < 256; i++) {
				colours[3 * i]     = tmp.get_red(i);
				colours[3 * i + 1] = tmp.get_green(i);
				colours[3 * i + 2] = tmp.get_blue(i);
			}
			weather_pals.push_back(std::move(colours));
		}
	} catch (...) {
		weather_pals.clear();
		return false;
	}
	const Clock::time_point start = Clock::now();
	fog_table.resize(shaded_fog_table_size);
	flash_table.resize(shaded_flash_table_size);
	switch (backend()) {
	case Shaded_backend::gpu:    // backend() falls back to CPU.
	case Shaded_backend::cpu:
		Shaded_cpu::build_fog_tables(ref_pals[0].data(), weather_pals[0].data(), fog_fit_mix, fog_colour, fog_table.data());
		Shaded_cpu::build_flash_table(ref_pals[0].data(), weather_pals[1].data(), flash_table.data());
		break;
	}
	weather_prep_us += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
	weather_refs_game = key;
	return true;
}

/*
 *  The palette state machine: lighting takes over from the whole-screen
 *  light palettes while the clock shows a lightable palette (or a
 *  transition into or out of one) that is darker than day.
 */

bool Shaded_light::filter_palette(
		const unsigned char* in768, int palnum, bool world, bool transition, unsigned char* out768) {
	if (!lighting_on) {
		return false;
	}
	Game_window* gwin = Game_window::get_instance();
	if (!gwin->get_main_actor() || gwin->is_in_exult_menu()) {
		set_lit(false);
		return false;
	}
	if (world || transition) {
		seen_world_palette = true;
	}
	// Hit and lightning flashes show as they are over the lit frame.
	if (world && (palnum == PALETTE_RED || palnum == PALETTE_LIGHTNING)) {
		return false;
	}
	// The overcast softening follows the clock's palette (transitions
	// keep it as it was).
	if (world) {
		overcast_target = palnum == PALETTE_OVERCAST ? 1.0f : 0.0f;
	}
	// With weather lighting, overcast and weather by day are lit too, even
	// at full ambient (clouds dim the sky light).
	const bool weather_lit = weather_wanted()
							 && (palnum == PALETTE_OVERCAST || (palnum == PALETTE_DAY && weather_present)
								 || (transition && weather_present));
	// Day, fog, overcast, invisibility and custom palettes stay as they are.
	if (!((world && lightable_palette(palnum)) || transition)) {
		set_lit(false);
		return false;
	}
	clock_palette         = world ? palnum : -1;
	const std::string key = Game_key();
	const bool        same_ambient
			= have_ambient && ambient_pal.size() == 768 && std::equal(in768, in768 + 768, ambient_pal.begin());
	if (!same_ambient || remap_slider != params.ambient || remap_tint != params.night_tint || remap_game != key) {
		if (!load_refs()) {
			set_lit(false);
			return false;
		}
		ambient_pal.assign(in768, in768 + 768);
		have_ambient = true;
		remap.resize(shaded_remap_size);
		displayed_pal.resize(768);
		const uint8_t* refs[4] = {ref_pals[0].data(), ref_pals[1].data(), ref_pals[2].data(), ref_pals[3].data()};
		switch (backend()) {
		case Shaded_backend::gpu:    // backend() falls back to CPU.
		case Shaded_backend::cpu:
			Shaded_cpu::build_remap(ambient_pal.data(), refs, params, remap.data(), displayed_pal.data(), &ramp);
			break;
		}
		ambient_y    = Shaded_cpu::palette_luminance(ambient_pal.data(), refs[0]);
		remap_slider = params.ambient;
		remap_tint   = params.night_tint;
		remap_game   = key;
		if (lit_active) {    // The lit frame no longer matches.
			need_full = true;
			gwin->set_all_dirty();
		}
	}
	if (params.ambient_level >= 0.999f && !weather_lit) {
		set_lit(false);
		return false;
	}
	set_lit(true);
	std::memcpy(out768, displayed_pal.data(), 768);
	// The border entry is handled as without lighting.
	std::memcpy(out768 + 3 * 0xff, in768 + 3 * 0xff, 3);
	return true;
}

/*
 *  Match the buffers to the draw surface.  Returns false if the paint does
 *  not go into the draw surface (a redirected render target).
 */

bool Shaded_light::setup_buffers(Image_window8& win) {
	SDL_Surface*   surf = win.get_draw_surface();
	Image_buffer8* ib   = win.get_ib8();
	if (!surf || !surf->pixels || !ib || !ib->get_bits() || static_cast<int>(ib->get_line_width()) != surf->pitch) {
		return false;
	}
	const auto base   = reinterpret_cast<uintptr_t>(surf->pixels);
	const auto bits   = reinterpret_cast<uintptr_t>(ib->get_bits());
	const auto size   = static_cast<uintptr_t>(surf->pitch) * static_cast<uintptr_t>(surf->h);
	if (bits < base || bits - base >= size) {
		return false;
	}
	const int origin = static_cast<int>(bits - base);
	if (buffers.width != surf->w || buffers.height != surf->h || buffers.stride != surf->pitch || buffers.origin != origin) {
		const auto count = static_cast<size_t>(size);
		buffers.width    = surf->w;
		buffers.height   = surf->h;
		buffers.stride   = surf->pitch;
		buffers.origin   = origin;
		buffers.height_map.assign(count, 0);
		buffers.facing_map.assign(count, shaded_face_none);
		buffers.object_ids.assign(count, shaded_id_none);
		buffers.light_levels.assign(count, 0);
		buffers.shadow_levels.assign(count, 255);
		buffers.sky_levels.assign(count, 255);
		buffers.fog_codes.assign(count, 0);
		buffers.flash_levels.assign(count, 0);
		buffers.particle_mask.assign(count, 0);
		buffers.normal_map.assign(count, 0);
		buffers.objects.clear();
		buffers.object_opacity.clear();
		buffers.object_volumes.clear();
		buffers.object_flags.clear();
		object_extents.clear();
		object_index.clear();
		main_actor_id         = shaded_id_none;
		need_full             = true;    // Nothing recorded outside this clip.
		params.buffer_width   = buffers.width;
		params.buffer_height  = buffers.height;
		params.buffer_stride  = buffers.stride;
		params.buffer_origin  = buffers.origin;
	}
	return true;
}

void Shaded_light::begin_paint(Image_window8& win, bool full) {
	if (!record_on && !timing) {
		return;
	}
	paint_record_us = 0;
	rounded_pixels  = 0;
	seam_pixels     = 0;
	// (Re)allocating the buffers is not part of the timed paint.
	const bool ready = record_on && setup_buffers(win);
	const Clock::time_point start = Clock::now();
	paint_start                   = start;
	if (!ready) {
		return;
	}
	Game_window*   gwin = Game_window::get_instance();
	Image_buffer8* ib   = win.get_ib8();
	int            cx, cy, cw, ch;
	ib->get_clip(cx, cy, cw, ch);
	clip = Intersect(Shaded_rect{cx, cy, cw, ch}, buffers.bounds());
	if (full) {
		buffers.objects.clear();
		buffers.object_opacity.clear();
		buffers.object_volumes.clear();
		buffers.object_flags.clear();
		object_extents.clear();
		object_index.clear();
		main_actor_id = shaded_id_none;
		need_full     = false;
	} else if (gwin->get_scrolltx() != params.scroll_tx || gwin->get_scrollty() != params.scroll_ty) {
		// Scrolling copies the frame and paints only the new strip, so move
		// the record the same way.
		auto delta = [](int now, int before) {
			int d = (now - before) % c_num_tiles;
			if (d > c_num_tiles / 2) {
				d -= c_num_tiles;
			} else if (d < -c_num_tiles / 2) {
				d += c_num_tiles;
			}
			return d * c_tilesize;
		};
		shift(delta(gwin->get_scrolltx(), params.scroll_tx), delta(gwin->get_scrollty(), params.scroll_ty));
	}
	params.scroll_tx = gwin->get_scrolltx();
	params.scroll_ty = gwin->get_scrollty();
	params.scroll_px = gwin->get_scrolltx_lo();
	params.scroll_py = gwin->get_scrollty_lo();
	switch (backend()) {
	case Shaded_backend::gpu:    // backend() falls back to CPU.
	case Shaded_backend::cpu:
		Shaded_cpu::clear_rect(buffers, clip);
		for (int y = clip.y; y < clip.y + clip.h; y++) {
			std::fill_n(buffers.particle_mask.begin() + buffers.index(clip.x, y), clip.w, 0);
		}
		break;
	}
	target = ib;
	active = this;
	if (timing) {
		paint_record_us += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
	}
}

/*
 *  Move the record's game area by (-dx, -dy) pixels, as the frame was.
 */

void Shaded_light::shift(int dx, int dy) {
	const Shaded_rect area = buffers.bounds();
	if (std::abs(dx) >= area.w || std::abs(dy) >= area.h) {
		Shaded_cpu::clear_rect(buffers, area);
		return;
	}
	const int w     = area.w - std::abs(dx);
	const int src_x = area.x + std::max(dx, 0);
	const int dst_x = area.x + std::max(-dx, 0);
	// The weather outputs and the drop mask move like the light levels.
	std::vector<uint8_t>* const weather_buffers[4]
			= {&buffers.sky_levels, &buffers.fog_codes, &buffers.flash_levels, &buffers.particle_mask};
	auto      move_row = [&](int src_y, int dst_y) {
		const int from = buffers.index(src_x, src_y);
		const int to   = buffers.index(dst_x, dst_y);
		std::copy_n(buffers.height_map.begin() + from, w, buffers.height_map.begin() + to);
		std::copy_n(buffers.facing_map.begin() + from, w, buffers.facing_map.begin() + to);
		std::copy_n(buffers.object_ids.begin() + from, w, buffers.object_ids.begin() + to);
		std::copy_n(buffers.light_levels.begin() + from, w, buffers.light_levels.begin() + to);
		std::copy_n(buffers.shadow_levels.begin() + from, w, buffers.shadow_levels.begin() + to);
		std::copy_n(buffers.normal_map.begin() + from, w, buffers.normal_map.begin() + to);
		for (std::vector<uint8_t>* b : weather_buffers) {
			std::copy_n(b->begin() + from, w, b->begin() + to);
		}
	};
	const int rows = area.h - std::abs(dy);
	if (dy > 0 || (dy == 0 && dx > 0)) {    // Moving up or left: go forwards.
		for (int r = 0; r < rows; r++) {
			move_row(area.y + r + dy, area.y + r);
		}
	} else if (dy < 0) {
		for (int r = rows - 1; r >= 0; r--) {
			move_row(area.y + r, area.y + r - dy);
		}
	} else if (dx < 0) {    // Same row, moving right: copy_n can overlap.
		std::vector<uint16_t> h16(static_cast<size_t>(w));
		std::vector<uint8_t>  f8(static_cast<size_t>(w));
		std::vector<uint16_t> i16(static_cast<size_t>(w));
		std::vector<uint8_t>  l8(static_cast<size_t>(w));
		std::vector<uint8_t>  s8(static_cast<size_t>(w));
		std::vector<uint16_t> n16(static_cast<size_t>(w));
		for (int r = 0; r < rows; r++) {
			const int from = buffers.index(src_x, area.y + r);
			const int to   = buffers.index(dst_x, area.y + r);
			std::copy_n(buffers.height_map.begin() + from, w, h16.begin());
			std::copy_n(buffers.facing_map.begin() + from, w, f8.begin());
			std::copy_n(buffers.object_ids.begin() + from, w, i16.begin());
			std::copy_n(buffers.light_levels.begin() + from, w, l8.begin());
			std::copy_n(buffers.shadow_levels.begin() + from, w, s8.begin());
			std::copy_n(buffers.normal_map.begin() + from, w, n16.begin());
			std::copy(h16.begin(), h16.end(), buffers.height_map.begin() + to);
			std::copy(f8.begin(), f8.end(), buffers.facing_map.begin() + to);
			std::copy(i16.begin(), i16.end(), buffers.object_ids.begin() + to);
			std::copy(l8.begin(), l8.end(), buffers.light_levels.begin() + to);
			std::copy(s8.begin(), s8.end(), buffers.shadow_levels.begin() + to);
			std::copy(n16.begin(), n16.end(), buffers.normal_map.begin() + to);
			for (std::vector<uint8_t>* b : weather_buffers) {
				std::copy_n(b->begin() + from, w, s8.begin());
				std::copy(s8.begin(), s8.end(), b->begin() + to);
			}
		}
	}
}

void Shaded_light::end_paint(Image_window8& win, bool full) {
	if (!record_on && !timing) {
		return;
	}
	const Clock::time_point stop = Clock::now();
	const bool              was_active = active == this;
	recorded                           = was_active ? target : nullptr;    // For the drops.
	active                             = nullptr;
	target                             = nullptr;
	if (in_object) {
		end_object();
	}
	if (timing) {
		Image_buffer8* ib = win.get_ib8();
		int            cx, cy, cw, ch;
		ib->get_clip(cx, cy, cw, ch);
		const double us = std::chrono::duration<double, std::micro>(stop - paint_start).count();
		paints++;
		paint_pixels += static_cast<double>(cw) * ch;
		all_paint_us += us;
		all_record_us += paint_record_us;
		if (full) {
			game_width  = Game_window::get_instance()->get_width();
			game_height = Game_window::get_instance()->get_height();
			full_paints++;
			paint_map_us += us;
			record_us += paint_record_us;
		}
		// Every 5 s: Exult may quit without running static destructors.
		if (paints == 1) {
			last_report = stop;
		} else if (stop - last_report >= std::chrono::seconds(5)) {
			last_report = stop;
			report_timing();
		}
	}
	if (!was_active) {
		return;
	}
	if (params.rounded && params.roof_seams) {
		switch (backend()) {
		case Shaded_backend::gpu:    // backend() falls back to CPU.
		case Shaded_backend::cpu:
			Shaded_cpu::merge_roof_seams(buffers, clip, seam_pixels);
			break;
		}
	}
	params.object_count = static_cast<int32_t>(buffers.objects.size());
	finish_due          = true;
}

/*
 *  Light the rows of r into levels, in bands on the workers if threaded
 *  and r is big enough, then relight frame's pixels if it is not null.
 *  Returns the number of bands.
 */

int Shaded_light::light_pass(
		const Shaded_rect& r, uint8_t* levels, bool threaded, unsigned char* frame, const Shaded_shadow_set* shadows,
		uint8_t* shadow_out, const Shaded_weather_set* weather, const Shaded_weather_out* wout) {
	if (r.w <= 0 || r.h <= 0) {
		return 0;
	}
	const bool fog_on = params.fog && fog_table.size() == shaded_fog_table_size;
	auto       band   = [&](int b, int bands) {
		const int y0 = r.y + r.h * b / bands;
		const int y1 = r.y + r.h * (b + 1) / bands;
		Shaded_cpu::light_rows(
				buffers, params, lights.data(), buffers.bin_offsets.data(), buffers.bin_lights.data(), r, y0, y1, levels,
				shadows, shadow_out, weather, wout);
		if (frame && weather) {
			// The fog table only while there is fog.  A flash is the traced
			// level lift alone: the flash table is not applied, as BG's
			// LIGHTNING palette sends lit browns to DAY's magentas (0x5a).
			Shaded_cpu::apply_weather_rows(
					buffers, r, y0, y1, levels, remap.data(), wout ? wout->fog : nullptr, fog_on ? fog_table.data() : nullptr,
					nullptr, nullptr, frame);
		} else if (frame) {
			Shaded_cpu::apply_light_rows(buffers, r, y0, y1, levels, remap.data(), frame);
		}
	};
	const int bands = threaded && params.threads > 1 && r.w * r.h >= 4096 ? std::min(params.threads, r.h) : 1;
	if (bands <= 1) {
		band(0, 1);
		return 1;
	}
	if (!workers || workers->size() != params.threads) {
		workers = std::make_unique<Shaded_workers>(params.threads);
	}
	workers->run(bands, [&](int b) {
		band(b, bands);
	});
	return bands;
}

void Shaded_light::run_bands(int bands, const std::function<void(int)>& job) {
	if (bands <= 1) {
		job(0);
		return;
	}
	if (!workers || workers->size() != params.threads) {
		workers = std::make_unique<Shaded_workers>(params.threads);
	}
	workers->run(bands, job);
}

/*
 *  Trace the shadows the lights cast into rectangle r: grid fields around
 *  each light and screen maps over r (grown for the upsampling), reusing
 *  cached ones whose inputs are unchanged when use_cache.  False if there
 *  are no shadows to apply.
 */

bool Shaded_light::shadow_pass(
		const Shaded_rect& r, Shaded_shadow_set& set, bool threaded, bool use_cache, Shadow_stats& stats) {
	stats = Shadow_stats{};
	if (!shadows_wanted() || r.w <= 0 || r.h <= 0) {
		return false;
	}
	Shaded_grid* grid = Shaded_grid::active;
	if (grid) {
		grid->sync();
	}
	const Shaded_grid_volume* vol = grid && grid->get_volume().size_x > 0 ? &grid->get_volume() : nullptr;
	Game_window*              gwin       = Game_window::get_instance();
	const int                 gw         = gwin->get_width();
	const int                 gh         = gwin->get_height();
	const bool                use_screen = shadow_parts != shaded_shadow_parts_grid;
	const bool                use_grid   = shadow_parts != shaded_shadow_parts_screen && vol;
	const bool                ray        = use_grid && shadow_grid_ray;
	const bool                use_fields = use_grid && !ray;
	const int                 res        = params.shadow_resolution;
	const int                 generation = grid ? grid->get_params().generation : 0;
	set.resolution                       = res;
	set.area_w                           = gw;
	set.area_h                           = gh;
	set.grid_tx                          = vol ? vol->origin_tx : 0;
	set.grid_ty                          = vol ? vol->origin_ty : 0;
	set.screen                           = use_screen ? 1 : 0;
	set.ray_grid                         = ray ? 1 : 0;
	set.strength                         = params.shadow_strength;
	set.light_map.assign(lights.size(), -1);
	set.light_field.assign(lights.size(), -1);
	set.light_boxes.assign(lights.size(), Shaded_light_box{});
	for (size_t i = 0; i < lights.size(); i++) {
		if (lights[i].flags & shaded_light_carried) {
			continue;    // Held: the bearer is not in the grid.
		}
		const Game_object& owner = *light_objects[i];
		const Tile_coord   t     = owner.get_tile();
		const Shape_info&  info  = owner.get_info();
		const int          frame = owner.get_framenum();
		Shaded_light_box&  box   = set.light_boxes[i];
		box.lo[0]                = static_cast<float>(t.tx + 1 - info.get_3d_xtiles(frame));
		box.lo[1]                = static_cast<float>(t.ty + 1 - info.get_3d_ytiles(frame));
		box.lo[2]                = static_cast<float>(t.tz);
		box.hi[0]                = static_cast<float>(t.tx + 1);
		box.hi[1]                = static_cast<float>(t.ty + 1);
		box.hi[2]                = static_cast<float>(t.tz + info.get_3d_height());
		box.valid                = 1;
	}
	set.maps.clear();
	set.fields.clear();
	set.map_data.clear();
	set.field_data.clear();
	if (use_cache) {
		shadow_paints++;
	}
	// The generation is in every field key and only moves forward, so the
	// fields of an older one can never be hit again.
	if (use_fields && generation != field_generation) {
		field_cache.clear();
		field_generation = generation;
	}
	const Shaded_rect grown{r.x - 2 * res, r.y - 2 * res, r.w + 4 * res, r.h + 4 * res};
	std::vector<Shadow_key> field_keys;    // Of the fields and maps built.
	std::vector<Shadow_key> map_keys;
	auto                    q16 = [](float v) {
		return static_cast<int64_t>(std::lround(v * 16.0f));
	};
	for (size_t i = 0; i < lights.size(); i++) {
		const Shaded_light_record& l  = lights[i];
		const Shaded_rect          lr = Shaded_cpu::light_screen_rect(params, l, gw, gh);
		if (lr.w <= 0 || Intersect(lr, r).w <= 0) {
			continue;
		}
		stats.lights++;
		const float      radius = l.radius * params.radius_scale;
		const Shadow_key base{l.shape, l.frame, q16(l.x), q16(l.y), q16(l.z), q16(radius), q16(l.intensity), l.flags};
		if (use_fields && !(l.flags & shaded_light_directional)) {
			// The cells within reach of the light, clipped to the grid.
			const float lx  = Shaded_wrap_delta(l.x - static_cast<float>(vol->origin_tx));
			const float ly  = Shaded_wrap_delta(l.y - static_cast<float>(vol->origin_ty));
			const float zlo = (l.flags & shaded_light_area) ? l.z - 1.0f : l.z - radius / params.tiles_per_lift;
			const float zhi = (l.flags & shaded_light_area) ? l.z + params.area_height : l.z + radius / params.tiles_per_lift;
			const int   x0  = std::max(static_cast<int>(std::floor(lx - radius)), 0);
			const int   y0  = std::max(static_cast<int>(std::floor(ly - radius)), 0);
			const int   z0  = std::max(static_cast<int>(std::floor(zlo)), 0);
			const int   x1  = std::min(static_cast<int>(std::floor(lx + radius)), vol->size_x - 1);
			const int   y1  = std::min(static_cast<int>(std::floor(ly + radius)), vol->size_y - 1);
			const int   z1  = std::min(static_cast<int>(std::floor(zhi)), vol->size_z - 1);
			if (x0 <= x1 && y0 <= y1 && z0 <= z1) {
				Shaded_shadow_field f;
				f.x      = x0;
				f.y      = y0;
				f.z      = z0;
				f.w      = x1 - x0 + 1;
				f.h      = y1 - y0 + 1;
				f.d      = z1 - z0 + 1;
				f.light  = static_cast<int32_t>(i);
				f.offset = static_cast<uint32_t>(set.field_data.size());
				const size_t size = static_cast<size_t>(f.w) * f.h * f.d;
				Shadow_key   key  = base;
				key.insert(key.end(), {generation, vol->origin_tx, vol->origin_ty, x0, y0, z0, f.w, f.h, f.d});
				const auto hit = use_cache ? field_cache.find(key) : field_cache.end();
				if (hit != field_cache.end() && hit->second.data.size() == size) {
					set.field_data.insert(set.field_data.end(), hit->second.data.begin(), hit->second.data.end());
					hit->second.used = shadow_paints;
					stats.fields_reused++;
				} else {
					set.field_data.resize(set.field_data.size() + size);
					f.build = 1;
					stats.fields_built++;
					if (use_cache) {
						field_keys.push_back(std::move(key));
					}
				}
				set.light_field[i] = static_cast<int32_t>(set.fields.size());
				set.fields.push_back(f);
			}
		}
		if (!use_screen && !ray) {
			continue;
		}
		// Map cells over the light's rectangle within the grown paint.
		const Shaded_rect need = Intersect(lr, grown);
		if (need.w <= 0) {
			continue;
		}
		const int cx0 = Floor_div(need.x, res);
		const int cy0 = Floor_div(need.y, res);
		const int cx1 = Floor_div(need.x + need.w - 1, res);
		const int cy1 = Floor_div(need.y + need.h - 1, res);
		Shaded_shadow_map m;
		m.x     = cx0;
		m.y     = cy0;
		m.w     = cx1 - cx0 + 1;
		m.h     = cy1 - cy0 + 1;
		m.light = static_cast<int32_t>(i);
		Shadow_key key;
		if (use_cache) {
			// The record where the traces can look: the light's rectangle.
			uint64_t hash = 14695981039346656037ull;
			for (int y = lr.y; y < lr.y + lr.h; y++) {
				const int idx = buffers.index(lr.x, y);
				for (int k = idx; k < idx + lr.w; k++) {
					hash = (hash
							^ (static_cast<uint64_t>(buffers.height_map[k]) | static_cast<uint64_t>(buffers.object_ids[k]) << 16
							   | static_cast<uint64_t>(buffers.normal_map[k]) << 32))
						   * 1099511628211ull;
				}
			}
			key = base;
			key.insert(key.end(), {static_cast<int64_t>(light_hash), static_cast<int64_t>(hash), params.scroll_tx, params.scroll_ty,
								   params.scroll_px, params.scroll_py, res, params.shadow_lights, params.shadow_steps,
								   q16(params.shadow_softness * 256.0f), q16(params.shadow_bias * 256.0f),
								   q16(params.facing_min * 256.0f), q16(params.area_height), set.screen, set.ray_grid,
								   ray ? generation : 0, ray ? vol->origin_tx : 0, ray ? vol->origin_ty : 0, params.rounded,
								   params.roof_seams});
		}
		const auto hit = use_cache ? map_cache.find(key) : map_cache.end();
		if (hit != map_cache.end() && hit->second.map.x <= m.x && hit->second.map.y <= m.y
			&& hit->second.map.x + hit->second.map.w >= m.x + m.w && hit->second.map.y + hit->second.map.h >= m.y + m.h) {
			m.x = hit->second.map.x;
			m.y = hit->second.map.y;
			m.w = hit->second.map.w;
			m.h = hit->second.map.h;
			m.offset = static_cast<uint32_t>(set.map_data.size());
			set.map_data.insert(set.map_data.end(), hit->second.data.begin(), hit->second.data.end());
			hit->second.used = shadow_paints;
			stats.maps_reused++;
		} else {
			m.offset = static_cast<uint32_t>(set.map_data.size());
			m.build  = 1;
			set.map_data.resize(set.map_data.size() + static_cast<size_t>(m.w) * m.h, 255);
			if (use_cache) {
				map_keys.push_back(std::move(key));
			}
		}
		stats.maps++;
		stats.cells += static_cast<long>(m.w) * m.h;
		set.light_map[i] = static_cast<int32_t>(set.maps.size());
		set.maps.push_back(m);
	}
	const int bands = threaded && params.threads > 1 ? params.threads : 1;
	// Grid fields: bands of field rows across the fields being built.
	const Clock::time_point field_start = Clock::now();
	if (stats.fields_built > 0) {
		run_bands(bands, [&](int b) {
			for (const Shaded_shadow_field& f : set.fields) {
				if (f.build) {
					Shaded_cpu::grid_field_rows(
							*vol, params, lights[f.light], set.light_boxes[f.light], f, f.h * b / bands, f.h * (b + 1) / bands,
							set.field_data.data());
				}
			}
		});
	}
	const Clock::time_point screen_start = Clock::now();
	stats.field_us = std::chrono::duration<double, std::micro>(screen_start - field_start).count();
	// Screen maps: bands of cell rows over all maps being built.
	int cx0 = std::numeric_limits<int>::max(), cy0 = cx0;
	int cx1 = std::numeric_limits<int>::min(), cy1 = cx1;
	for (const Shaded_shadow_map& m : set.maps) {
		if (m.build) {
			cx0 = std::min(cx0, m.x);
			cy0 = std::min(cy0, m.y);
			cx1 = std::max(cx1, m.x + m.w);
			cy1 = std::max(cy1, m.y + m.h);
		}
	}
	if (cx1 > cx0) {
		const Shaded_rect cells{cx0, cy0, cx1 - cx0, cy1 - cy0};
		std::vector<long> traces(static_cast<size_t>(bands), 0);
		run_bands(bands, [&](int b) {
			Shaded_cpu::screen_shadow_rows(
					buffers, params, lights.data(), buffers.bin_offsets.data(), buffers.bin_lights.data(),
					ray ? vol : nullptr, set, cells, cells.y + cells.h * b / bands, cells.y + cells.h * (b + 1) / bands,
					traces[static_cast<size_t>(b)]);
		});
		for (const long n : traces) {
			stats.traces += n;
		}
		// Soften the screen-space part, keeping to each surface.
		if (use_screen && params.shadow_softness > 0.0f) {
			shadow_scratch.resize(set.map_data.size());
			run_bands(bands, [&](int b) {
				for (const Shaded_shadow_map& m : set.maps) {
					if (m.build) {
						Shaded_cpu::blur_shadow_rows(
								buffers, set, m, m.h * b / bands, m.h * (b + 1) / bands, shadow_scratch.data());
					}
				}
			});
			for (const Shaded_shadow_map& m : set.maps) {
				if (m.build) {
					std::copy_n(shadow_scratch.begin() + m.offset, static_cast<size_t>(m.w) * m.h, set.map_data.begin() + m.offset);
				}
			}
		}
	}
	stats.screen_us = std::chrono::duration<double, std::micro>(Clock::now() - screen_start).count();
	if (!use_cache) {
		return true;
	}
	// Keep what was built; drop fields unused for 64 paints, maps for 8.
	size_t next = 0;
	for (const Shaded_shadow_field& f : set.fields) {
		if (f.build) {
			Shadow_cache_entry& entry = field_cache[field_keys[next++]];
			entry.field               = f;
			entry.data.assign(set.field_data.begin() + f.offset,
							  set.field_data.begin() + f.offset + static_cast<size_t>(f.w) * f.h * f.d);
			entry.used = shadow_paints;
		}
	}
	next = 0;
	for (const Shaded_shadow_map& m : set.maps) {
		if (m.build) {
			Shadow_cache_entry& entry = map_cache[map_keys[next++]];
			entry.map                 = m;
			entry.data.assign(set.map_data.begin() + m.offset, set.map_data.begin() + m.offset + static_cast<size_t>(m.w) * m.h);
			entry.used = shadow_paints;
		}
	}
	auto age = [this](std::map<Shadow_key, Shadow_cache_entry>& cache, long long limit) {
		for (auto it = cache.begin(); it != cache.end();) {
			it = shadow_paints - it->second.used > limit ? cache.erase(it) : std::next(it);
		}
	};
	age(field_cache, 64);
	age(map_cache, 8);
	return true;
}

/*
 *  The shadow self-checks for the dump info, over rectangle r of the paint
 *  just lit with shadow_set.
 */

void Shaded_light::check_shadows(const Shaded_rect& r) {
	std::ostringstream out;
	const char*        parts = shadow_parts == shaded_shadow_parts_screen ? "screen"
							   : (shadow_parts == shaded_shadow_parts_grid ? "grid" : "both");
	out << "shadows " << (shadow_set_on ? "on" : "off") << " res " << params.shadow_resolution << " lights "
		<< params.shadow_lights << " strength " << params.shadow_strength << " softness " << params.shadow_softness
		<< " parts " << parts << " grid " << (shadow_grid_ray ? "ray" : "field") << " cache "
		<< (shadow_cache ? "on" : "off") << "\n";
	if (!shadow_set_on) {
		shadow_info = out.str();
		return;
	}
	const Shadow_stats& st = shadow_stats;
	out << "shadow lights traced " << st.lights << " maps " << st.maps << " cells " << st.cells << " fields built "
		<< st.fields_built << " reused " << st.fields_reused << "\n"
		<< "shadow maps reused " << st.maps_reused << " screen traces " << st.traces << " field_us " << st.field_us
		<< " screen_us " << st.screen_us << "\n";
	auto same_in = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
		for (int y = r.y; y < r.y + r.h; y++) {
			const int idx = buffers.index(r.x, y);
			if (!std::equal(a.begin() + idx, a.begin() + idx + r.w, b.begin() + idx)) {
				return false;
			}
		}
		return true;
	};
	// The same weather terms as the paint's pass.
	const Shaded_weather_set* weather = weather_set_on ? weather_set.get() : nullptr;
	// One thread, no caches: the same levels and shadow shares.
	Shaded_shadow_set    single;
	Shadow_stats         single_stats;
	std::vector<uint8_t> levels(buffers.light_levels);
	std::vector<uint8_t> shares(buffers.shadow_levels);
	if (shadow_pass(r, single, false, false, single_stats)) {
		light_pass(r, levels.data(), false, nullptr, &single, shares.data(), weather);
	}
	const bool threads_same = same_in(levels, buffers.light_levels) && same_in(shares, buffers.shadow_levels);
	out << "check shadows threads identical " << (threads_same ? "yes" : "no") << " (" << params.threads
		<< " threads, no cache)\n";
	// Strength 0 must give the levels without shadows; with shadows no
	// pixel may be brighter than without.
	std::vector<uint8_t> unshadowed(buffers.light_levels);
	light_pass(r, unshadowed.data(), false, nullptr, nullptr, nullptr, weather);
	Shaded_shadow_set none = shadow_set;
	none.strength          = 0.0f;
	std::vector<uint8_t> zero(buffers.light_levels);
	light_pass(r, zero.data(), false, nullptr, &none, nullptr, weather);
	out << "check shadows strength0 identical " << (same_in(zero, unshadowed) ? "yes" : "no") << "\n";
	long brighter = 0, darker = 0;
	for (int y = r.y; y < r.y + r.h; y++) {
		const int idx = buffers.index(r.x, y);
		for (int i = idx; i < idx + r.w; i++) {
			brighter += buffers.light_levels[i] > unshadowed[i] ? 1 : 0;
			darker += buffers.light_levels[i] < unshadowed[i] ? 1 : 0;
		}
	}
	out << "check shadows darker violations " << brighter << " (pixels darkened " << darker << ")\n";
	// The grid fields against a grid ray per pixel and light, over about
	// 2000 lit pixels.
	const Shaded_grid* grid = Shaded_grid::active;
	if (shadow_set.fields.empty() || !grid) {
		out << "check grid field error not measured (no fields)\n";
		shadow_info = out.str();
		return;
	}
	const Shaded_grid_volume& vol = grid->get_volume();
	// The pixels some light with a field reaches, then 2000 spread evenly
	// over them.
	auto field_lights = [&](int x, int y, const std::function<void(int, const Shaded_point&)>& visit) {
		const int     idx  = buffers.index(x, y);
		const uint8_t face = buffers.facing_map[idx] & shaded_face_mask;
		if (face == shaded_face_none) {
			return;
		}
		const Shaded_point  p   = Pixel_world(buffers, params, x, y);
		const Shaded_normalf n  = Shaded_pixel_normal(buffers, idx, face);
		const int           id  = buffers.object_ids[idx];
		const int           bin = std::clamp(y / params.bin_size, 0, params.bins_y - 1) * params.bins_x
						+ std::clamp(x / params.bin_size, 0, params.bins_x - 1);
		for (uint32_t k = buffers.bin_offsets[bin]; k < buffers.bin_offsets[bin + 1]; k++) {
			const int                  li = buffers.bin_lights[k];
			const Shaded_light_record& l  = lights[li];
			if (shadow_set.light_field[li] >= 0 && !(id != shaded_id_none && l.object_id == id)
				&& Unshadowed(params, l, p, n) > 0.0f) {
				visit(li, p);
			}
		}
	};
	std::vector<std::pair<int, int>> lit;
	for (int y = r.y; y < r.y + r.h; y++) {
		for (int x = r.x; x < r.x + r.w; x++) {
			bool reached = false;
			field_lights(x, y, [&reached](int, const Shaded_point&) {
				reached = true;
			});
			if (reached) {
				lit.emplace_back(x, y);
			}
		}
	}
	const size_t samples = std::min<size_t>(lit.size(), 2000);
	long         pairs = 0, off = 0;
	for (size_t i = 0; i < samples; i++) {
		const auto [x, y] = lit[i * lit.size() / samples];
		field_lights(x, y, [&](int li, const Shaded_point& p) {
			const float traced = Shaded_cpu::grid_transmittance(vol, lights[li], shadow_set.light_boxes[li], p);
			const float field  = Shaded_cpu::shadow_grid_at(shadow_set, shadow_set.fields[shadow_set.light_field[li]], p);
			pairs++;
			off += std::abs(traced - field) > 0.5f ? 1 : 0;
		});
	}
	const long pixels = static_cast<long>(samples);
	out << "check grid field error " << (pairs > 0 ? 100.0 * static_cast<double>(off) / static_cast<double>(pairs) : 0.0)
		<< "% of " << pairs << " (pixel, light) pairs at " << pixels << " lit pixels (of " << lit.size() << ")\n";
	shadow_info = out.str();
}

/*
 *  The weather state and self-checks for the dump info, over rectangle r
 *  of the paint just lit.
 */

void Shaded_light::check_weather(const Shaded_rect& r) {
	std::ostringstream          out;
	const Shaded_weather_input& in  = *weather_input;
	const Shaded_weather_set&   set = *weather_set;
	out << "weather " << (weather_wanted() ? "on" : "off") << " active " << (weather_set_on ? "yes" : "no") << " clouds "
		<< set.clouds.size() << " (effect " << (in.clouds_effect ? "yes" : "no") << ", " << in.clouds.size()
		<< " reported) fog " << (in.fog ? "yes" : "no") << " rain " << (in.rain ? "yes" : "no") << " snow "
		<< (in.snow ? "yes" : "no") << " flash " << (params.flash > 0.0f ? flash_count : 0) << " overcast "
		<< params.overcast << "\n"
		<< "night_tint " << params.night_tint << " ambient_y " << ambient_y << "\n"
		<< "weather cloud_shadows " << params.cloud_shadows << " cloud_softness " << params.cloud_softness
		<< " overcast_soften " << params.overcast_soften << " fade " << params.fog_fade << " shadow strength "
		<< params.shadow_strength << " softness " << params.shadow_softness << " (config " << shadow_strength_config
		<< ", " << shadow_softness_config << ")\n"
		<< "weather sky map " << set.open_w << "x" << set.open_h << " at " << set.open_tx << "," << set.open_ty << " grid "
		<< (set.grid ? "yes" : "no") << " masks " << set.masks.size() << " (" << set.mask_data.size() << " bytes)\n";
	const long scroll_x = static_cast<long>(params.scroll_tx) * c_tilesize + params.scroll_px;
	const long scroll_y = static_cast<long>(params.scroll_ty) * c_tilesize + params.scroll_py;
	for (size_t i = 0; i < set.clouds.size(); i++) {
		const Shaded_cloud&      c = set.clouds[i];
		const Shaded_cloud_mask& m = set.masks[static_cast<size_t>(c.mask)];
		out << "cloud " << i << " world px " << c.wx << "," << c.wy << " screen "
			<< static_cast<long>(c.wx) - scroll_x << "," << static_cast<long>(c.wy) - scroll_y << " mask " << c.mask << " "
			<< m.w << "x" << m.h << " at " << m.x0 << "," << m.y0 << "\n";
	}
	if (!weather_set_on) {
		out << "check weather not measured (no weather terms this paint)\n";
		weather_info = out.str();
		return;
	}
	// Covered pixels (no sky, no flash) must light exactly as without the
	// weather: one thread, the same shadows.
	std::vector<uint8_t> plain(buffers.light_levels);
	light_pass(r, plain.data(), false, nullptr, shadow_set_on ? &shadow_set : nullptr, nullptr);
	long   outdoor = 0, covered = 0, violations = 0, fogged = 0;
	long   flash_lit = 0, flash_shadowed = 0, flash_covered = 0;    // Faces turned to the flash.
	double sky_sum = 0;
	const float flash_l[3] = {params.flash_x, params.flash_y, params.flash_z * params.tiles_per_lift};
	long particles = 0, relit = 0;    // Drop and flake pixels, and those lit apart.
	for (int y = r.y; y < r.y + r.h; y++) {
		const int idx = buffers.index(r.x, y);
		for (int i = idx; i < idx + r.w; i++) {
			fogged += buffers.fog_codes[i] != 0 ? 1 : 0;
			if (buffers.particle_mask[i] != 0) {
				particles++;
				relit += plain[i] != buffers.light_levels[i] ? 1 : 0;
			}
			const uint8_t face = buffers.facing_map[i] & shaded_face_mask;
			if (face == shaded_face_none) {
				continue;
			}
			if (params.flash > 0.0f) {
				const Shaded_normalf n = Shaded_pixel_normal(buffers, i, face);
				if (n.x * flash_l[0] + n.y * flash_l[1] + n.z * flash_l[2] > 0.0f) {
					const bool lit = buffers.flash_levels[i] != 0;
					flash_lit += lit ? 1 : 0;
					flash_shadowed += lit ? 0 : 1;
					flash_covered += lit && buffers.sky_levels[i] == 0 ? 1 : 0;
				}
			}
			if (buffers.sky_levels[i] == 0) {
				// Nor any fog under cover.
				covered++;
				violations += (buffers.flash_levels[i] == 0 && plain[i] != buffers.light_levels[i]) || buffers.fog_codes[i] != 0
									  ? 1
									  : 0;
			} else {
				outdoor++;
				sky_sum += buffers.sky_levels[i];
			}
		}
	}
	out << "weather fog " << (params.fog ? "on" : "off") << " mix " << params.fog_mix << " colour " << fog_colour[0] << ","
		<< fog_colour[1] << "," << fog_colour[2] << " fogged px " << fogged << " avatar " << params.avatar_x << ","
		<< params.avatar_y << "," << params.avatar_z << "\n";
	if (params.flash > 0.0f) {
		// Faces turned to the flash: lit by it, or in its shadow; and
		// those lit under cover (through an opening).
		out << "weather flash " << flash_count << " dir " << params.flash_x << "," << params.flash_y << "," << params.flash_z
			<< " lit px " << flash_lit << " shadowed px " << flash_shadowed << " (lit under cover " << flash_covered
			<< ") traces " << flash_traces << " flash_us " << flash_pass_us << "\n";
	}
	out << "weather precipitation " << (in.rain ? "rain" : in.snow ? "snow" : "none") << " ("
		<< (params.precipitation ? "on" : "off") << ") particle px " << particles << " relit " << relit << "\n";
	out << "weather outdoor mean sky " << (outdoor > 0 ? sky_sum / static_cast<double>(outdoor) : 0.0) << "\n"
		<< "check weather indoor violations " << violations << " (outdoor px " << outdoor << ", covered px " << covered
		<< ")\n";
	weather_info = out.str();
}

void Shaded_light::finish_paint(Image_window8& win, bool full) {
	recorded = nullptr;
	if (!finish_due) {
		return;
	}
	finish_due                    = false;
	const Clock::time_point start = Clock::now();
	// Due a dump?  Wanted on a full paint; full paints can be rare (the
	// game repaints only dirty areas), so after a second's wait a partial
	// paint does: the record outside its clip is still that of the frame.
	bool due = false;
	if (!dump_dir.empty()) {
		if (!clock_started && full) {
			clock_started = true;
			first_paint   = start;
		}
		if (clock_started && dump_next < dump_at.size()) {
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(start - first_paint).count();
			due           = ms >= dump_at[dump_next] && (full || ms >= dump_at[dump_next] + 1000);
		}
	}
	// The first full paints of lightning flashes are dumped too, as 100 on
	// (EXULT_SHADED_DUMP_FLASH).
	const bool flash_dump = !due && !dump_dir.empty() && flash_dumps_left > 0 && full && lit_active && params.flash > 0.0f;
	const bool checks     = due || flash_dump;
	const int  dump_number
			= flash_dump ? shaded_flash_dump_first + flash_dumps_taken : static_cast<int>(dump_next);
	// The pass runs while lighting is active, for the light views and for
	// dumps; it relights the frame only while active and not viewing unlit.
	const auto view  = static_cast<Shaded_view>(params.view);
	const bool apply = lit_active && view != Shaded_view::unlit && remap.size() == shaded_remap_size;
	const bool pass  = lit_active || view == Shaded_view::light || view == Shaded_view::unlit
					  || view == Shaded_view::shadow || view == Shaded_view::weather || due;
	auto*      frame = static_cast<unsigned char*>(win.get_draw_surface()->pixels);
	params.lit       = apply ? 1 : 0;
	relit_pixels     = 0;
	unlit_frame.clear();
	if (checks && apply) {
		unlit_frame.assign(frame, frame + buffers.height_map.size());
	}
	int bands      = 0;
	shadow_set_on  = false;
	weather_set_on = false;
	const Shaded_weather_out wout{buffers.sky_levels.data(), buffers.fog_codes.data(), buffers.flash_levels.data()};
	if (pass) {
		// The lights' own objects, as recorded in this frame (0 = not painted).
		for (size_t i = 0; i < lights.size(); i++) {
			const auto it       = object_index.find(light_objects[i]);
			const int  id       = it != object_index.end() ? it->second : shaded_id_none;
			lights[i].object_id = id == shaded_id_overflow ? shaded_id_none : id;
		}
		Game_window* gwin  = Game_window::get_instance();
		params.light_count = static_cast<int32_t>(lights.size());
		params.bins_x      = (gwin->get_width() + params.bin_size - 1) / params.bin_size;
		params.bins_y      = (gwin->get_height() + params.bin_size - 1) / params.bin_size;
		double shadow_us = 0;
		switch (backend()) {
		case Shaded_backend::gpu:    // backend() falls back to CPU.
		case Shaded_backend::cpu: {
			Shaded_cpu::build_bins(params, lights, buffers.bin_offsets, buffers.bin_lights);
			const Clock::time_point shadow_start = Clock::now();
			shadow_set_on = shadow_pass(clip, shadow_set, true, shadow_cache, shadow_stats);
			shadow_us     = std::chrono::duration<double, std::micro>(Clock::now() - shadow_start).count();
			if (!shadow_set_on) {    // The shadow view shows no shadow.
				for (int y = clip.y; y < clip.y + clip.h; y++) {
					std::fill_n(buffers.shadow_levels.begin() + buffers.index(clip.x, y), clip.w, 255);
				}
			}
			prepare_weather_pass();
			if (weather_set_on && params.flash > 0.0f) {
				flash_pass(clip);
			} else {
				weather_set->flash_map = -1;
				flash_pass_us          = 0;
			}
			if (!weather_set_on) {    // No weather: open sky, no fog, no flash.
				for (int y = clip.y; y < clip.y + clip.h; y++) {
					const int idx = buffers.index(clip.x, y);
					std::fill_n(buffers.sky_levels.begin() + idx, clip.w, 255);
					std::fill_n(buffers.fog_codes.begin() + idx, clip.w, 0);
					std::fill_n(buffers.flash_levels.begin() + idx, clip.w, 0);
				}
			}
			bands = light_pass(
					clip, buffers.light_levels.data(), true, apply ? frame : nullptr, shadow_set_on ? &shadow_set : nullptr,
					shadow_set_on ? buffers.shadow_levels.data() : nullptr, weather_set_on ? weather_set.get() : nullptr,
					weather_set_on ? &wout : nullptr);
			break;
		}
		}
		if (apply) {
			lit_hash       = light_hash;
			lit_sliders    = slider_values();
			lit_cloud_hash = cloud_hash;
			lit_fog_track  = params.fog ? fog_track : std::array<int, 5>{-1, -1, -1, -1, -1};
			lit_grid       = Shaded_grid::active ? Shaded_grid::active->get_params().generation : -1;
		}
		if (timing) {
			// The light pass without the shadows, which are timed apart.
			const double us
					= std::chrono::duration<double, std::micro>(Clock::now() - start).count() - shadow_us - flash_pass_us;
			light_passes++;
			all_light_us += us;
			if (full) {
				full_light_passes++;
				full_light_us += us;
			}
			if (shadow_set_on) {
				shadow_passes++;
				all_shadow_us += shadow_us;
				shadow_field_us += shadow_stats.field_us;
				shadow_screen_us += shadow_stats.screen_us;
				shadow_cache_hits += shadow_stats.fields_reused + shadow_stats.maps_reused;
				if (full) {
					full_shadow_passes++;
					full_shadow_us += shadow_us;
				}
			}
		}
		// The flash has been painted: it may end now.
		flash_shown = flash_shown || (flash_on && weather_set_on && params.flash > 0.0f);
	}
	if (checks) {
		// The threaded pass must give what one thread does.
		if (bands > 1) {
			std::vector<uint8_t>     single(buffers.light_levels);
			std::vector<uint8_t>     single_sky(buffers.sky_levels);
			std::vector<uint8_t>     single_fog(buffers.fog_codes);
			std::vector<uint8_t>     single_flash(buffers.flash_levels);
			const Shaded_weather_out single_out{single_sky.data(), single_fog.data(), single_flash.data()};
			light_pass(
					clip, single.data(), false, nullptr, shadow_set_on ? &shadow_set : nullptr, nullptr,
					weather_set_on ? weather_set.get() : nullptr, weather_set_on ? &single_out : nullptr);
			bool same = true;
			for (int y = clip.y; y < clip.y + clip.h && same; y++) {
				const int idx = buffers.index(clip.x, y);
				same          = std::equal(
						 single.begin() + idx, single.begin() + idx + clip.w, buffers.light_levels.begin() + idx)
					   && std::equal(
							   single_sky.begin() + idx, single_sky.begin() + idx + clip.w, buffers.sky_levels.begin() + idx)
					   && std::equal(
							   single_fog.begin() + idx, single_fog.begin() + idx + clip.w, buffers.fog_codes.begin() + idx)
					   && std::equal(
							   single_flash.begin() + idx, single_flash.begin() + idx + clip.w,
							   buffers.flash_levels.begin() + idx);
			}
			thread_check = std::string("check threads identical ") + (same ? "yes" : "no") + " ("
						   + std::to_string(params.threads) + " threads, " + std::to_string(bands) + " bands)";
		} else {
			thread_check = "check threads not compared (" + std::to_string(params.threads) + " threads, "
						   + std::to_string(bands) + " band)";
		}
		if (!unlit_frame.empty()) {
			for (int y = clip.y; y < clip.y + clip.h; y++) {
				const int idx = buffers.index(clip.x, y);
				for (int i = idx; i < idx + clip.w; i++) {
					relit_pixels += unlit_frame[i] != frame[i] ? 1 : 0;
				}
			}
		}
		check_shadows(clip);
		check_weather(clip);
		dump(win, dump_number, false, full);
		thread_check.clear();
		shadow_info.clear();
		weather_info.clear();
	}
	if (view != Shaded_view::normal && view != Shaded_view::unlit) {
		uint8_t lut[lut_size];
		build_lut(lut);
		switch (backend()) {
		case Shaded_backend::gpu:    // backend() falls back to CPU.
		case Shaded_backend::cpu:
			Shaded_cpu::resolve_view(buffers, params, clip, lut, frame);
			break;
		}
		if (checks) {
			dump(win, dump_number, true, full);
		}
	}
	if (due) {
		dump_next++;
		unlit_frame.clear();
		if (timing) {
			report_timing();
		}
	}
	if (flash_dump) {
		flash_dumps_left--;
		flash_dumps_taken++;
		unlit_frame.clear();
	}
	// A flash that ended before it was painted ends now it has been.
	if (flash_on && flash_shown && flash_end_pending) {
		end_flash();
	}
}

/*
 *  Relight a world paint made outside Game_window::paint (the crystal
 *  ball's display_area, save thumbnails).  Nothing is recorded for it, so
 *  every pixel gets the ambient level the displayed palette expects.
 */

void Shaded_light::relight_ambient(Image_window8& win, int x, int y, int w, int h) {
	const auto view = static_cast<Shaded_view>(params.view);
	if (!lit_active || view == Shaded_view::unlit || remap.size() != shaded_remap_size || !setup_buffers(win)) {
		return;
	}
	const Shaded_rect r = Intersect(Shaded_rect{x, y, w, h}, buffers.bounds());
	if (r.w <= 0 || r.h <= 0) {
		return;
	}
	auto* frame = static_cast<unsigned char*>(win.get_draw_surface()->pixels);
	switch (backend()) {
	case Shaded_backend::gpu:    // backend() falls back to CPU.
	case Shaded_backend::cpu:
		Shaded_cpu::apply_level_rows(buffers, params, r, r.y, r.y + r.h, params.ambient_level, remap.data(), frame);
		break;
	}
}

void Shaded_light::begin_object(const Game_object& obj, int ox, int oy) {
	if (active != this) {
		return;
	}
	const Clock::time_point start = timing ? Clock::now() : Clock::time_point();
	const Shape_info&       info  = obj.get_info();
	const int               frame = obj.get_framenum();
	const Tile_coord        t     = obj.get_tile();
	const Shaded_object_record rec{
			t.tx, t.ty, t.tz, info.get_3d_xtiles(frame), info.get_3d_ytiles(frame), info.get_3d_height(), obj.get_shapenum(), frame};
	const Shaded_volume volume  = volume_for(obj);
	const uint8_t       opacity = Screen_opacity(obj, info, frame, volume);
	const Shaded_grid*  grid    = Shaded_grid::active;
	const bool          roof    = params.rounded && params.roof_seams
							&& (info.occludes() || rec.ztiles == 1
								|| (grid && (grid->get_rules().lookup(obj.get_shapenum(), frame).flags & shaded_cell_roof)));
	const uint8_t       flags   = static_cast<uint8_t>(
            (volume != Shaded_volume::box ? shaded_object_rounded : 0) | (roof ? shaded_object_roof : 0));
	auto [it, inserted]         = object_index.try_emplace(&obj, shaded_id_overflow);
	if (inserted && buffers.objects.size() < shaded_id_overflow - 1) {
		buffers.objects.push_back(rec);
		buffers.object_opacity.push_back(opacity);
		buffers.object_volumes.emplace_back();
		buffers.object_flags.push_back(flags);
		object_extents.emplace_back();
		it->second = static_cast<uint16_t>(buffers.objects.size());
	} else if (!inserted && it->second != shaded_id_overflow) {
		buffers.objects[it->second - 1]        = rec;    // Moved or changed frame.
		buffers.object_opacity[it->second - 1] = opacity;
		buffers.object_volumes[it->second - 1] = Shaded_object_volume{};
		buffers.object_flags[it->second - 1]   = flags;
		object_extents[it->second - 1]         = Shaded_rounded_extent{};
	}
	object.ox          = ox;
	object.oy          = oy;
	object.xpx         = rec.xtiles * c_tilesize;
	object.ypx         = rec.ytiles * c_tilesize;
	object.hpx         = rec.ztiles * (c_tilesize / 2);
	object.base_height = rec.tz * shaded_height_units_per_lift;
	object.id          = it->second;
	object.volume      = volume;
	object.ax_px       = 0.5f - 0.5f * static_cast<float>(object.xpx);
	object.ay_px       = 0.5f - 0.5f * static_cast<float>(object.ypx);
	object_extent      = Shaded_rounded_extent{};
	if (&obj == Game_window::get_instance()->get_main_actor()) {
		main_actor_id = object.id;
	}
	in_object = true;
	Shape_manager::get_instance()->set_paint_listener(this);
	if (timing) {
		paint_record_us += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
	}
}

void Shaded_light::end_object() {
	Shape_manager::get_instance()->set_paint_listener(nullptr);
	in_object = false;
	if (object.volume == Shaded_volume::box || object.id == shaded_id_none || object.id == shaded_id_overflow
		|| object.id > buffers.objects.size()) {
		return;
	}
	// The shadow volume: an ellipsoid over the pixels' extent (a tree's
	// crown), and for a tree the trunk capsule under it.
	const Shaded_object_record& o   = buffers.objects[object.id - 1];
	const Shaded_rounded_extent& e  = object_extent;
	Shaded_object_volume         v;
	const Shaded_rounded_extent::Range& main = object.volume == Shaded_volume::tree && !e.crown.empty() ? e.crown : e.body;
	object_extents[object.id - 1]            = e;
	if (main.empty()) {
		buffers.object_volumes[object.id - 1] = v;    // Nothing rounded painted: its box.
		return;
	}
	const float tile = static_cast<float>(c_tilesize);
	const float lift = tile / 2.0f;    // Pixels per lift.
	v.centre[0]      = static_cast<float>(o.tx + 1) - 0.5f * static_cast<float>(o.xtiles) + 0.5f * (main.lo[0] + main.hi[0]) / tile;
	v.centre[1]      = static_cast<float>(o.ty + 1) - 0.5f * static_cast<float>(o.ytiles) + 0.5f * (main.lo[1] + main.hi[1]) / tile;
	v.centre[2]      = static_cast<float>(o.tz) + 0.5f * (main.lo[2] + main.hi[2]) / lift;
	for (int i = 0; i < 3; i++) {
		// Half a pixel more: pixel centres lie inside.
		v.radius[i] = 0.5f * (main.hi[i] - main.lo[i]) / tile + 0.5f / tile;
	}
	if (object.volume == Shaded_volume::tree && !e.crown.empty() && !e.body.empty()) {
		v.trunk_radius = e.trunk_radius / tile;
		v.trunk_top    = v.centre[2];    // Up into the crown.
	}
	v.kind                                = static_cast<int32_t>(object.volume);
	buffers.object_volumes[object.id - 1] = v;
}

/*
 *  The rounded geometry of a frame painted at (xoff, yoff) for the
 *  current object, built once and cached.
 */

const Shaded_silhouette& Shaded_light::silhouette_for(Shape_frame& shape, int xoff, int yoff, int skip_from) {
	if (silhouettes.size() > 2048) {
		silhouettes.clear();
	}
	const Silhouette_key key{&shape,     skip_from,         static_cast<int>(object.volume), object.xpx,
							 object.ypx, xoff - object.ox, yoff - object.oy};
	auto [it, inserted] = silhouettes.try_emplace(key);
	if (inserted) {
		Shaded_cpu::build_silhouette(
				shape.get_data(), shape.get_xleft(), shape.get_yabove(), shape.get_width(), shape.get_height(), skip_from,
				object.volume, object.xpx, object.ypx, object.ax_px - static_cast<float>(key.du),
				object.ay_px - static_cast<float>(key.dv), it->second);
	}
	return it->second;
}

void Shaded_light::shape_painted(int xoff, int yoff, Shape_frame& shape, bool translucent, int xfcnt) {
	if (in_particles) {
		// A rain drop or snowflake: only where it is, not what it is.
		if (shape.is_rle() && Game_window::get_instance()->get_win()->get_ib8() == recorded) {
			switch (backend()) {
			case Shaded_backend::gpu:    // backend() falls back to CPU.
			case Shaded_backend::cpu:
				Shaded_cpu::mark_spans(buffers, clip, shape.get_data(), xoff, yoff);
				break;
			}
		}
		return;
	}
	// Only shapes painted for the current world object, into the frame.
	if (!in_object || active != this || Game_window::get_instance()->get_win()->get_ib8() != target) {
		return;
	}
	const Clock::time_point start = timing ? Clock::now() : Clock::time_point();
	switch (backend()) {
	case Shaded_backend::gpu:    // backend() falls back to CPU.
	case Shaded_backend::cpu:
		if (shape.is_rle() && object.volume != Shaded_volume::box) {
			const int                skip = translucent ? 0xff - xfcnt : 0x100;
			const Shaded_silhouette& sil  = silhouette_for(shape, xoff, yoff, skip);
			Shaded_cpu::record_rounded(buffers, clip, object, sil, shape.get_data(), xoff, yoff, skip, rounded_pixels);
			object_extent.body.add(sil.extent.body, 0.0f, 0.0f);
			object_extent.crown.add(sil.extent.crown, 0.0f, 0.0f);
			object_extent.trunk_radius = std::max(object_extent.trunk_radius, sil.extent.trunk_radius);
		} else if (shape.is_rle()) {
			Shaded_cpu::record_spans(buffers, clip, object, shape.get_data(), xoff, yoff, translucent ? 0xff - xfcnt : 0x100);
		} else {    // An 8x8 flat tile, drawn up and left of its origin.
			const Shaded_rect r = Intersect(Shaded_rect{xoff - c_tilesize, yoff - c_tilesize, c_tilesize, c_tilesize}, clip);
			Shaded_cpu::fill_plane(
					buffers, r, static_cast<uint16_t>(object.base_height), shaded_face_ground, object.id);
		}
		break;
	}
	if (timing) {
		paint_record_us += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
	}
}

void Shaded_light::record_plane(int x, int y, int w, int h, int lift, uint8_t face) {
	if (active != this) {
		return;
	}
	const Clock::time_point start = timing ? Clock::now() : Clock::time_point();
	const Shaded_rect       r     = Intersect(Shaded_rect{x, y, w, h}, clip);
	switch (backend()) {
	case Shaded_backend::gpu:    // backend() falls back to CPU.
	case Shaded_backend::cpu:
		Shaded_cpu::fill_plane(
				buffers, r, static_cast<uint16_t>(std::min(lift * shaded_height_units_per_lift, 0xffff)), face, shaded_id_none);
		break;
	}
	if (timing) {
		paint_record_us += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
	}
}

void Shaded_light::world_position(int x, int y, float& wx, float& wy, float& wz) const {
	const int idx = buffers.index(x, y);
	wz            = static_cast<float>(buffers.height_map[idx]) / params.height_units_per_lift;
	const float lift_px = wz * params.pixels_per_lift;
	wx = static_cast<float>(params.scroll_tx)
		 + (static_cast<float>(x + params.scroll_px) + 0.5f + lift_px) / params.pixels_per_tile;
	wy = static_cast<float>(params.scroll_ty)
		 + (static_cast<float>(y + params.scroll_py) + 0.5f + lift_px) / params.pixels_per_tile;
	const auto tiles = static_cast<float>(c_num_tiles);
	wx               = std::fmod(wx + tiles, tiles);
	wy               = std::fmod(wy + tiles, tiles);
}

/*
 *  Palette index for each false colour code of the current view.
 */

void Shaded_light::build_lut(uint8_t* lut) const {
	const Palette* pal   = Game_window::get_instance()->get_pal();
	const int      scale = pal->get_max_val();
	for (int code = 0; code < lut_size; code++) {
		Rgb col{0, 0, 0};
		switch (static_cast<Shaded_view>(params.view)) {
		case Shaded_view::height:
			if (code > 0) {
				col = Height_colour(static_cast<float>(code - 1) / (lut_size - 2));
			}
			break;
		case Shaded_view::facing:
			col = Facing_colour(static_cast<uint8_t>(code));
			break;
		case Shaded_view::object_id:
			if (code > 0) {
				col = Id_colour(static_cast<unsigned>(code) * 7919u);
			}
			break;
		case Shaded_view::light:
		case Shaded_view::shadow:
			if (code > 0) {
				const int grey = (code - 1) * 255 / (lut_size - 2);
				col            = Rgb{grey, grey, grey};
			}
			break;
		case Shaded_view::normals:
			if (code > 0) {
				col = Rgb{((code >> 4) & 3) * 85, ((code >> 2) & 3) * 85, (code & 3) * 85};
			}
			break;
		case Shaded_view::weather:
			if (code == lut_size - 1) {
				col = Rgb{255, 255, 255};
			} else if (code >= 48) {
				const int c = 96 + (code - 48) * 159 / 14;    // Cyan ramp.
				col         = Rgb{0, c, c};
			} else if (code > 0) {
				const int grey = (code - 1) * 255 / 46;
				col            = Rgb{grey, grey, grey};
			}
			break;
		case Shaded_view::normal:
		case Shaded_view::unlit:
		case Shaded_view::count:
			break;
		}
		const int r = col.r * scale / 255, g = col.g * scale / 255, b = col.b * scale / 255;
		// While lit, the screen shows the displayed palette, not pal's.
		const int index = lit_active && displayed_pal.size() == 768
								  ? Shaded_cpu::nearest_colour(displayed_pal.data(), r, g, b, 0, shaded_last_lit_index)
								  : pal->find_color(r, g, b);
		lut[code]       = static_cast<uint8_t>(std::max(index, 0));
	}
}

/*
 *  Write the record (or, with view, the resolved debug frame) as images.
 */

void Shaded_light::dump(Image_window8& win, int number, bool view, bool full) {
	Game_window*  gwin   = Game_window::get_instance();
	const int     gw     = gwin->get_width();
	const int     gh     = gwin->get_height();
	const auto*   frame  = static_cast<const unsigned char*>(win.get_draw_surface()->pixels);
	const Palette* pal   = gwin->get_pal();
	const int     maxval = std::max(pal->get_max_val(), 1);
	const std::string prefix = dump_dir + "/shaded-" + std::to_string(number) + "-";
	auto pixel_rgb           = [](unsigned char* out, const Rgb& c) {
		out[0] = static_cast<unsigned char>(c.r);
		out[1] = static_cast<unsigned char>(c.g);
		out[2] = static_cast<unsigned char>(c.b);
	};
	auto image = [&](const char* name, auto&& colour) {
		std::vector<unsigned char> rgb(static_cast<size_t>(gw) * gh * 3);
		for (int y = 0; y < gh; y++) {
			for (int x = 0; x < gw; x++) {
				pixel_rgb(&rgb[(static_cast<size_t>(y) * gw + x) * 3], colour(buffers.index(x, y)));
			}
		}
#ifdef HAVE_PNG_H
		if (!Shaded_write_png(prefix + name + ".png", gw, gh, false, rgb)) {
			std::cout << "SHADED: could not write " << prefix << name << ".png" << std::endl;
		}
#else
		ignore_unused_variable_warning(name);
#endif
	};
	// The frame through the palette on screen: B' while lit, else pal's.
	const bool shown_lit    = lit_active && displayed_pal.size() == 768;
	auto       frame_colour = [&](int idx) {
		const int pix = frame[idx];
		if (shown_lit) {
			return Rgb{displayed_pal[3 * pix] * 255 / maxval, displayed_pal[3 * pix + 1] * 255 / maxval,
					   displayed_pal[3 * pix + 2] * 255 / maxval};
		}
		return Rgb{pal->get_red(pix) * 255 / maxval, pal->get_green(pix) * 255 / maxval, pal->get_blue(pix) * 255 / maxval};
	};
#ifndef HAVE_PNG_H
	std::cout << "SHADED: PNG support not built; dump skipped" << std::endl;
	ignore_unused_variable_warning(image, frame_colour, view, full);
	return;
#else
	if (view) {
		image("view", frame_colour);
		std::cout << "SHADED: dump " << number << " view " << view_name(static_cast<Shaded_view>(params.view)) << " written to "
				  << prefix << "view.png" << std::endl;
		return;
	}
	image("normal", frame_colour);
	if (!unlit_frame.empty() && ambient_pal.size() == 768) {
		// The frame before the pass, through the ambient palette.
		image("unlit", [&](int idx) {
			const int pix = unlit_frame[idx];
			return Rgb{ambient_pal[3 * pix] * 255 / maxval, ambient_pal[3 * pix + 1] * 255 / maxval,
					   ambient_pal[3 * pix + 2] * 255 / maxval};
		});
	}
	image("height", [&](int idx) {
		if ((buffers.facing_map[idx] & shaded_face_mask) == shaded_face_none) {
			return Rgb{0, 0, 0};
		}
		return Height_colour(
				static_cast<float>(buffers.height_map[idx]) / params.height_units_per_lift / params.debug_height_range);
	});
	image("facing", [&](int idx) {
		const uint8_t facing = buffers.facing_map[idx];
		Rgb           col    = Facing_colour(facing);
		if ((facing & shaded_face_mask) == shaded_face_rounded) {
			// Rounded: brighter where the surface faces up.
			const float shade = 0.5f + 0.5f * Shaded_decode_normal(buffers.normal_map[idx]).z;
			col               = Rgb{static_cast<int>(static_cast<float>(col.r) * shade), static_cast<int>(static_cast<float>(col.g) * shade),
									static_cast<int>(static_cast<float>(col.b) * shade)};
		}
		return col;
	});
	image("normals", [&](int idx) {
		const uint8_t face = buffers.facing_map[idx] & shaded_face_mask;
		if (face == shaded_face_none) {
			return Rgb{0, 0, 0};
		}
		const Shaded_normalf n = Shaded_pixel_normal(buffers, idx, face);
		auto                 c = [](float v) {
            return std::clamp(static_cast<int>(std::lround((v * 0.5f + 0.5f) * 255.0f)), 0, 255);
		};
		return Rgb{c(n.x), c(n.y), c(n.z)};
	});
	image("id", [&](int idx) {
		return Id_colour(buffers.object_ids[idx]);
	});
	const int top_level = std::max(params.level_count - 1, 1);
	image("light", [&](int idx) {
		const int grey = buffers.light_levels[idx] * 255 / top_level;
		return Rgb{grey, grey, grey};
	});
	image("shadow", [&](int idx) {
		const int grey = buffers.shadow_levels[idx];
		return Rgb{grey, grey, grey};
	});
	if (lit_active) {
		// Sky factor: black under cover, darker under clouds.
		image("sky", [&](int idx) {
			const int grey = buffers.sky_levels[idx];
			return Rgb{grey, grey, grey};
		});
	}
	if (lit_active && params.fog) {
		// Fog amount: black clear, brighter with more fog.
		image("fog", [&](int idx) {
			const int grey = (buffers.fog_codes[idx] & 7) * 36;
			return Rgb{grey, grey, grey};
		});
	}
	if (lit_active && params.flash > 0.0f) {
		// Flash level: black in its shadows and away from it.
		image("flash", [&](int idx) {
			const int grey = std::min(buffers.flash_levels[idx] * 36, 255);
			return Rgb{grey, grey, grey};
		});
	}
	std::vector<unsigned char> grey(static_cast<size_t>(gw) * gh * 2);
	for (int y = 0; y < gh; y++) {
		for (int x = 0; x < gw; x++) {
			const uint16_t h   = buffers.height_map[buffers.index(x, y)];
			const size_t   out = (static_cast<size_t>(y) * gw + x) * 2;
			grey[out]          = static_cast<unsigned char>(h >> 8);
			grey[out + 1]      = static_cast<unsigned char>(h & 0xff);
		}
	}
	if (!Shaded_write_png(prefix + "height16.png", gw, gh, true, grey)) {
		std::cout << "SHADED: could not write " << prefix << "height16.png" << std::endl;
	}
	// Facts for checking the geometry: counts per face, and the avatar's
	// pixels rebuilt into world positions, which should lie in its box.
	long  faces[8] = {};
	long  avatar_px = 0, avatar_clamped = 0, avatar_rounded = 0;
	long  box_px = 0, box_outside = 0;    // Unclamped object pixels.
	long  clamped_px = 0;
	long  rounded_px = 0, rounded_outside = 0;    // Rounded pixels, outside their fitted extent.
	float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
	for (int y = 0; y < gh; y++) {
		for (int x = 0; x < gw; x++) {
			const int idx = buffers.index(x, y);
			faces[buffers.facing_map[idx] & shaded_face_mask]++;
			clamped_px += (buffers.facing_map[idx] & shaded_face_clamped) ? 1 : 0;
			const uint16_t id      = buffers.object_ids[idx];
			const bool     rounded = (buffers.facing_map[idx] & shaded_face_mask) == shaded_face_rounded;
			if (rounded && id != shaded_id_none && id != shaded_id_overflow && id <= object_extents.size()) {
				// Rebuilt position must lie in the bounds of the object's
				// fitted volume (body and crown) plus a quarter tile.
				const Shaded_object_record&  o = buffers.objects[id - 1];
				const Shaded_rounded_extent& e = object_extents[id - 1];
				float                        w[3];
				world_position(x, y, w[0], w[1], w[2]);
				const float c[3] = {Shaded_wrap_delta(w[0] - (static_cast<float>(o.tx + 1) - 0.5f * static_cast<float>(o.xtiles)))
											* c_tilesize,
									Shaded_wrap_delta(w[1] - (static_cast<float>(o.ty + 1) - 0.5f * static_cast<float>(o.ytiles)))
											* c_tilesize,
									(w[2] - static_cast<float>(o.tz)) * (c_tilesize / 2)};
				auto        in = [&c](const Shaded_rounded_extent::Range& r) {
					constexpr float slack = 0.25f * c_tilesize;
					for (int i = 0; i < 3; i++) {
						if (r.empty() || c[i] < r.lo[i] - slack || c[i] > r.hi[i] + slack) {
							return false;
						}
					}
					return true;
				};
				rounded_px++;
				rounded_outside += in(e.body) || in(e.crown) ? 0 : 1;
			}
			if (id != shaded_id_none && id != shaded_id_overflow && !rounded
				&& !(buffers.facing_map[idx] & (shaded_face_clamped | shaded_face_merged))) {
				// Rebuilt position must lie in the object's box (within
				// half a pixel; world x/y wrap is ignored here).
				const Shaded_object_record& o = buffers.objects[id - 1];
				float                       w[3];
				world_position(x, y, w[0], w[1], w[2]);
				constexpr float e = 0.07f;
				box_px++;
				if (w[0] < static_cast<float>(o.tx + 1 - o.xtiles) - e || w[0] > static_cast<float>(o.tx + 1) + e
					|| w[1] < static_cast<float>(o.ty + 1 - o.ytiles) - e || w[1] > static_cast<float>(o.ty + 1) + e
					|| w[2] < static_cast<float>(o.tz) - e || w[2] > static_cast<float>(o.tz + o.ztiles) + e) {
					box_outside++;
				}
			}
			if (main_actor_id == shaded_id_none || buffers.object_ids[idx] != main_actor_id) {
				continue;
			}
			avatar_px++;
			if (buffers.facing_map[idx] & shaded_face_clamped) {
				avatar_clamped++;
				continue;
			}
			if ((buffers.facing_map[idx] & shaded_face_mask) == shaded_face_rounded) {
				avatar_rounded++;
				continue;
			}
			float w[3];
			world_position(x, y, w[0], w[1], w[2]);
			for (int i = 0; i < 3; i++) {
				lo[i] = std::min(lo[i], w[i]);
				hi[i] = std::max(hi[i], w[i]);
			}
		}
	}
	// Rounded pixels within reach of a light (the nearest one not their
	// own): their mean light level where the normal faces it (n.l > 0.3)
	// and where it faces away (n.l < -0.1).
	long   lit_toward = 0, lit_away = 0;
	double level_toward = 0, level_away = 0;
	if (params.bins_x > 0 && params.bins_y > 0 && buffers.bin_offsets.size() > static_cast<size_t>(params.bins_x * params.bins_y)) {
		for (int y = 0; y < gh; y++) {
			for (int x = 0; x < gw; x++) {
				const int idx = buffers.index(x, y);
				if ((buffers.facing_map[idx] & shaded_face_mask) != shaded_face_rounded) {
					continue;
				}
				const Shaded_point   p   = Pixel_world(buffers, params, x, y);
				const Shaded_normalf n   = Shaded_pixel_normal(buffers, idx, shaded_face_rounded);
				const int            id  = buffers.object_ids[idx];
				const int            bin = std::clamp(y / params.bin_size, 0, params.bins_y - 1) * params.bins_x
								+ std::clamp(x / params.bin_size, 0, params.bins_x - 1);
				float best = 1e30f, dot = 0.0f;
				for (uint32_t k = buffers.bin_offsets[bin]; k < buffers.bin_offsets[bin + 1]; k++) {
					const Shaded_light_record& l = lights[buffers.bin_lights[k]];
					if (l.object_id == id || (l.flags & (shaded_light_area | shaded_light_off | shaded_light_directional))) {
						continue;
					}
					const float dx = Shaded_wrap_delta(l.x - p.x);
					const float dy = Shaded_wrap_delta(l.y - p.y);
					const float dz = (l.z - p.z) * params.tiles_per_lift;
					const float d2 = dx * dx + dy * dy + dz * dz;
					const float r  = l.radius * params.radius_scale;
					if (d2 < r * r && d2 < best && d2 > 1e-6f) {
						best = d2;
						dot  = (n.x * dx + n.y * dy + n.z * dz) / std::sqrt(d2);
					}
				}
				if (best >= 1e30f) {
					continue;
				}
				if (dot > 0.3f) {
					lit_toward++;
					level_toward += buffers.light_levels[idx];
				} else if (dot < -0.1f) {
					lit_away++;
					level_away += buffers.light_levels[idx];
				}
			}
		}
	}
	long kinds[shaded_volume_kinds] = {};    // Fitted volumes by kind.
	for (const Shaded_object_volume& v : buffers.object_volumes) {
		kinds[std::clamp(v.kind, 0, shaded_volume_kinds - 1)]++;
	}
	std::ofstream info(prefix + "info.txt");
	info << "game " << gw << "x" << gh << "\n"
		 << "paint " << (full ? "full" : "partial (frame outside its clip may hold earlier sprites or text)") << "\n"
		 << "buffer " << buffers.width << "x" << buffers.height << " stride " << buffers.stride << " origin " << buffers.origin
		 << "\n"
		 << "scroll tile " << params.scroll_tx << "," << params.scroll_ty << " px " << params.scroll_px << ","
		 << params.scroll_py << "\n"
		 << "objects " << buffers.objects.size() << "\n"
		 << "pixels none " << faces[shaded_face_none] << " ground " << faces[shaded_face_ground] << " top "
		 << faces[shaded_face_top] << " south " << faces[shaded_face_south] << " east " << faces[shaded_face_east] << "\n"
		 << "object pixels (unclamped) " << box_px << " rebuilt outside their box " << box_outside << ", clamped "
		 << clamped_px << "\n"
		 << "rounded " << (params.rounded ? "on" : "off") << " seams " << (params.rounded && params.roof_seams ? "on" : "off")
		 << "\n"
		 << "pixels rounded " << rounded_px << "\n"
		 << "seam pixels merged " << seam_pixels << " (last paint)\n"
		 << "clamped " << clamped_px << "\n"
		 << "rounded objects " << kinds[1] + kinds[2] + kinds[3] + kinds[4] << " (figure " << kinds[1] << " round " << kinds[2]
		 << " upright " << kinds[3] << " tree " << kinds[4] << ")\n"
		 << "rounded pixels rebuilt outside their fitted volume " << rounded_outside << "\n"
		 << "rounded pixels near a light facing it " << lit_toward << " mean level "
		 << (lit_toward > 0 ? level_toward / static_cast<double>(lit_toward) : 0.0) << ", facing away " << lit_away
		 << " mean level " << (lit_away > 0 ? level_away / static_cast<double>(lit_away) : 0.0) << "\n";
	if (main_actor_id != shaded_id_none && main_actor_id != shaded_id_overflow) {
		const Shaded_object_record& a = buffers.objects[main_actor_id - 1];
		// The box spans tiles (tx + 1 - xtiles .. tx + 1) and lifts tz .. tz + ztiles.
		const float box_lo[3] = {static_cast<float>(a.tx + 1 - a.xtiles), static_cast<float>(a.ty + 1 - a.ytiles),
								 static_cast<float>(a.tz)};
		const float box_hi[3] = {static_cast<float>(a.tx + 1), static_cast<float>(a.ty + 1), static_cast<float>(a.tz + a.ztiles)};
		bool        inside    = avatar_px > avatar_clamped + avatar_rounded;
		for (int i = 0; i < 3; i++) {
			// Pixel centres can sit half a pixel (1/16 tile) past the box.
			inside = inside && lo[i] >= box_lo[i] - 0.07f && hi[i] <= box_hi[i] + 0.07f;
		}
		info << "avatar id " << main_actor_id << " shape " << a.shape << " frame " << a.frame << " tile " << a.tx << "," << a.ty
			 << "," << a.tz << " box " << a.xtiles << "x" << a.ytiles << "x" << a.ztiles << "\n"
			 << "avatar pixels " << avatar_px << " clamped " << avatar_clamped << " rounded " << avatar_rounded << "\n"
			 << "avatar world x " << lo[0] << ".." << hi[0] << " y " << lo[1] << ".." << hi[1] << " z " << lo[2] << ".." << hi[2]
			 << "\n"
			 << "avatar box x " << box_lo[0] << ".." << box_hi[0] << " y " << box_lo[1] << ".." << box_hi[1] << " z "
			 << box_lo[2] << ".." << box_hi[2] << "\n"
			 << "avatar inside box "
			 << (inside ? "yes" : (avatar_px == avatar_clamped + avatar_rounded && avatar_rounded > 0 ? "rounded (fitted volume check)" : "no"))
			 << "\n";
	} else {
		info << "avatar not recorded\n";
	}
	// The light list, with the pixels its lit object painted in this frame
	// (0 = not painted: hidden, or off screen).
	std::vector<long> id_pixels(buffers.objects.size() + 1, 0);
	for (int y = 0; y < gh; y++) {
		for (int x = 0; x < gw; x++) {
			const uint16_t id = buffers.object_ids[buffers.index(x, y)];
			if (id != shaded_id_overflow && id < id_pixels.size()) {
				id_pixels[id]++;
			}
		}
	}
	// Light levels over the game area, and the bins.
	long level_counts[shaded_light_levels] = {};
	for (int y = 0; y < gh; y++) {
		for (int x = 0; x < gw; x++) {
			level_counts[std::min<int>(buffers.light_levels[buffers.index(x, y)], shaded_light_levels - 1)]++;
		}
	}
	info << "light levels " << params.level_count << " ambient " << params.ambient_level << " dither " << params.dither
		 << " softness " << params.softness << " facing " << params.facing_min << " brightness " << params.brightness
		 << " radius " << params.radius_scale << "\n"
		 << "light level pixels";
	for (const long n : level_counts) {
		info << " " << n;
	}
	info << "\n"
		 << "bins " << params.bins_x << "x" << params.bins_y << " of " << params.bin_size << " px, entries "
		 << buffers.bin_lights.size() << "\n";
	if (!thread_check.empty()) {
		info << thread_check << "\n";
	}
	info << shadow_info << weather_info;
	write_lighting_info(info);
	info << "lights " << lights.size() << " hash " << std::hex << light_hash << std::dec << "\n";
	for (size_t i = 0; i < lights.size(); i++) {
		const Shaded_light_record& rec = lights[i];
		const Tile_coord           t   = light_objects[i]->get_tile();
		const auto                 it  = object_index.find(light_objects[i]);
		const unsigned             id  = it != object_index.end() ? it->second : shaded_id_none;
		const long                 px  = id != shaded_id_none && id < id_pixels.size() ? id_pixels[id] : 0;
		info << "light " << i << " shape " << rec.shape << " frame " << rec.frame << " tile " << t.tx << "," << t.ty << ","
			 << t.tz << " world " << rec.x << "," << rec.y << "," << rec.z << " radius " << rec.radius << " intensity "
			 << rec.intensity << " flags " << rec.flags << " source "
			 << ((rec.flags & shaded_light_carried) ? "carried " : "") << Row_name(light_rows[i]) << " id " << id
			 << " pixels " << px << "\n";
	}
	std::cout << "SHADED: dump " << number << " written to " << prefix << "*.png (" << buffers.objects.size() << " objects)"
			  << std::endl;
#endif
}

/*
 *  Lighting state and the remap self-checks, for the dump info file.
 */

void Shaded_light::write_lighting_info(std::ostream& info) {
	const Palette* pal = Game_window::get_instance()->get_pal();
	info << "lighting " << (lit_active ? "active" : "inactive") << " (option " << (lighting_on ? "on" : "off")
		 << ", relit " << params.lit << ")\n"
		 << "clock palette " << clock_palette << " window palette " << pal->get_palette_index() << "\n"
		 << "ambient Y " << ambient_y << " slider " << params.ambient << " effective " << params.ambient_level << "\n"
		 << "ramp stops";
	if (ramp.count == 0) {
		info << " none";
	}
	for (int k = 0; k < ramp.count; k++) {
		info << " ";
		if (ramp.palette[k] == Shaded_ramp::black) {
			info << "black";
		} else if (ramp.palette[k] == Shaded_ramp::ambient) {
			info << "ambient";
		} else {
			info << "palette" << ramp.palette[k];
		}
		info << "=" << ramp.level[k];
	}
	info << "\n"
		 << "pixels relit " << relit_pixels << "\n";
	if (!load_refs()) {
		info << "check remap not built (no palettes)\n";
		return;
	}
	const uint8_t* refs[4] = {ref_pals[0].data(), ref_pals[1].data(), ref_pals[2].data(), ref_pals[3].data()};
	const uint8_t* day     = refs[0];
	// Luminance of every palettes.flx palette, relative to DAY.
	info << "palette Y";
	for (int n = 0; n <= PALETTE_MANY_LIGHTS; n++) {
		info << " " << n << "=";
		try {
			Palette tmp;
			tmp.load(PALETTES_FLX, PATCH_PALETTES, n);
			uint8_t colours[768];
			for (int i = 0; i < 256; i++) {
				colours[3 * i]     = tmp.get_red(i);
				colours[3 * i + 1] = tmp.get_green(i);
				colours[3 * i + 2] = tmp.get_blue(i);
			}
			info << Shaded_cpu::palette_luminance(colours, day);
		} catch (...) {
			info << "?";
		}
	}
	info << "\n";
	// Built for a DAY ambient, full light must give every index back, unless
	// DAY has the same colour twice.
	const int            top = std::max(params.level_count - 1, 1);
	std::vector<uint8_t> scratch(shaded_remap_size);
	std::vector<uint8_t> shown(768);
	Shaded_light_params  day_params = params;
	day_params.ambient              = 1.0f;
	Shaded_cpu::build_remap(day, refs, day_params, scratch.data(), shown.data(), nullptr);
	int same = 0;
	for (int i = 1; i <= shaded_last_lit_index; i++) {
		same += scratch[top * 256 + i] == i ? 1 : 0;
	}
	info << "check remap day-identity " << same << " of " << shaded_last_lit_index << "\n";
	// Indices whose relit colour gets darker anywhere as the level rises.
	if (remap.size() != shaded_remap_size) {
		info << "check monotonic not built\n";
		return;
	}
	// Also counted beyond 2/63 of white, past the nearest colour's jitter.
	constexpr int jitter  = Shaded_luminance(2, 2, 2);
	int           falling = 0, falling_far = 0;
	auto          lum     = [day](int index) {
		return Shaded_luminance(day[3 * index], day[3 * index + 1], day[3 * index + 2]);
	};
	for (int i = 1; i <= shaded_last_lit_index; i++) {
		bool fell = false, fell_far = false;
		for (int q = 1; q <= top; q++) {
			const int drop = lum(remap[(q - 1) * 256 + i]) - lum(remap[q * 256 + i]);
			fell           = fell || drop > 0;
			fell_far       = fell_far || drop > jitter;
		}
		falling += fell ? 1 : 0;
		falling_far += fell_far ? 1 : 0;
	}
	info << "check monotonic " << falling << " (" << falling_far << " by more than 2/63)\n";
}

void Shaded_light::report_timing() const {
	const double n  = full_paints > 0 ? static_cast<double>(full_paints) : 1.0;
	const double np = paints > 0 ? static_cast<double>(paints) : 1.0;
	std::cout << "SHADED-TIMING: record=" << (record_on ? "on" : "off") << " game=" << game_width << "x" << game_height
			  << " full_paints=" << full_paints << " paint_map_avg_us=" << paint_map_us / n << " record_avg_us=" << record_us / n
			  << " all_paints=" << paints << " avg_pixels=" << paint_pixels / np << " all_paint_map_avg_us=" << all_paint_us / np
			  << " all_record_avg_us=" << all_record_us / np
			  << " light_avg_us=" << full_light_us / static_cast<double>(std::max(full_light_passes, 1LL))
			  << " all_light_avg_us=" << all_light_us / static_cast<double>(std::max(light_passes, 1LL))
			  << " light_passes=" << light_passes << " threads=" << params.threads << " lights=" << params.light_count
			  << " lit=" << (lit_active ? 1 : 0) << " widened_need=" << widened_need << " widened_lights=" << widened_lights
			  << " widened_scroll=" << widened_scroll << " widened_shadow=" << widened_shadow
			  << " shadow_avg_us=" << full_shadow_us / static_cast<double>(std::max(full_shadow_passes, 1LL))
			  << " all_shadow_avg_us=" << all_shadow_us / static_cast<double>(std::max(shadow_passes, 1LL))
			  << " shadow_field_us=" << shadow_field_us / static_cast<double>(std::max(shadow_passes, 1LL))
			  << " shadow_screen_us=" << shadow_screen_us / static_cast<double>(std::max(shadow_passes, 1LL))
			  << " shadow_passes=" << shadow_passes << " shadow_cache_hits=" << shadow_cache_hits
			  << " weather_prep_us=" << weather_prep_us << " flash_us=" << flash_us << std::endl;
}
