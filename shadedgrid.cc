/*
 *  shadedgrid.cc - World occlusion grid for the "shaded light" renderer.
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

#include "shadedgrid.h"

#include "Configuration.h"
#include "actors.h"
#include "chunks.h"
#include "databuf.h"
#include "exceptions.h"
#include "exult.h"
#include "game.h"
#include "gamemap.h"
#include "gamewin.h"
#include "ignore_unused_variable_warning.h"
#include "items.h"
#include "msgfile.h"
#include "objiter.h"
#include "objs.h"
#include "shadedgrid_data.h"
#include "shadedlight.h"
#include "shapeinf.h"
#include "tqueue.h"
#include "utils.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>

/*
 *  Fires the headless dumps and tests (udata 0) and polls door probes in
 *  test mode (udata 1).
 */
class Shaded_grid_driver final : public Time_sensitive {
public:
	void handle_event(unsigned long curtime, uintptr udata) override;
};

namespace {
	constexpr int chunk_tiles = c_tiles_per_chunk;
	constexpr int poll_ms     = 100;

	struct Rgb {
		int r, g, b;
	};

	// -ln(1 - o / 255): optical depth of one cell length of opacity o.
	const std::array<double, 256>& Optical_depth() {
		static const std::array<double, 256> table = [] {
			std::array<double, 256> t{};
			for (int o = 0; o < 255; o++) {
				t[static_cast<size_t>(o)] = -std::log(1.0 - static_cast<double>(o) / 255.0);
			}
			t[255] = 1e30;
			return t;
		}();
		return table;
	}

	inline int Wrap(int v, int m) {
		v %= m;
		return v < 0 ? v + m : v;
	}

	// World tile delta into [-1536, 1536).
	inline double Wrap_delta(double v) {
		v = std::fmod(v, static_cast<double>(c_num_tiles));
		if (v >= c_num_tiles / 2) {
			v -= c_num_tiles;
		} else if (v < -c_num_tiles / 2) {
			v += c_num_tiles;
		}
		return v;
	}

	inline int Floor_div(int a, int b) {
		return a >= 0 ? a / b : -((-a + b - 1) / b);
	}

	/*
	 *  The DDA.  visit(x, y, z, length, end_cell) sees every cell crossed,
	 *  in order (the self-test uses it; the real paths pass a no-op).
	 */
	template <typename Visit>
	Shaded_grid_hit Trace_cells(const Shaded_grid_volume& vol, const Shaded_grid_ray& ray, uint32_t options, Visit&& visit) {
		Shaded_grid_hit hit;
		if (vol.size_x <= 0 || vol.size_y <= 0 || vol.size_z <= 0) {
			hit.flags = shaded_hit_outside;
			return hit;
		}
		// Local cell space; b is taken relative to a, so a ray never splits
		// at the world wrap.
		const double p[3] = {Wrap_delta(ray.a[0] - vol.origin_tx), Wrap_delta(ray.a[1] - vol.origin_ty), ray.a[2]};
		const double d[3] = {Wrap_delta(ray.b[0] - ray.a[0]), Wrap_delta(ray.b[1] - ray.a[1]), ray.b[2] - ray.a[2]};
		const int    size[3] = {vol.size_x, vol.size_y, vol.size_z};
		// Clip the segment to the grid box.
		double t0 = 0.0;
		double t1 = 1.0;
		for (int i = 0; i < 3; i++) {
			if (std::abs(d[i]) < 1e-12) {
				if (p[i] < 0.0 || p[i] >= size[i]) {
					t1 = -1.0;
				}
				continue;
			}
			double ta = -p[i] / d[i];
			double tb = (size[i] - p[i]) / d[i];
			if (ta > tb) {
				std::swap(ta, tb);
			}
			t0 = std::max(t0, ta);
			t1 = std::min(t1, tb);
		}
		if (t1 < t0 || t1 < 0.0) {
			hit.flags = shaded_hit_outside;
			return hit;
		}
		if (t0 > 0.0 || t1 < 1.0) {
			hit.flags |= shaded_hit_outside;
		}
		int ends[2][3];
		for (int i = 0; i < 3; i++) {
			ends[0][i] = static_cast<int>(std::floor(p[i]));
			ends[1][i] = static_cast<int>(std::floor(p[i] + d[i]));
		}
		const bool skip_ends = (options & shaded_ray_include_ends) == 0;
		int        c[3];
		int        step[3];
		double     tmax[3];
		double     tdelta[3];
		for (int i = 0; i < 3; i++) {
			const double s = p[i] + d[i] * t0;
			c[i]           = std::clamp(static_cast<int>(std::floor(s)), 0, size[i] - 1);
			if (d[i] > 1e-12) {
				step[i]   = 1;
				tdelta[i] = 1.0 / d[i];
				tmax[i]   = (c[i] + 1 - p[i]) / d[i];
			} else if (d[i] < -1e-12) {
				step[i]   = -1;
				tdelta[i] = -1.0 / d[i];
				tmax[i]   = (c[i] - p[i]) / d[i];
			} else {
				step[i]   = 0;
				tdelta[i] = 1e30;
				tmax[i]   = 1e30;
			}
		}
		const auto&  depth  = Optical_depth();
		const double length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		double       t      = t0;
		double       sum    = 0.0;
		bool         opaque = false;
		while (true) {
			const double tn  = std::min({tmax[0], tmax[1], tmax[2], t1});
			const double len = (tn - t) * length;
			hit.cells++;
			const bool end_cell = skip_ends
								  && ((c[0] == ends[0][0] && c[1] == ends[0][1] && c[2] == ends[0][2])
									  || (c[0] == ends[1][0] && c[1] == ends[1][1] && c[2] == ends[1][2]));
			visit(c[0], c[1], c[2], len, end_cell);
			if (!end_cell && len > 1e-9) {
				const size_t  idx = vol.index(c[0], c[1], c[2]);
				const uint8_t o   = vol.cells[idx];
				if (o) {
					const uint8_t f = vol.cells[idx + 1];
					if (f & shaded_cell_passes) {
						hit.flags |= shaded_hit_passes;
					}
					if (f & shaded_cell_canopy) {
						hit.flags |= shaded_hit_canopy;
					}
					if (o >= shaded_grid_sky_threshold && hit.hit_x < 0) {
						hit.hit_x = Wrap(vol.origin_tx + c[0], c_num_tiles);
						hit.hit_y = Wrap(vol.origin_ty + c[1], c_num_tiles);
						hit.hit_z = c[2];
					}
					if (o == 255) {
						opaque = true;
						break;
					}
					sum += len * depth[o];
				}
			}
			if (tn >= t1) {
				break;
			}
			const int axis = tmax[0] <= tmax[1] ? (tmax[0] <= tmax[2] ? 0 : 2) : (tmax[1] <= tmax[2] ? 1 : 2);
			c[axis] += step[axis];
			if (c[axis] < 0 || c[axis] >= size[axis]) {
				break;
			}
			t = tn;
			tmax[axis] += tdelta[axis];
		}
		hit.transmittance = opaque ? 0.0f : static_cast<float>(std::exp(-sum));
		if (opaque || hit.transmittance < shaded_grid_min_transmittance) {
			hit.flags |= shaded_hit_blocked;
		}
		return hit;
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

	// Debug colour of a cell.
	Rgb Cell_colour(uint8_t opa, uint8_t flg) {
		const int dim = 80 + 175 * (255 - opa) / 255;    // Brighter = more light through.
		if (!(flg & shaded_cell_object) && opa == 0) {
			return Rgb{0, 0, 0};
		}
		if (flg & shaded_cell_roof) {
			return Rgb{210, 40, 40};
		}
		if (flg & shaded_cell_floor) {
			return Rgb{235, 140, 30};
		}
		if (flg & shaded_cell_passes) {
			return Rgb{0, dim, dim};
		}
		if (flg & shaded_cell_canopy) {
			return Rgb{20, 60 + opa * 3 / 4, 20};
		}
		if (opa == 255) {
			return Rgb{190, 190, 190};
		}
		return Rgb{40, 40, 80 + opa * 3 / 5};
	}

	class Rgb_image {
	public:
		int                        w, h;
		std::vector<unsigned char> data;

		Rgb_image(int width, int height)
				: w(width), h(height), data(static_cast<size_t>(width) * static_cast<size_t>(height) * 3, 0) {}

		void put(int x, int y, const Rgb& c) {
			if (x < 0 || y < 0 || x >= w || y >= h) {
				return;
			}
			unsigned char* px = &data[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3];
			px[0]             = static_cast<unsigned char>(std::clamp(c.r, 0, 255));
			px[1]             = static_cast<unsigned char>(std::clamp(c.g, 0, 255));
			px[2]             = static_cast<unsigned char>(std::clamp(c.b, 0, 255));
		}

		bool is_black(int x, int y) const {
			const unsigned char* px
					= &data[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3];
			return px[0] == 0 && px[1] == 0 && px[2] == 0;
		}

		void fill(int x, int y, int fw, int fh, const Rgb& c) {
			for (int j = y; j < y + fh; j++) {
				for (int i = x; i < x + fw; i++) {
					put(i, j, c);
				}
			}
		}

		void frame(int x, int y, int fw, int fh, const Rgb& c) {
			for (int i = x; i < x + fw; i++) {
				put(i, y, c);
				put(i, y + fh - 1, c);
			}
			for (int j = y; j < y + fh; j++) {
				put(x, j, c);
				put(x + fw - 1, j, c);
			}
		}
	};

#ifdef HAVE_PNG_H
	// Write an 8-bit RGB image as a PNG.
	bool Write_png(const std::string& path, const Rgb_image& img) {
		return Shaded_write_png(path, img.w, img.h, false, img.data);
	}
#endif

	Shaded_grid_ray Make_ray(float ax, float ay, float az, float bx, float by, float bz) {
		return Shaded_grid_ray{
				{ax, ay, az, 0.0f},
				{bx, by, bz, 0.0f}
		};
	}

	void Report(bool ok, const std::string& group, const std::string& name, const std::string& detail, int& failures) {
		std::cout << "TEST " << (ok ? "PASS " : "FAIL ") << group << ' ' << name;
		if (!detail.empty()) {
			std::cout << ' ' << detail;
		}
		std::cout << std::endl;
		if (!ok) {
			failures++;
		}
	}

	std::string Fmt(double v, int prec = 3) {
		std::ostringstream out;
		out << std::fixed << std::setprecision(prec) << v;
		return out.str();
	}
}    // namespace

/*
 *  Volume.
 */

void Shaded_grid_volume::resize(int sx, int sy, int sz) {
	size_x = sx;
	size_y = sy;
	size_z = sz;
	cells.assign(static_cast<size_t>(sx) * static_cast<size_t>(sy) * static_cast<size_t>(sz) * 2, 0);
	sky.assign(static_cast<size_t>(sx) * static_cast<size_t>(sy), 0);
}

bool Shaded_grid_volume::local(int world_tx, int world_ty, int& x, int& y) const {
	x = Wrap(world_tx - origin_tx, c_num_tiles);
	y = Wrap(world_ty - origin_ty, c_num_tiles);
	return x < size_x && y < size_y;
}

void Shaded_grid_volume::stamp_box(int x0, int y0, int z0, int x1, int y1, int z1, uint8_t opa, uint8_t flg) {
	x0 = std::max(x0, 0);
	y0 = std::max(y0, 0);
	z0 = std::max(z0, 0);
	x1 = std::min(x1, size_x - 1);
	y1 = std::min(y1, size_y - 1);
	z1 = std::min(z1, size_z - 1);
	for (int z = z0; z <= z1; z++) {
		for (int y = y0; y <= y1; y++) {
			uint8_t* cell = &cells[index(x0, y, z)];
			for (int x = x0; x <= x1; x++, cell += 2) {
				cell[0] = std::max(cell[0], opa);
				cell[1] |= flg;
			}
		}
	}
}

void Shaded_grid_volume::clear_block(int x0, int y0, int w, int h) {
	for (int z = 0; z < size_z; z++) {
		for (int y = y0; y < y0 + h; y++) {
			std::fill_n(&cells[index(x0, y, z)], static_cast<size_t>(w) * 2, 0);
		}
	}
}

void Shaded_grid_volume::rebuild_sky(int x0, int y0, int w, int h, int threshold) {
	for (int y = y0; y < y0 + h; y++) {
		for (int x = x0; x < x0 + w; x++) {
			uint8_t top = 0;
			for (int z = size_z - 1; z >= 0; z--) {
				const size_t idx = index(x, y, z);
				if (cells[idx] >= threshold && !(cells[idx + 1] & shaded_cell_canopy)) {
					top = static_cast<uint8_t>(z + 1);
					break;
				}
			}
			sky[static_cast<size_t>(y) * static_cast<size_t>(size_x) + static_cast<size_t>(x)] = top;
		}
	}
}

/*
 *  CPU backend.
 */

Shaded_grid_hit Shaded_grid_cpu::trace(const Shaded_grid_volume& vol, const Shaded_grid_ray& ray, uint32_t options) {
	return Trace_cells(vol, ray, options, [](int, int, int, double, bool) {});
}

void Shaded_grid_cpu::trace_batch(
		const Shaded_grid_volume& vol, const Shaded_grid_ray* rays, Shaded_grid_hit* hits, size_t count, uint32_t options) {
	for (size_t i = 0; i < count; i++) {
		hits[i] = trace(vol, rays[i], options);
	}
}

bool Shaded_grid_cpu::open_to_sky(const Shaded_grid_volume& vol, int x, int y, int z) {
	if (x < 0 || y < 0 || x >= vol.size_x || y >= vol.size_y) {
		return true;
	}
	return z >= vol.sky[static_cast<size_t>(y) * static_cast<size_t>(vol.size_x) + static_cast<size_t>(x)];
}

/*
 *  Override lists.
 */

void Shaded_grid_rules::clear() {
	percent_rows.clear();
	shape_flags.clear();
	shapes.clear();
	rows_default = 0;
	rows_patch   = 0;
}

bool Shaded_grid_rules::parse(std::string_view text, bool patch) {
	IBufferDataView            ds(text.data(), text.size());
	const Text_msg_file_reader reader(ds);
	std::vector<std::string>   strings;
	size_t                     rows = 0;
	// Rows as Exult reads shape_info.txt: numbers separated by '/'.
	auto percent_section = [&](std::string_view section, uint8_t flag) {
		reader.get_section_strings(section, strings);
		for (const auto& s : strings) {
			std::istringstream in(s);
			const int          shape = ReadInt(in, -1);
			if (shape < 0 || s.empty()) {
				continue;
			}
			int frame = ReadInt(in, -1);
			frame     = frame < 0 ? 63 : (frame & 31);
			int pct   = std::clamp(ReadInt(in, 100), 0, 100);
			percent_rows[(shape << 6) | frame] = Shaded_grid_rule{pct, flag};
			shapes.insert(shape);
			rows++;
		}
	};
	auto shape_section = [&](std::string_view section, uint8_t flag) {
		reader.get_section_strings(section, strings);
		for (const auto& s : strings) {
			std::istringstream in(s);
			const int          shape = ReadInt(in, -1);
			if (shape < 0 || s.empty()) {
				continue;
			}
			shape_flags[shape] |= flag;
			shapes.insert(shape);
			rows++;
		}
	};
	percent_section("light_passes_through", shaded_cell_passes);
	percent_section("canopy_shapes", shaded_cell_canopy);
	shape_section("roof_shapes", shaded_cell_roof);
	shape_section("floor_shapes", shaded_cell_floor);
	(patch ? rows_patch : rows_default) += rows;
	return rows > 0;
}

void Shaded_grid_rules::load() {
	clear();
	auto read_file = [this](const std::string& file, bool patch) {
		try {
			if (!U7exists(file)) {
				return;
			}
			IFileDataSource ds(file, true);
			if (!ds.good()) {
				return;
			}
			std::string text;
			ds.read(text, ds.getAvail());
			parse(text, patch);
		} catch (const std::exception& err) {
			std::cout << "SHADED-GRID: could not read " << file << ": " << err.what() << std::endl;
		}
	};
	switch (Game::get_game_type()) {
	case BLACK_GATE:
		parse(shaded_grid_bg_text, false);
		break;
	case SERPENT_ISLE:
		parse(shaded_grid_si_text, false);
		break;
	default:
		read_file("<STATIC>/shaded_grid.txt", false);
		break;
	}
	if (is_system_path_defined("<PATCH>")) {
		read_file("<PATCH>/shaded_grid.txt", true);
	}
}

Shaded_grid_rule Shaded_grid_rules::lookup(int shape, int frame) const {
	Shaded_grid_rule rule;
	auto             it = percent_rows.find((shape << 6) | (frame & 31));
	if (it == percent_rows.end()) {
		it = percent_rows.find((shape << 6) | 63);
	}
	if (it != percent_rows.end()) {
		rule = it->second;
	}
	if (const auto fl = shape_flags.find(shape); fl != shape_flags.end()) {
		rule.flags |= fl->second;
	}
	return rule;
}

/*
 *  Synthetic tests.
 */

int Shaded_grid_selftest() {
	int               failures = 0;
	const std::string group    = "grid-synthetic";
	auto              report   = [&](bool ok, const std::string& name, const std::string& detail = "") {
		Report(ok, group, name, detail, failures);
	};
	// A two-storey house: walls x, y 4..12 at lifts 0-9, a floor slab at
	// lift 5, a roof at lift 10; a 90 % window on the east wall, a doorway
	// on the south wall and a 50 % canopy outside.
	Shaded_grid_volume vol;
	vol.origin_tx = 1000;
	vol.origin_ty = 2000;
	vol.resize(24, 24, shaded_grid_lifts);
	for (int i = 4; i <= 12; i++) {
		vol.stamp_box(i, 4, 0, i, 4, 9, 255, shaded_cell_solid | shaded_cell_object);
		vol.stamp_box(i, 12, 0, i, 12, 9, 255, shaded_cell_solid | shaded_cell_object);
		vol.stamp_box(4, i, 0, 4, i, 9, 255, shaded_cell_solid | shaded_cell_object);
		vol.stamp_box(12, i, 0, 12, i, 9, 255, shaded_cell_solid | shaded_cell_object);
	}
	vol.stamp_box(5, 5, 5, 11, 11, 5, 255, shaded_cell_solid | shaded_cell_floor | shaded_cell_object);
	vol.stamp_box(4, 4, 10, 12, 12, 10, 255, shaded_cell_solid | shaded_cell_roof | shaded_cell_object);
	auto set_cell = [&vol](int x, int y, int z, uint8_t opa, uint8_t flg) {
		vol.cells[vol.index(x, y, z)]     = opa;
		vol.cells[vol.index(x, y, z) + 1] = flg;
	};
	set_cell(12, 7, 2, Shaded_grid_opacity_for(90), shaded_cell_passes | shaded_cell_object);
	for (int z = 0; z <= 2; z++) {
		set_cell(8, 12, z, 0, 0);
	}
	vol.stamp_box(17, 4, 0, 17, 6, 3, Shaded_grid_opacity_for(50), shaded_cell_canopy | shaded_cell_object);
	vol.rebuild_sky(0, 0, vol.size_x, vol.size_y, shaded_grid_sky_threshold);
	auto world = [&vol](float x, float y, float z, float x2, float y2, float z2) {
		return Make_ray(vol.origin_tx + x, vol.origin_ty + y, z, vol.origin_tx + x2, vol.origin_ty + y2, z2);
	};
	Shaded_grid_hit h = Shaded_grid_cpu::trace(vol, world(8.5f, 8.5f, 1.5f, 20.5f, 8.5f, 1.5f), 0);
	report((h.flags & shaded_hit_blocked) != 0 && h.transmittance == 0.0f, "wall-blocks",
		   "T=" + Fmt(h.transmittance) + " hit=" + std::to_string(h.hit_x - vol.origin_tx));
	h = Shaded_grid_cpu::trace(vol, world(8.5f, 7.5f, 2.5f, 15.5f, 7.5f, 2.5f), 0);
	report(std::abs(h.transmittance - 0.90f) <= 0.01f && (h.flags & shaded_hit_passes) && !(h.flags & shaded_hit_blocked),
		   "window-90", "T=" + Fmt(h.transmittance));
	h = Shaded_grid_cpu::trace(vol, world(8.5f, 8.5f, 1.5f, 8.5f, 15.5f, 1.5f), 0);
	report(h.transmittance == 1.0f && !(h.flags & shaded_hit_blocked), "doorway-clear", "T=" + Fmt(h.transmittance));
	h = Shaded_grid_cpu::trace(vol, world(8.5f, 8.5f, 2.5f, 8.5f, 8.5f, 7.5f), 0);
	report((h.flags & shaded_hit_blocked) != 0 && h.hit_z == 5, "slab-vertical", "hit_z=" + std::to_string(h.hit_z));
	h = Shaded_grid_cpu::trace(vol, world(6.5f, 6.5f, 2.5f, 10.5f, 10.5f, 7.5f), 0);
	report((h.flags & shaded_hit_blocked) != 0, "slab-diagonal", "hit_z=" + std::to_string(h.hit_z));
	h = Shaded_grid_cpu::trace(vol, world(14.5f, 5.5f, 1.5f, 20.5f, 5.5f, 1.5f), 0);
	report(std::abs(h.transmittance - 0.5f) <= 0.02f && (h.flags & shaded_hit_canopy), "canopy-50",
		   "T=" + Fmt(h.transmittance));
	// Diagonal through the canopy: longer path, less light.
	const Shaded_grid_hit hd = Shaded_grid_cpu::trace(vol, world(14.5f, 3.5f, 1.5f, 20.5f, 6.5f, 1.5f), 0);
	report(hd.transmittance < h.transmittance && hd.transmittance > 0.2f, "canopy-path-length",
		   "T=" + Fmt(hd.transmittance));
	report(!Shaded_grid_cpu::open_to_sky(vol, 8, 8, 0) && !Shaded_grid_cpu::open_to_sky(vol, 8, 8, 6)
				   && Shaded_grid_cpu::open_to_sky(vol, 8, 8, 11) && Shaded_grid_cpu::open_to_sky(vol, 20, 20, 0)
				   && Shaded_grid_cpu::open_to_sky(vol, 17, 5, 0),
		   "open-to-sky", "sky(8,8)=" + std::to_string(vol.sky[8 * 24 + 8]));
	h = Shaded_grid_cpu::trace(vol, world(12.5f, 9.5f, 1.5f, 14.5f, 9.5f, 1.5f), 0);
	const Shaded_grid_hit he
			= Shaded_grid_cpu::trace(vol, world(12.5f, 9.5f, 1.5f, 14.5f, 9.5f, 1.5f), shaded_ray_include_ends);
	report(h.transmittance == 1.0f && (he.flags & shaded_hit_blocked), "end-cells",
		   "skip T=" + Fmt(h.transmittance) + " include T=" + Fmt(he.transmittance));
	h = Shaded_grid_cpu::trace(vol, world(-5.5f, 8.5f, 1.5f, 30.5f, 8.5f, 1.5f), 0);
	report((h.flags & shaded_hit_outside) && (h.flags & shaded_hit_blocked), "outside-flag");
	// Random rays against dense sampling (1/64 cell steps), over random
	// contents.  Coverage uses partial cells only, so no ray stops early.
	for (int pass = 0; pass < 2; pass++) {
		const bool         coverage = pass == 0;
		Shaded_grid_volume rnd;
		rnd.origin_tx = 500;
		rnd.origin_ty = 600;
		rnd.resize(24, 24, shaded_grid_lifts);
		std::mt19937                          gen(20251113);
		std::uniform_int_distribution<int>    pick(0, 99);
		std::uniform_real_distribution<float> coord(-2.0f, 26.0f);
		std::uniform_real_distribution<float> lift(-1.0f, 17.0f);
		for (size_t i = 0; i < rnd.cells.size(); i += 2) {
			const int r = pick(gen);
			if (r < 8) {
				rnd.cells[i] = coverage ? 200 : 255;
			} else if (r < 16) {
				rnd.cells[i] = static_cast<uint8_t>(1 + pick(gen) * 2);
			}
		}
		int                  missed           = 0;
		long long            samples          = 0;
		int                  disagree_sampled = 0;
		int                  dda_only         = 0;
		const int            rays             = 5000;
		std::vector<int32_t> visited(rnd.cells.size() / 2, -1);    // Ray number.
		for (int r = 0; r < rays; r++) {
			const Shaded_grid_ray ray = Make_ray(
					500 + coord(gen), 600 + coord(gen), lift(gen), 500 + coord(gen), 600 + coord(gen), lift(gen));
			const Shaded_grid_hit hit = Trace_cells(rnd, ray, 0, [&visited, &rnd, r](int x, int y, int z, double, bool) {
				visited[rnd.index(x, y, z) / 2] = r;
			});
			const double dx = ray.b[0] - ray.a[0];
			const double dy = ray.b[1] - ray.a[1];
			const double dz = ray.b[2] - ray.a[2];
			const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
			const int    n   = static_cast<int>(std::ceil(len * 64.0)) + 1;
			const std::array<int, 3> ea{static_cast<int>(std::floor(ray.a[0] - 500)), static_cast<int>(std::floor(ray.a[1] - 600)),
										static_cast<int>(std::floor(ray.a[2]))};
			const std::array<int, 3> eb{static_cast<int>(std::floor(ray.b[0] - 500)), static_cast<int>(std::floor(ray.b[1] - 600)),
										static_cast<int>(std::floor(ray.b[2]))};
			bool sampled_block = false;
			for (int k = 0; k <= n; k++) {
				const double             s = static_cast<double>(k) / n;
				const std::array<int, 3> cell{static_cast<int>(std::floor(ray.a[0] - 500 + dx * s)),
											  static_cast<int>(std::floor(ray.a[1] - 600 + dy * s)),
											  static_cast<int>(std::floor(ray.a[2] + dz * s))};
				if (!rnd.inside(cell[0], cell[1], cell[2])) {
					continue;
				}
				samples++;
				if (visited[rnd.index(cell[0], cell[1], cell[2]) / 2] != r) {
					missed++;
				}
				if (cell != ea && cell != eb && rnd.opacity(cell[0], cell[1], cell[2]) == 255) {
					sampled_block = true;
				}
			}
			const bool dda_block = hit.transmittance == 0.0f;
			if (sampled_block && !dda_block) {
				disagree_sampled++;
			} else if (dda_block && !sampled_block) {
				dda_only++;    // Clipped a corner thinner than the sample step.
			}
		}
		if (coverage) {
			report(missed == 0, "dda-covers-samples",
				   std::to_string(rays) + " rays, " + std::to_string(samples) + " samples, " + std::to_string(missed) + " missed");
		} else {
			report(disagree_sampled == 0 && dda_only * 100 <= rays, "dda-blocking-agrees",
				   std::to_string(rays) + " rays, " + std::to_string(disagree_sampled) + " missed blocks, "
						   + std::to_string(dda_only) + " corner-only blocks");
		}
	}
	// World wrap: a grid across tile 3071 -> 0.
	{
		Shaded_grid_volume wrap;
		wrap.origin_tx = c_num_tiles - 8;
		wrap.origin_ty = 100;
		wrap.resize(24, 24, shaded_grid_lifts);
		int lx = -1;
		int ly = -1;
		const bool in = wrap.local(2, 105, lx, ly);
		wrap.stamp_box(lx, ly, 0, lx, ly, 3, 255, shaded_cell_solid | shaded_cell_object);
		h = Shaded_grid_cpu::trace(wrap, Make_ray(c_num_tiles - 1.5f, 105.5f, 1.5f, 4.5f, 105.5f, 1.5f), 0);
		const Shaded_grid_hit h2 = Shaded_grid_cpu::trace(wrap, Make_ray(c_num_tiles - 1.5f, 106.5f, 1.5f, 4.5f, 106.5f, 1.5f), 0);
		report(in && lx == 10 && ly == 5 && (h.flags & shaded_hit_blocked) && h.hit_x == 2 && h2.transmittance == 1.0f,
			   "world-wrap", "local=" + std::to_string(lx) + "," + std::to_string(ly) + " hit_x=" + std::to_string(h.hit_x));
	}
	// Override parsing: a patch row replaces a default row.
	{
		Shaded_grid_rules rules;
		rules.parse(
				"%%section light_passes_through\n:270/4/70\n:322/-1/50\n:347-1/20\n%%endsection\n"
				"%%section roof_shapes\n:156\n%%endsection\n",
				false);
		rules.parse("%%section light_passes_through\n:270/4/0\n%%endsection\n", true);
		const Shaded_grid_rule door  = rules.lookup(270, 4);
		const Shaded_grid_rule door2 = rules.lookup(270, 4 | 32);
		const Shaded_grid_rule door5 = rules.lookup(270, 5);
		const Shaded_grid_rule shut  = rules.lookup(322, 7);
		const Shaded_grid_rule roof  = rules.lookup(156, 3);
		report(door.percent == 0 && door2.percent == 0 && door5.percent == -1 && shut.percent == 50
					   && (roof.flags & shaded_cell_roof) && roof.percent == -1 && rules.rows_default == 4
					   && rules.rows_patch == 1 && rules.has_shape(347),
			   "rules-patch",
			   "270/4=" + std::to_string(door.percent) + " 322/7=" + std::to_string(shut.percent) + " rows="
					   + std::to_string(rules.rows_default) + "+" + std::to_string(rules.rows_patch));
		Shaded_grid_rules bg;
		Shaded_grid_rules si;
		bg.parse(shaded_grid_bg_text, false);
		si.parse(shaded_grid_si_text, false);
		report(bg.lookup(270, 5).percent == 70 && bg.lookup(373, 0).percent == 90 && (bg.lookup(161, 0).flags & shaded_cell_roof)
					   && (si.lookup(193, 0).flags & shaded_cell_floor) && si.lookup(290, 4).percent == 90,
			   "rules-defaults", "bg rows=" + std::to_string(bg.rows_default) + " si rows=" + std::to_string(si.rows_default));
	}
	return failures;
}

/*
 *  The world grid.
 */

Shaded_grid* Shaded_grid::active = nullptr;

Shaded_grid& Shaded_grid::get() {
	static Shaded_grid instance;
	return instance;
}

Shaded_grid::Shaded_grid() {
	bool enabled = false;
	if (config) {
		config->value("config/video/shaded_light_grid", enabled, false);
	}
	// Traced shadows and weather lighting need the grid.
	if (Shaded_light::get().shadows_wanted() || Shaded_light::get().weather_wanted()) {
		enabled = true;
	}
	if (const char* dir = std::getenv("EXULT_SHADED_GRID_DUMP"); dir && *dir) {
		dump_dir = dir;
		std::error_code ec;
		std::filesystem::create_directories(dump_dir, ec);
		if (ec) {
			std::cout << "SHADED-GRID: could not create dump folder " << dump_dir << ": " << ec.message() << std::endl;
			dump_dir.clear();
		}
		enabled = true;
	}
	if (const char* test = std::getenv("EXULT_SHADED_GRID_TEST"); test && *test && *test != '0') {
		test_mode = true;
		enabled   = true;
	}
	const char*       at = std::getenv("EXULT_SHADED_GRID_DUMP_AT");
	std::stringstream times(at && *at ? at : "6000");
	std::string       item;
	while (std::getline(times, item, ',')) {
		dump_at.push_back(std::atoi(item.c_str()));
	}
	std::sort(dump_at.begin(), dump_at.end());
	if (!enabled) {
		return;
	}
	active         = this;
	params.enabled = 1;
	driver         = std::make_unique<Shaded_grid_driver>();
	driver->set_always(true);
	rules.load();
	int cx0 = 0;
	int cy0 = 0;
	int w   = 0;
	int h   = 0;
	desired_extent(cx0, cy0, w, h);
	std::cout << "SHADED-GRID: on (" << w << "x" << h << " chunks, "
			  << (static_cast<size_t>(w) * h * chunk_tiles * chunk_tiles * shaded_grid_lifts * 2 + static_cast<size_t>(w) * h * 256) / 1024
			  << " KB), " << rules.rows_default << " default + " << rules.rows_patch << " patch rule rows"
			  << (dump_dir.empty() ? "" : ", dumps in ") << dump_dir << std::endl;
	if (test_mode) {
		const auto start = Clock::now();
		const int  fails = Shaded_grid_selftest();
		std::cout << "TEST INFO grid-synthetic done, " << fails << " failed, "
				  << Fmt(std::chrono::duration<double, std::milli>(Clock::now() - start).count(), 1) << " ms" << std::endl;
	}
}

void Shaded_grid::desired_extent(int& cx0, int& cy0, int& w, int& h) const {
	// The chunk range Game_render::paint_map paints, plus the margin.
	const Game_window* gwin = Game_window::get_instance();
	auto range = [](int scroll, int pixels, int& start, int& count) {
		start           = Floor_div(scroll - 1, chunk_tiles);
		const int stop  = 2 + (scroll + (pixels + c_tilesize - 2) / c_tilesize + chunk_tiles / 2) / chunk_tiles;
		count           = stop - start + 2 * shaded_grid_margin_chunks;
		start          -= shaded_grid_margin_chunks;
		if (count > shaded_grid_max_chunks) {
			start += (count - shaded_grid_max_chunks) / 2;
			count  = shaded_grid_max_chunks;
		}
		start = Wrap(start, c_num_chunks);
	};
	range(gwin->get_scrolltx(), gwin->get_width(), cx0, w);
	range(gwin->get_scrollty(), gwin->get_height(), cy0, h);
}

void Shaded_grid::recentre(int cx0, int cy0, int w, int h) {
	Shaded_grid_volume next;
	next.origin_tx = cx0 * chunk_tiles;
	next.origin_ty = cy0 * chunk_tiles;
	next.resize(w * chunk_tiles, h * chunk_tiles, shaded_grid_lifts);
	std::vector<Slot> next_slots(static_cast<size_t>(w) * static_cast<size_t>(h));
	for (int sy = 0; sy < h; sy++) {
		for (int sx = 0; sx < w; sx++) {
			Slot& slot = next_slots[static_cast<size_t>(sy * w + sx)];
			slot.cx    = Wrap(cx0 + sx, c_num_chunks);
			slot.cy    = Wrap(cy0 + sy, c_num_chunks);
			const int ox = Wrap(slot.cx - origin_cx, c_num_chunks);
			const int oy = Wrap(slot.cy - origin_cy, c_num_chunks);
			if (ox >= chunks_x || oy >= chunks_y || slots.empty()) {
				continue;
			}
			const Slot& old = slots[static_cast<size_t>(oy * chunks_x + ox)];
			slot.built      = old.built;
			slot.dirty      = old.dirty;
			if (!old.built) {
				continue;
			}
			for (int ty = 0; ty < chunk_tiles; ty++) {
				const int ny = sy * chunk_tiles + ty;
				const int oyt = oy * chunk_tiles + ty;
				for (int z = 0; z < shaded_grid_lifts; z++) {
					std::copy_n(&volume.cells[volume.index(ox * chunk_tiles, oyt, z)], chunk_tiles * 2,
								&next.cells[next.index(sx * chunk_tiles, ny, z)]);
				}
				std::copy_n(&volume.sky[static_cast<size_t>(oyt) * static_cast<size_t>(volume.size_x) + static_cast<size_t>(ox * chunk_tiles)],
							chunk_tiles, &next.sky[static_cast<size_t>(ny) * static_cast<size_t>(next.size_x) + static_cast<size_t>(sx * chunk_tiles)]);
			}
		}
	}
	volume    = std::move(next);
	slots     = std::move(next_slots);
	origin_cx = cx0;
	origin_cy = cy0;
	chunks_x  = w;
	chunks_y  = h;
	any_dirty = true;    // New slots are unbuilt.
	// Everything moved in texture space.
	dirty_x0 = 0;
	dirty_y0 = 0;
	dirty_x1 = volume.size_x - 1;
	dirty_y1 = volume.size_y - 1;
}

void Shaded_grid::stamp_object(const Game_object& obj, int sx, int sy) {
	if (obj.as_actor() || obj.is_egg()) {
		return;
	}
	const Shape_info& info  = obj.get_info();
	const int         shape = obj.get_shapenum();
	const int         frame = obj.get_framenum();
	int               zt    = info.get_3d_height();
	const bool        solid = info.is_solid();
	const bool        ruled = rules.has_shape(shape);
	if (!(solid && zt > 0) && !ruled) {
		return;
	}
	const Shaded_grid_rule rule = ruled ? rules.lookup(shape, frame) : Shaded_grid_rule{};
	const uint8_t          cover = rule.flags & (shaded_cell_roof | shaded_cell_floor);
	if (zt == 0) {
		if (!cover) {
			return;
		}
		zt = 1;    // A flat roof or floor still stops light.
	}
	int     opacity = solid || cover ? 255 : 0;
	uint8_t flags   = shaded_cell_object | cover;
	if (solid) {
		flags |= shaded_cell_solid;
	}
	if (info.is_door()) {
		flags |= shaded_cell_door;
	}
	if (rule.percent >= 0) {
		opacity = Shaded_grid_opacity_for(rule.percent);
		if (rule.percent > 0) {
			flags |= rule.flags & (shaded_cell_passes | shaded_cell_canopy);
		}
	}
	const Tile_coord t  = obj.get_tile();
	const int        xt = info.get_3d_xtiles(frame);
	const int        yt = info.get_3d_ytiles(frame);
	int              lx = 0;
	int              ly = 0;
	volume.local(t.tx, t.ty, lx, ly);
	if (lx >= c_num_tiles / 2) {    // West of / north of the grid.
		lx -= c_num_tiles;
	}
	if (ly >= c_num_tiles / 2) {
		ly -= c_num_tiles;
	}
	const int bx0 = sx * chunk_tiles;
	const int by0 = sy * chunk_tiles;
	const int x0  = std::max(lx - xt + 1, bx0);
	const int x1  = std::min(lx, bx0 + chunk_tiles - 1);
	const int y0  = std::max(ly - yt + 1, by0);
	const int y1  = std::min(ly, by0 + chunk_tiles - 1);
	if (x0 > x1 || y0 > y1) {
		return;
	}
	const int z1 = t.tz + zt - 1;
	if (z1 >= shaded_grid_lifts) {
		clipped_cells += static_cast<long long>(x1 - x0 + 1) * (y1 - y0 + 1)
						 * (z1 - std::max(static_cast<int>(t.tz), shaded_grid_lifts) + 1);
	}
	volume.stamp_box(x0, y0, t.tz, x1, y1, z1, static_cast<uint8_t>(opacity), flags);
}

bool Shaded_grid::build_slot(int sx, int sy) {
	Slot&      slot   = slots[static_cast<size_t>(sy * chunks_x + sx)];
	const bool before = slot.built;
	const int  bx0    = sx * chunk_tiles;
	const int  by0    = sy * chunk_tiles;
	const auto rows   = [&](const auto& visit) {
		for (int z = 0; z < volume.size_z; z++) {
			for (int y = by0; y < by0 + chunk_tiles; y++) {
				visit(&volume.cells[volume.index(bx0, y, z)]);
			}
		}
	};
	// Keep the old cells of a rebuilt slot, to tell a real change from a
	// dirty mark that left the cells as they were.
	const size_t row_bytes = static_cast<size_t>(chunk_tiles) * 2;
	slot_scratch.clear();
	if (before) {
		rows([&](const uint8_t* row) {
			slot_scratch.insert(slot_scratch.end(), row, row + row_bytes);
		});
	}
	slot.built = true;
	slot.dirty = false;
	volume.clear_block(bx0, by0, chunk_tiles, chunk_tiles);
	Game_map* map = Game_window::get_instance()->get_map();
	// Footprints reach west and north out of their chunk, so the chunks to
	// the east, south and south-east can reach into this one.
	for (int dy = 0; dy <= 1; dy++) {
		for (int dx = 0; dx <= 1; dx++) {
			Map_chunk* chunk = map->get_chunk_unsafe(Wrap(slot.cx + dx, c_num_chunks), Wrap(slot.cy + dy, c_num_chunks));
			if (!chunk) {
				continue;
			}
			Nonflat_object_iterator next(chunk);
			while (const Game_object* obj = next.get_next()) {
				stamp_object(*obj, sx, sy);
			}
		}
	}
	volume.rebuild_sky(sx * chunk_tiles, sy * chunk_tiles, chunk_tiles, chunk_tiles, shaded_grid_sky_threshold);
	if (dirty_x1 < dirty_x0) {
		dirty_x0 = sx * chunk_tiles;
		dirty_y0 = sy * chunk_tiles;
		dirty_x1 = dirty_x0 + chunk_tiles - 1;
		dirty_y1 = dirty_y0 + chunk_tiles - 1;
	} else {
		dirty_x0 = std::min(dirty_x0, sx * chunk_tiles);
		dirty_y0 = std::min(dirty_y0, sy * chunk_tiles);
		dirty_x1 = std::max(dirty_x1, sx * chunk_tiles + chunk_tiles - 1);
		dirty_y1 = std::max(dirty_y1, sy * chunk_tiles + chunk_tiles - 1);
	}
	chunks_built++;
	if (!before) {
		return true;
	}
	bool   changed = false;
	size_t offset  = 0;
	rows([&](const uint8_t* row) {
		changed = changed || !std::equal(row, row + row_bytes, slot_scratch.begin() + offset);
		offset += row_bytes;
	});
	chunks_unchanged += changed ? 0 : 1;
	return changed;
}

bool Shaded_grid::sync() {
	if (!active) {
		return false;
	}
	Game_window* gwin = Game_window::get_instance();
	Game_map*    map  = gwin ? gwin->get_map() : nullptr;
	if (!map) {
		return false;
	}
	int cx0 = 0;
	int cy0 = 0;
	int w   = 0;
	int h   = 0;
	desired_extent(cx0, cy0, w, h);
	const auto start   = Clock::now();
	bool       changed = false;
	if (map->get_num() != map_num || w != chunks_x || h != chunks_y) {
		if (w * h > 0 && (w > chunks_x || h > chunks_y) && !warned_cap && (w == shaded_grid_max_chunks || h == shaded_grid_max_chunks)) {
			std::cout << "SHADED-GRID: view needs more than " << shaded_grid_max_chunks << " chunks; clamped around the centre"
					  << std::endl;
			warned_cap = true;
		}
		map_num   = map->get_num();
		slots.clear();
		chunks_x  = 0;
		chunks_y  = 0;
		clipped_cells = 0;
		recentre(cx0, cy0, w, h);
		changed = true;
	} else if (cx0 != origin_cx || cy0 != origin_cy) {
		recentre(cx0, cy0, w, h);
		changed = true;
	}
	const auto moved = Clock::now();
	if (changed) {
		last_recentre_ms = std::chrono::duration<double, std::milli>(moved - start).count();
	}
	int  built        = 0;
	bool cells_change = false;
	if (any_dirty) {
		any_dirty = false;
		for (int sy = 0; sy < chunks_y; sy++) {
			for (int sx = 0; sx < chunks_x; sx++) {
				const Slot& slot = slots[static_cast<size_t>(sy * chunks_x + sx)];
				if (!slot.built || slot.dirty) {
					cells_change = build_slot(sx, sy) || cells_change;
					built++;
				}
			}
		}
	}
	if (built) {
		last_build_ms = std::chrono::duration<double, std::milli>(Clock::now() - moved).count();
		last_built    = built;
	}
	// Only a real change of the cells moves the generation: dirty marks
	// that rebuild a chunk to the same cells (objects moved within it, or
	// actors, which are not stamped) keep the users' caches valid.
	changed = changed || cells_change;
	if (changed) {
		params.generation++;
		params.origin_tx = volume.origin_tx;
		params.origin_ty = volume.origin_ty;
		params.size_x    = volume.size_x;
		params.size_y    = volume.size_y;
		params.size_z    = volume.size_z;
		params.map_num   = map_num;
	}
	return changed;
}

void Shaded_grid::mark_dirty(int world_tx, int world_ty) {
	const int cx = Wrap(world_tx, c_num_tiles) / chunk_tiles;
	const int cy = Wrap(world_ty, c_num_tiles) / chunk_tiles;
	const int sx = Wrap(cx - origin_cx, c_num_chunks);
	const int sy = Wrap(cy - origin_cy, c_num_chunks);
	if (sx < chunks_x && sy < chunks_y) {
		slots[static_cast<size_t>(sy * chunks_x + sx)].dirty = true;
		any_dirty                                            = true;
	}
}

void Shaded_grid::object_changed(const Game_object& obj, bool frame_change) {
	const Map_chunk* chunk = obj.get_chunk();
	if (!chunk || obj.as_actor() || obj.is_egg()) {
		return;
	}
	const Shape_info& info = obj.get_info();
	if (test_mode && frame_change && info.is_door() && !in_cpp_toggle) {
		try {
			door_probes.push_back(obj.shared_from_this());
		} catch (const std::bad_weak_ptr&) {
			// Not owned yet (being created); nothing to probe.
		}
	}
	if (chunks_x == 0 || chunk->get_map()->get_num() != map_num) {
		return;    // Nothing built yet, or another map.
	}
	const int         shape = obj.get_shapenum();
	if (!(info.is_solid() && info.get_3d_height() > 0) && !rules.has_shape(shape)) {
		return;
	}
	// A square covers both the frame and its reflection (swapped x and y).
	const int        frame = obj.get_framenum();
	const int        s     = std::max(info.get_3d_xtiles(frame), info.get_3d_ytiles(frame));
	const Tile_coord t     = obj.get_tile();
	mark_dirty(t.tx, t.ty);
	mark_dirty(t.tx - s + 1, t.ty);
	mark_dirty(t.tx, t.ty - s + 1);
	mark_dirty(t.tx - s + 1, t.ty - s + 1);
}

void Shaded_grid::world_cleared() {
	map_num   = -1;    // Full rebuild on the next sync.
	chunks_x  = 0;
	chunks_y  = 0;
	slots.clear();
	any_dirty = false;
	scheduled = false;    // The time queue was cleared.
	door_probes.clear();
	rules.load();
}

void Shaded_grid::view_changed() {
	if (!scheduled && (!dump_dir.empty() || test_mode)) {
		schedule();
	}
}

Shaded_grid_hit Shaded_grid::visibility(float ax, float ay, float az, float bx, float by, float bz, uint32_t options) {
	sync();
	return Shaded_grid_cpu::trace(volume, Make_ray(ax, ay, az, bx, by, bz), options);
}

void Shaded_grid::trace_batch(const Shaded_grid_ray* rays, Shaded_grid_hit* hits, size_t count, uint32_t options) {
	sync();
	Shaded_grid_cpu::trace_batch(volume, rays, hits, count, options);
}

bool Shaded_grid::open_to_sky(int tx, int ty, int lift) {
	sync();
	int x = 0;
	int y = 0;
	if (!volume.local(tx, ty, x, y)) {
		return true;
	}
	return Shaded_grid_cpu::open_to_sky(volume, x, y, lift);
}

bool Shaded_grid::take_dirty_box(int& x0, int& y0, int& z0, int& x1, int& y1, int& z1) {
	if (dirty_x1 < dirty_x0) {
		return false;
	}
	x0       = dirty_x0;
	y0       = dirty_y0;
	z0       = 0;
	x1       = dirty_x1;
	y1       = dirty_y1;
	z1       = volume.size_z - 1;
	dirty_x0 = 0;
	dirty_y0 = 0;
	dirty_x1 = -1;
	dirty_y1 = -1;
	return true;
}

/*
 *  Headless dumps and tests.
 */

void Shaded_grid_driver::handle_event(unsigned long curtime, uintptr udata) {
	ignore_unused_variable_warning(curtime);
	Shaded_grid& grid = Shaded_grid::get();
	if (udata == 1) {
		grid.poll_door_probes();
		if (grid.scheduled) {
			Game_window::get_instance()->get_tqueue()->add(SDL_GetTicks() + poll_ms, this, static_cast<uintptr>(1));
		}
		return;
	}
	grid.driver_event();
}

void Shaded_grid::schedule() {
	Time_queue* tqueue = Game_window::get_instance()->get_tqueue();
	if (!tqueue || !driver) {
		return;
	}
	const unsigned now = SDL_GetTicks();
	if (activation_ticks == 0) {
		activation_ticks = now;
	}
	scheduled = true;
	if (dump_next < dump_at.size()) {
		const unsigned due = activation_ticks + static_cast<unsigned>(std::max(dump_at[dump_next], 0));
		tqueue->add(std::max(due, now), driver.get(), static_cast<uintptr>(0));
	}
	if (test_mode) {
		tqueue->add(now + poll_ms, driver.get(), static_cast<uintptr>(1));
	}
}

void Shaded_grid::driver_event() {
	sync();
	const int number = dump_number++;
	if (!dump_dir.empty()) {
		dump(number);
	}
	if (test_mode) {
		run_world_tests(number);
	}
	dump_next++;
	if (dump_next < dump_at.size()) {
		const unsigned now = SDL_GetTicks();
		const unsigned due = activation_ticks + static_cast<unsigned>(std::max(dump_at[dump_next], 0));
		Game_window::get_instance()->get_tqueue()->add(std::max(due, now), driver.get(), static_cast<uintptr>(0));
	}
}

void Shaded_grid::dump(int number) {
	const std::string prefix = dump_dir + "/grid-" + std::to_string(number) + "-";
	Game_window*      gwin   = Game_window::get_instance();
	const TileRect    view   = gwin->get_win_tile_rect();
	int               vx     = 0;
	int               vy     = 0;
	volume.local(view.x, view.y, vx, vy);
	int        ax     = -1;
	int        ay     = -1;
	int        alift  = 0;
	const bool avatar = gwin->get_main_actor()
						&& volume.local(gwin->get_main_actor()->get_tile().tx, gwin->get_main_actor()->get_tile().ty, ax, ay);
	if (avatar) {
		alift = gwin->get_main_actor()->get_lift();
	}
	// Counts.
	long long count_set = 0;
	std::map<uint8_t, long long> per_flag;
	std::vector<bool>            lift_used(static_cast<size_t>(volume.size_z), false);
	for (int z = 0; z < volume.size_z; z++) {
		for (int y = 0; y < volume.size_y; y++) {
			for (int x = 0; x < volume.size_x; x++) {
				const uint8_t f = volume.flags(x, y, z);
				if (!f && !volume.opacity(x, y, z)) {
					continue;
				}
				count_set++;
				lift_used[static_cast<size_t>(z)] = true;
				for (int b = 0; b < 8; b++) {
					if (f & (1 << b)) {
						per_flag[static_cast<uint8_t>(1 << b)]++;
					}
				}
			}
		}
	}
	int written = 0;
#ifdef HAVE_PNG_H
	constexpr int scale = 4;
	auto paint_lift = [&](Rgb_image& img, int ox, int oy, int z, int px) {
		for (int y = 0; y < volume.size_y; y++) {
			for (int x = 0; x < volume.size_x; x++) {
				const uint8_t f = volume.flags(x, y, z);
				img.fill(ox + x * px, oy + y * px, px, px, Cell_colour(volume.opacity(x, y, z), f));
				if ((f & shaded_cell_door) && px >= 4) {
					img.fill(ox + x * px + px / 2 - 1, oy + y * px + px / 2 - 1, 2, 2, Rgb{255, 230, 0});
				} else if (f & shaded_cell_door) {
					img.put(ox + x * px, oy + y * px, Rgb{255, 230, 0});
				}
			}
		}
		// Chunk lines over empty cells.
		const int w = volume.size_x * px;
		const int h = volume.size_y * px;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				if ((x % (chunk_tiles * px) == 0 || y % (chunk_tiles * px) == 0) && img.is_black(ox + x, oy + y)) {
					img.put(ox + x, oy + y, Rgb{60, 60, 60});
				}
			}
		}
		img.frame(ox + vx * px, oy + vy * px, view.w * px, view.h * px, Rgb{255, 255, 0});
		if (avatar) {
			const Rgb mag = Rgb{255, 0, 255};
			if (z == alift || z == alift + 1 || z == alift + 2 || z == alift + 3) {
				img.fill(ox + ax * px, oy + ay * px, px, px, mag);
			} else {
				img.frame(ox + ax * px - 1, oy + ay * px - 1, px + 2, px + 2, mag);
			}
		}
	};
	for (int z = 0; z < volume.size_z; z++) {
		if (!lift_used[static_cast<size_t>(z)]) {
			continue;
		}
		Rgb_image img(volume.size_x * scale, volume.size_y * scale);
		paint_lift(img, 0, 0, z, scale);
		char name[32];
		snprintf(name, sizeof(name), "lift-%02d.png", z);
		written += Write_png(prefix + name, img) ? 1 : 0;
	}
	{
		constexpr int px  = 2;
		constexpr int gap = 2;
		const int     tw  = volume.size_x * px;
		const int     th  = volume.size_y * px;
		Rgb_image     img(4 * tw + 3 * gap, 4 * th + 3 * gap);
		img.fill(0, 0, img.w, img.h, Rgb{255, 255, 255});
		for (int z = 0; z < volume.size_z; z++) {
			paint_lift(img, (z % 4) * (tw + gap), (z / 4) * (th + gap), z, px);
		}
		written += Write_png(prefix + "montage.png", img) ? 1 : 0;
	}
	{
		Rgb_image img(volume.size_x * scale, volume.size_y * scale);
		for (int y = 0; y < volume.size_y; y++) {
			for (int x = 0; x < volume.size_x; x++) {
				const int s = volume.sky[static_cast<size_t>(y) * static_cast<size_t>(volume.size_x) + static_cast<size_t>(x)];
				if (s) {
					img.fill(x * scale, y * scale, scale, scale, Height_colour(static_cast<float>(s) / shaded_grid_lifts));
				}
			}
		}
		img.frame(vx * scale, vy * scale, view.w * scale, view.h * scale, Rgb{255, 255, 0});
		if (avatar) {
			img.fill(ax * scale, ay * scale, scale, scale, Rgb{255, 0, 255});
		}
		written += Write_png(prefix + "sky.png", img) ? 1 : 0;
	}
#else
	std::cout << "SHADED-GRID: PNG support not built; images skipped" << std::endl;
#endif
	// Info file: extent, counts and the shapes that matter for the lists.
	std::ofstream info(prefix + "info.txt");
	info << "origin tile " << volume.origin_tx << "," << volume.origin_ty << " chunk " << origin_cx << "," << origin_cy << "\n";
	info << "chunks " << chunks_x << "x" << chunks_y << " cells " << volume.size_x << "x" << volume.size_y << "x" << volume.size_z
		 << " bytes " << volume.cells.size() << " + sky " << volume.sky.size() << "\n";
	info << "map " << map_num << " generation " << params.generation << "\n";
	info << "view tiles " << view.x << "," << view.y << " " << view.w << "x" << view.h << "\n";
	if (avatar) {
		const Tile_coord at = gwin->get_main_actor()->get_tile();
		info << "avatar " << at.tx << "," << at.ty << "," << at.tz << " (local " << ax << "," << ay << ")\n";
	}
	info << "last build " << Fmt(last_build_ms) << " ms for " << last_built << " chunks, last recentre "
		 << Fmt(last_recentre_ms) << " ms, chunks built in total " << chunks_built << " (" << chunks_unchanged
		 << " to the same cells)\n";
	info << "cells set " << count_set << ", clipped above lift " << (shaded_grid_lifts - 1) << ": " << clipped_cells << "\n";
	static const std::pair<uint8_t, const char*> flag_names[]
			= {{shaded_cell_solid, "solid"}, {shaded_cell_passes, "passes"}, {shaded_cell_roof, "roof"},
			   {shaded_cell_floor, "floor"}, {shaded_cell_door, "door"},     {shaded_cell_canopy, "canopy"},
			   {shaded_cell_object, "object"}};
	for (const auto& [flag, fname] : flag_names) {
		info << "flag " << fname << " " << per_flag[flag] << "\n";
	}
	info << "rule rows " << rules.rows_default << " default, " << rules.rows_patch << " patch\n";
	struct Shape_count {
		std::set<int> frames;
		long long     count = 0;
		int           height = 0;
		int           max_lift = 0;
		bool          solid = false;
		bool          building = false;
	};
	std::map<int, Shape_count> shapes;
	Game_map*                  map = gwin->get_map();
	for (const Slot& slot : slots) {
		Map_chunk* chunk = map->get_chunk_unsafe(slot.cx, slot.cy);
		if (!chunk) {
			continue;
		}
		Nonflat_object_iterator next(chunk);
		while (const Game_object* obj = next.get_next()) {
			if (obj->as_actor() || obj->is_egg()) {
				continue;
			}
			const Shape_info& sinfo    = obj->get_info();
			const int         shape    = obj->get_shapenum();
			const bool        building = sinfo.get_shape_class() == Shape_info::building && obj->get_lift() >= 5;
			if (!(sinfo.is_solid() && sinfo.get_3d_height() >= 4) && !rules.has_shape(shape) && !building) {
				continue;
			}
			Shape_count& sc = shapes[shape];
			sc.frames.insert(obj->get_framenum() & 31);
			sc.count++;
			sc.height   = sinfo.get_3d_height();
			sc.max_lift = std::max(sc.max_lift, obj->get_lift());
			sc.solid    = sinfo.is_solid();
			sc.building = sc.building || building;
		}
	}
	info << "shapes (shape frames count height max-lift solid building rule name):\n";
	for (const auto& [shape, sc] : shapes) {
		info << shape << " ";
		bool first = true;
		for (const int f : sc.frames) {
			info << (first ? "" : ",") << f;
			first = false;
		}
		const char* name = get_item_name(static_cast<unsigned>(shape));
		info << " " << sc.count << " h" << sc.height << " l" << sc.max_lift << (sc.solid ? " solid" : " -")
			 << (sc.building ? " building" : " -") << (rules.has_shape(shape) ? " rule" : " NO-RULE") << " "
			 << (name ? name : "?") << "\n";
	}
	std::cout << "SHADED-GRID: dump " << number << " written to " << prefix << "* (" << written << " images, " << count_set
			  << " cells set, " << chunks_x << "x" << chunks_y << " chunks)" << std::endl;
}

bool Shaded_grid::door_probe(const Game_object& door, float& transmittance, bool& blocked, int& cells) {
	const Shape_info& info  = door.get_info();
	const int         frame = door.get_framenum();
	const int         xt    = info.get_3d_xtiles(frame);
	const int         yt    = info.get_3d_ytiles(frame);
	const int         zt    = std::max(info.get_3d_height(), 1);
	const Tile_coord  t     = door.get_tile();
	const float       z     = static_cast<float>(t.tz) + (zt > 1 ? 1.5f : 0.5f);
	Shaded_grid_hit   hit;
	// Across the door's thin side, from the tile before it to the tile after.
	if (xt < yt) {
		const float y = static_cast<float>(t.ty - yt / 2) + 0.5f;
		hit = visibility(static_cast<float>(t.tx - xt) + 0.5f, y, z, static_cast<float>(t.tx + 1) + 0.5f, y, z);
		cells = xt;
	} else {
		const float x = static_cast<float>(t.tx - xt / 2) + 0.5f;
		hit = visibility(x, static_cast<float>(t.ty - yt) + 0.5f, z, x, static_cast<float>(t.ty + 1) + 0.5f, z);
		cells = yt;
	}
	transmittance = hit.transmittance;
	blocked       = (hit.flags & shaded_hit_blocked) != 0;
	return (hit.flags & shaded_hit_outside) == 0;
}

void Shaded_grid::poll_door_probes() {
	if (door_probes.empty()) {
		return;
	}
	int  failures = 0;
	auto probes   = std::move(door_probes);
	door_probes.clear();
	for (const auto& weak : probes) {
		const auto door = weak.lock();
		if (!door || !door->get_chunk()) {
			continue;
		}
		float      t       = 0;
		bool       blocked = false;
		int        cells   = 0;
		const bool inside  = door_probe(*door, t, blocked, cells);
		const Shaded_grid_rule rule = rules.lookup(door->get_shapenum(), door->get_framenum());
		float      expect  = door->get_info().is_solid() ? 0.0f : 1.0f;
		if (rule.percent >= 0) {
			expect = std::pow(static_cast<float>(rule.percent) / 100.0f, static_cast<float>(cells));
		}
		const bool ok = expect == 0.0f ? blocked : std::abs(t - expect) <= 0.05f;
		const Tile_coord at = door->get_tile();
		if (!inside) {
			std::cout << "TEST INFO grid-door-usecode door at " << at.tx << "," << at.ty << "," << at.tz
					  << " is outside the grid" << std::endl;
			continue;
		}
		Report(ok, "grid-door-usecode", "frame " + std::to_string(door->get_framenum()),
			   "shape " + std::to_string(door->get_shapenum()) + " at " + std::to_string(at.tx) + "," + std::to_string(at.ty)
					   + "," + std::to_string(at.tz) + " T=" + Fmt(t) + " expected " + Fmt(expect),
			   failures);
	}
}

void Shaded_grid::run_world_tests(int number) {
	int               failures = 0;
	const std::string group    = "grid-world";
	const std::string tag      = "#" + std::to_string(number) + " ";
	auto              report   = [&](bool ok, const std::string& name, const std::string& detail) {
		Report(ok, group, tag + name, detail, failures);
	};
	auto info_line = [&](const std::string& name, const std::string& detail) {
		std::cout << "TEST INFO " << group << " " << tag << name << " " << detail << std::endl;
	};
	auto centre = [this](int lx, int ly, int z, float& wx, float& wy, float& wz) {
		wx = static_cast<float>(volume.origin_tx + lx) + 0.5f;
		wy = static_cast<float>(volume.origin_ty + ly) + 0.5f;
		wz = static_cast<float>(z) + 0.5f;
	};
	auto clear = [this](int x, int y, int z) {
		return volume.inside(x, y, z) && volume.opacity(x, y, z) == 0;
	};
	// Walls: a solid cell with clear cells on both sides blocks the ray
	// between them.
	{
		int tested = 0;
		int blocked = 0;
		for (int z = 0; z < volume.size_z && tested < 200; z++) {
			for (int y = 1; y + 1 < volume.size_y && tested < 200; y++) {
				for (int x = 1; x + 1 < volume.size_x && tested < 200; x++) {
					const uint8_t f = volume.flags(x, y, z);
					if (!(f & shaded_cell_solid) || (f & (shaded_cell_passes | shaded_cell_door | shaded_cell_canopy))
						|| volume.opacity(x, y, z) != 255) {
						continue;
					}
					int nx = 0;
					int ny = 0;
					if (clear(x - 1, y, z) && clear(x + 1, y, z)) {
						nx = 1;
					} else if (clear(x, y - 1, z) && clear(x, y + 1, z)) {
						ny = 1;
					} else {
						continue;
					}
					float a[3];
					float b[3];
					centre(x - nx, y - ny, z, a[0], a[1], a[2]);
					centre(x + nx, y + ny, z, b[0], b[1], b[2]);
					const Shaded_grid_hit hit = visibility(a[0], a[1], a[2], b[0], b[1], b[2]);
					tested++;
					blocked += (hit.flags & shaded_hit_blocked) ? 1 : 0;
				}
			}
		}
		if (tested) {
			report(blocked == tested, "wall", std::to_string(blocked) + "/" + std::to_string(tested) + " blocked");
		} else {
			info_line("wall", "no free-standing wall cells");
		}
	}
	// Floors: an upper-storey slab with clear lifts above and below blocks
	// vertical and 45 degree rays.
	{
		int tested = 0;
		int blocked = 0;
		for (int z = 1; z + 1 < volume.size_z && tested < 100; z++) {
			for (int y = 1; y + 1 < volume.size_y && tested < 100; y++) {
				for (int x = 0; x < volume.size_x && tested < 100; x++) {
					if (!(volume.flags(x, y, z) & shaded_cell_floor) || volume.opacity(x, y, z) != 255) {
						continue;
					}
					int below = -1;
					int above = -1;
					for (int d = 1; d <= 2; d++) {
						if (below < 0 && clear(x, y, z - d)) {
							below = z - d;
						}
						if (above < 0 && clear(x, y, z + d)) {
							above = z + d;
						}
					}
					if (below < 0 || above < 0) {
						continue;
					}
					float a[3];
					float b[3];
					centre(x, y, below, a[0], a[1], a[2]);
					centre(x, y, above, b[0], b[1], b[2]);
					const Shaded_grid_hit v  = visibility(a[0], a[1], a[2], b[0], b[1], b[2]);
					const float           mz = static_cast<float>(z) + 0.5f;
					const Shaded_grid_hit d  = visibility(a[0], a[1] - 1.0f, mz - 1.0f, b[0], b[1] + 1.0f, mz + 1.0f);
					tested++;
					blocked += ((v.flags & d.flags) & shaded_hit_blocked) ? 1 : 0;
				}
			}
		}
		if (tested) {
			report(blocked == tested, "slab", std::to_string(blocked) + "/" + std::to_string(tested) + " blocked");
		} else {
			info_line("slab", "no floor cells with clear lifts both sides");
		}
	}
	// Sky: roofed columns are covered at lift 0; empty ones are open.
	{
		int roofed = 0;
		int roofed_ok = 0;
		int open = 0;
		int open_ok = 0;
		for (int y = 0; y < volume.size_y; y += 2) {
			for (int x = 0; x < volume.size_x; x += 2) {
				bool roof = false;
				for (int z = 0; z < volume.size_z && !roof; z++) {
					roof = (volume.flags(x, y, z) & shaded_cell_roof) != 0;
				}
				const int  wx  = volume.origin_tx + x;
				const int  wy  = volume.origin_ty + y;
				const int  sky = volume.sky[static_cast<size_t>(y) * static_cast<size_t>(volume.size_x) + static_cast<size_t>(x)];
				if (roof) {
					roofed++;
					roofed_ok += open_to_sky(wx, wy, 0) ? 0 : 1;
				} else if (sky == 0) {
					open++;
					open_ok += open_to_sky(wx, wy, 0) ? 1 : 0;
				}
			}
		}
		const std::string detail = std::to_string(roofed_ok) + "/" + std::to_string(roofed) + " roofed covered, "
								   + std::to_string(open_ok) + "/" + std::to_string(open) + " open";
		if (roofed) {
			report(roofed_ok == roofed && open_ok == open, "sky", detail);
		} else {
			info_line("sky", "no roofs in the grid; " + detail);
		}
	}
	// Rooms (information): rays from under a roof to the open sky.
	{
		std::vector<std::array<int, 2>> inside;
		std::vector<std::array<int, 2>> outside;
		for (int y = 0; y < volume.size_y; y++) {
			for (int x = 0; x < volume.size_x; x++) {
				const int sky = volume.sky[static_cast<size_t>(y) * static_cast<size_t>(volume.size_x) + static_cast<size_t>(x)];
				if (!clear(x, y, 1)) {
					continue;
				}
				bool roof = false;
				for (int z = 2; z < volume.size_z && !roof; z++) {
					roof = (volume.flags(x, y, z) & shaded_cell_roof) != 0;
				}
				if (roof && sky >= 3) {
					inside.push_back({x, y});
				} else if (sky == 0) {
					outside.push_back({x, y});
				}
			}
		}
		std::mt19937 gen(4242);
		int          rays = 0, blocked = 0, passes = 0, clear_gap = 0;
		for (int i = 0; i < 4000 && !inside.empty() && !outside.empty() && rays < 2000; i++) {
			const auto& a = inside[gen() % inside.size()];
			const auto& b = outside[gen() % outside.size()];
			if (std::abs(a[0] - b[0]) > 12 || std::abs(a[1] - b[1]) > 12) {
				continue;
			}
			float p[3];
			float q[3];
			centre(a[0], a[1], 1, p[0], p[1], p[2]);
			centre(b[0], b[1], 1, q[0], q[1], q[2]);
			const Shaded_grid_hit hit = visibility(p[0], p[1], p[2], q[0], q[1], q[2]);
			rays++;
			if (hit.flags & shaded_hit_blocked) {
				blocked++;
			} else if (hit.flags & shaded_hit_passes) {
				passes++;
			} else {
				clear_gap++;
			}
		}
		const double n = std::max(rays, 1);
		info_line("rooms", std::to_string(inside.size()) + " roofed cells, " + std::to_string(outside.size()) + " open cells, "
								   + std::to_string(rays) + " rays: " + Fmt(100.0 * blocked / n, 1) + "% blocked, "
								   + Fmt(100.0 * passes / n, 1) + "% through openings, " + Fmt(100.0 * clear_gap / n, 1)
								   + "% through clear gaps");
	}
	// A door, toggled through the real Game_object::change_frame path.
	{
		Game_window*     gwin = Game_window::get_instance();
		Game_map*        map  = gwin->get_map();
		const Tile_coord at   = gwin->get_main_actor() ? gwin->get_main_actor()->get_tile() : Tile_coord(0, 0, 0);
		Game_object*     door = nullptr;
		int              best = 1 << 30;
		for (const Slot& slot : slots) {
			Map_chunk* chunk = map->get_chunk_unsafe(slot.cx, slot.cy);
			if (!chunk) {
				continue;
			}
			Nonflat_object_iterator next(chunk);
			while (Game_object* obj = next.get_next()) {
				const Shape_info& sinfo = obj->get_info();
				const int         shape = obj->get_shapenum();
				const int         frame = obj->get_framenum();
				if (!sinfo.is_door() || obj->as_actor()) {
					continue;
				}
				const bool closed_frame = (frame & 31) <= 3 && sinfo.is_solid();
				const bool known        = shape == 270 || shape == 376 || rules.lookup(shape, frame + 4).percent >= 50;
				if (!closed_frame || !known) {
					continue;
				}
				const Tile_coord t = obj->get_tile();
				int              lx = 0;
				int              ly = 0;
				if (!volume.local(t.tx, t.ty, lx, ly) || lx < 2 || ly < 2 || lx + 2 >= volume.size_x
					|| ly + 2 >= volume.size_y) {
					continue;
				}
				const int dist = t.distance(at);
				if (dist < best) {
					best = dist;
					door = obj;
				}
			}
		}
		if (!door) {
			info_line("door", "no closed door in the grid");
		} else {
			const int        frame = door->get_framenum();
			const Tile_coord t     = door->get_tile();
			const std::string where = "shape " + std::to_string(door->get_shapenum()) + " frame " + std::to_string(frame) + " at "
									  + std::to_string(t.tx) + "," + std::to_string(t.ty) + "," + std::to_string(t.tz);
			float      t_closed = 0;
			float      t_open   = 0;
			float      t_again  = 0;
			bool       b_closed = false;
			bool       b_open   = false;
			bool       b_again  = false;
			int        cells    = 0;
			in_cpp_toggle       = true;
			door_probe(*door, t_closed, b_closed, cells);
			const auto start = Clock::now();
			door->change_frame(frame + 4);
			door_probe(*door, t_open, b_open, cells);
			const double open_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
			door->change_frame(frame);
			door_probe(*door, t_again, b_again, cells);
			in_cpp_toggle = false;
			report(b_closed && !b_open && t_open >= 0.5f && b_again, "door-toggle",
				   where + ": closed T=" + Fmt(t_closed) + ", open T=" + Fmt(t_open) + ", closed again T=" + Fmt(t_again)
						   + " (update+query " + Fmt(open_ms) + " ms)");
		}
	}
	// Timings.
	{
		for (Slot& slot : slots) {
			slot.built = false;
		}
		any_dirty        = true;
		const auto start = Clock::now();
		sync();
		const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		std::cout << "SHADED-GRID-TIMING: full build " << Fmt(build_ms) << " ms for " << chunks_x * chunks_y << " chunks ("
				  << Fmt(build_ms / std::max(chunks_x * chunks_y, 1), 4) << " ms per chunk)" << std::endl;
		info_line("timing-build", Fmt(build_ms) + " ms, " + std::to_string(chunks_x * chunks_y) + " chunks");
		constexpr int                         count = 100000;
		std::mt19937                          gen(777);
		std::uniform_real_distribution<float> ux(0.0f, static_cast<float>(volume.size_x));
		std::uniform_real_distribution<float> uy(0.0f, static_cast<float>(volume.size_y));
		std::uniform_real_distribution<float> uz(0.0f, 8.0f);
		std::uniform_real_distribution<float> dir(-1.0f, 1.0f);
		std::uniform_real_distribution<float> len(0.0f, 16.0f);
		std::vector<Shaded_grid_ray>          rays(count);
		for (auto& ray : rays) {
			const float x = ux(gen);
			const float y = uy(gen);
			const float z = uz(gen);
			float       d[3];
			float       n = 0;
			do {
				d[0] = dir(gen);
				d[1] = dir(gen);
				d[2] = dir(gen) * 0.5f;
				n    = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			} while (n < 0.1f);
			const float l = len(gen) / n;
			ray           = Make_ray(
					volume.origin_tx + x, volume.origin_ty + y, z, volume.origin_tx + x + d[0] * l, volume.origin_ty + y + d[1] * l,
					std::max(z + d[2] * l, 0.0f));
		}
		std::vector<Shaded_grid_hit> hits(count);
		auto                         t0      = Clock::now();
		int                          blocked = 0;
		for (int i = 0; i < count; i++) {
			const Shaded_grid_ray& r = rays[static_cast<size_t>(i)];
			const Shaded_grid_hit  h = visibility(r.a[0], r.a[1], r.a[2], r.b[0], r.b[1], r.b[2]);
			blocked += (h.flags & shaded_hit_blocked) ? 1 : 0;
		}
		const double single_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		t0                     = Clock::now();
		trace_batch(rays.data(), hits.data(), rays.size());
		const double batch_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		long long    visited  = 0;
		int          blocked2 = 0;
		for (const auto& h : hits) {
			visited += h.cells;
			blocked2 += (h.flags & shaded_hit_blocked) ? 1 : 0;
		}
		std::cout << "SHADED-GRID-TIMING: " << count << " random rays (<= 16 tiles): visibility() " << Fmt(single_ms) << " ms ("
				  << Fmt(single_ms * 1e6 / count, 1) << " ns/ray), trace_batch() " << Fmt(batch_ms) << " ms ("
				  << Fmt(batch_ms * 1e6 / count, 1) << " ns/ray), " << Fmt(100.0 * blocked / count, 1) << "% blocked, "
				  << Fmt(static_cast<double>(visited) / count, 1) << " cells/ray" << std::endl;
		report(blocked == blocked2, "batch-matches-single",
			   std::to_string(blocked) + " vs " + std::to_string(blocked2) + " blocked of " + std::to_string(count));
	}
	std::cout << "TEST INFO grid-world " << tag << "done, " << failures << " failed" << std::endl;
}

void Shaded_grid_world_cleared() {
	if (Shaded_grid::active) {
		Shaded_grid::active->world_cleared();
	}
}

void Shaded_grid_view_changed() {
	// The first call creates the grid (reads the config and environment).
	Shaded_grid& grid = Shaded_grid::get();
	if (Shaded_grid::active) {
		grid.view_changed();
	}
}
