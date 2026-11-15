/*
 *  shadedweather.cc - Weather in the "shaded light" renderer.
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

#include "shadedweather.h"

#include "endianio.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace {
	// Visit the pixels of an RLE frame painted at its origin, as
	// Image_buffer8::paint_rle reads it: visit(x, y) per pixel, x and y
	// relative to the origin.
	template <typename Visit>
	void For_rle_pixels(const unsigned char* rle, Visit&& visit) {
		const uint8* in = rle;
		int          scanlen;
		while ((scanlen = little_endian::Read2(in)) != 0) {
			const int encoded = scanlen & 1;
			scanlen           = scanlen >> 1;
			const int scanx   = static_cast<sint16>(little_endian::Read2(in));
			const int scany   = static_cast<sint16>(little_endian::Read2(in));
			if (!encoded) {
				for (int i = 0; i < scanlen; i++) {
					visit(scanx + i, scany);
				}
				in += scanlen;
				continue;
			}
			for (int b = 0; b < scanlen;) {
				unsigned char bcnt   = Read1(in);
				const int     repeat = bcnt & 1;
				bcnt                 = bcnt >> 1;
				for (int i = 0; i < bcnt; i++) {
					visit(scanx + b + i, scany);
				}
				in += repeat ? 1 : bcnt;
				b += bcnt;
			}
		}
	}

	// One box blur pass of radius r over a w x h byte image, along x
	// (step 1) or y (step w), rounding to nearest.
	void Box_blur(std::vector<uint8_t>& img, int w, int h, int r, bool along_x) {
		const int            n     = along_x ? w : h;
		const int            lines = along_x ? h : w;
		const int            step  = along_x ? 1 : w;
		const int            width = 2 * r + 1;
		std::vector<uint8_t> line(static_cast<size_t>(n));
		for (int l = 0; l < lines; l++) {
			const size_t base = along_x ? static_cast<size_t>(l) * static_cast<size_t>(w) : static_cast<size_t>(l);
			for (int i = 0; i < n; i++) {
				line[static_cast<size_t>(i)] = img[base + static_cast<size_t>(i) * static_cast<size_t>(step)];
			}
			int sum = 0;
			for (int i = -r; i <= r; i++) {
				sum += i >= 0 && i < n ? line[static_cast<size_t>(i)] : 0;
			}
			for (int i = 0; i < n; i++) {
				img[base + static_cast<size_t>(i) * static_cast<size_t>(step)] = static_cast<uint8_t>((sum + width / 2) / width);
				const int out = i - r;
				const int in  = i + r + 1;
				sum -= out >= 0 ? line[static_cast<size_t>(out)] : 0;
				sum += in < n ? line[static_cast<size_t>(in)] : 0;
			}
		}
	}

	// The relit DAY index nearest lab for the fog tables: CIELAB with
	// lightness and hue weighted 4 (chroma 1).  Fog greys DAY's colours, and
	// DAY has few greyed blues: with hue weighted 1 a fogged night sea
	// matched dark magentas of the same lightness.
	int Nearest_fog_colour(const float (*pal_lab)[3], const float lab[3]) {
		int   best      = 1;
		float best_dist = std::numeric_limits<float>::max();
		const float c1  = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
		for (int i = 1; i <= shaded_last_lit_index; i++) {
			const float dl   = lab[0] - pal_lab[i][0];
			const float da   = lab[1] - pal_lab[i][1];
			const float db   = lab[2] - pal_lab[i][2];
			const float dc   = c1 - std::sqrt(pal_lab[i][1] * pal_lab[i][1] + pal_lab[i][2] * pal_lab[i][2]);
			const float dh2  = std::max(da * da + db * db - dc * dc, 0.0f);
			const float dist = 4.0f * dl * dl + dc * dc + 4.0f * dh2;
			if (dist < best_dist) {
				best      = i;
				best_dist = dist;
			}
		}
		return best;
	}

	std::string Fmt(double v, int prec = 3) {
		std::ostringstream out;
		out << std::fixed << std::setprecision(prec) << v;
		return out.str();
	}
}    // namespace

/*
 *  The sky map: chamfer distances (1, sqrt 2) from the open columns, in
 *  two forward and backward raster sweeps through columns clear at lift 1.
 */

void Shaded_cpu::build_sky_open(const Shaded_grid_volume& vol, float fade_tiles, std::vector<uint8_t>& open) {
	const int w = vol.size_x;
	const int h = vol.size_y;
	open.assign(static_cast<size_t>(std::max(w, 0)) * static_cast<size_t>(std::max(h, 0)), 0);
	if (w <= 0 || h <= 0 || vol.sky.size() < open.size()) {
		return;
	}
	constexpr float      unreached  = std::numeric_limits<float>::max();
	constexpr float      diag = 1.41421356f;
	const int            lift = std::min(1, vol.size_z - 1);
	std::vector<float>   dist(open.size(), unreached);
	std::vector<uint8_t> pass(open.size(), 0);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
			dist[i]        = vol.sky[i] == 0 ? 0.0f : unreached;
			pass[i]        = vol.opacity(x, y, lift) < shaded_grid_sky_threshold ? 1 : 0;
		}
	}
	auto at = [&](int x, int y) {
		return x < 0 || y < 0 || x >= w || y >= h ? unreached : dist[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
	};
	auto relax = [&](int x, int y, int sx, int sy) {
		const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
		if (dist[i] == 0.0f || !pass[i]) {
			return;
		}
		dist[i] = std::min({dist[i], at(x - sx, y) + 1.0f, at(x, y - sy) + 1.0f, at(x - sx, y - sy) + diag,
							at(x + sx, y - sy) + diag});
	};
	// Twice: a path round a corner may need a second pair of sweeps.
	for (int round = 0; round < 2; round++) {
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				relax(x, y, 1, 1);
			}
		}
		for (int y = h - 1; y >= 0; y--) {
			for (int x = w - 1; x >= 0; x--) {
				relax(x, y, -1, -1);
			}
		}
	}
	for (size_t i = 0; i < open.size(); i++) {
		if (dist[i] == 0.0f) {
			open[i] = 255;
		} else if (fade_tiles > 0.0f && dist[i] < fade_tiles) {
			open[i] = static_cast<uint8_t>(std::clamp(std::lround(255.0f * (1.0f - dist[i] / fade_tiles)), 0L, 255L));
		}
	}
}

/*
 *  A cloud frame's soft mask.  The padding is twice the blur radius, the
 *  reach of two box passes, so the soft edge is never cut off.
 */

void Shaded_cpu::build_cloud_mask(
		const unsigned char* rle, int xleft, int yabove, int w, int h, int blur, std::vector<uint8_t>& data,
		Shaded_cloud_mask& mask) {
	blur          = std::max(blur, 0);
	const int pad = 2 * blur;
	mask.x0       = -xleft - pad;
	mask.y0       = -yabove - pad;
	mask.w        = std::max(w, 0) + 2 * pad;
	mask.h        = std::max(h, 0) + 2 * pad;
	mask.offset   = static_cast<uint32_t>(data.size());
	std::vector<uint8_t> img(static_cast<size_t>(mask.w) * static_cast<size_t>(mask.h), 0);
	For_rle_pixels(rle, [&](int x, int y) {
		const int mx = x - mask.x0;
		const int my = y - mask.y0;
		if (mx >= 0 && my >= 0 && mx < mask.w && my < mask.h) {
			img[static_cast<size_t>(my) * static_cast<size_t>(mask.w) + static_cast<size_t>(mx)] = 255;
		}
	});
	if (blur > 0) {
		for (int pass = 0; pass < 2; pass++) {
			Box_blur(img, mask.w, mask.h, blur, true);
			Box_blur(img, mask.w, mask.h, blur, false);
		}
	}
	data.insert(data.end(), img.begin(), img.end());
}

/*
 *  The fog tables: FOG as DAY mixed with a flat fog colour, fitted by
 *  least squares, then every (amount, light) row matched back to DAY.
 */

void Shaded_cpu::build_fog_tables(
		const uint8_t* day768, const uint8_t* fog768, float& fog_mix, float colour[3], uint8_t* table) {
	constexpr int first = 1;
	constexpr int last  = shaded_last_lit_index;
	constexpr int count = last - first + 1;
	float         a_sum = 0.0f;
	float         b[3];
	for (int c = 0; c < 3; c++) {
		double sx = 0, sy = 0, sxx = 0, sxy = 0;
		for (int i = first; i <= last; i++) {
			const double x = day768[3 * i + c];
			const double y = fog768[3 * i + c];
			sx += x;
			sy += y;
			sxx += x * x;
			sxy += x * y;
		}
		const double var = sxx - sx * sx / count;
		const double a   = var > 1e-9 ? (sxy - sx * sy / count) / var : 1.0;
		b[c]             = static_cast<float>((sy - a * sx) / count);
		a_sum += static_cast<float>(a);
	}
	fog_mix = std::clamp(1.0f - a_sum / 3.0f, 0.05f, 0.95f);
	for (int c = 0; c < 3; c++) {
		colour[c] = std::clamp(b[c] / fog_mix, 0.0f, 63.0f);
	}
	float day_lab[last + 1][3] = {};
	for (int i = first; i <= last; i++) {
		colour_lab(day768[3 * i], day768[3 * i + 1], day768[3 * i + 2], day_lab[i]);
	}
	for (int code = 0; code < shaded_fog_rows; code++) {
		uint8_t* row = table + code * 256;
		for (int i = 0; i < 256; i++) {
			row[i] = static_cast<uint8_t>(i);
		}
		const int fk = code & 7;
		const int fl = code >> 3;
		if (fk == 0) {
			continue;
		}
		const float amount = static_cast<float>(fk) / 8.0f;
		const float light  = static_cast<float>(fl) / 7.0f;
		for (int i = first; i <= last; i++) {
			int mixed[3];
			for (int c = 0; c < 3; c++) {
				const float v = static_cast<float>(day768[3 * i + c]) * (1.0f - amount) + colour[c] * light * amount;
				mixed[c]      = static_cast<int>(std::lround(v));
			}
			float lab[3];
			colour_lab(mixed[0], mixed[1], mixed[2], lab);
			row[i] = static_cast<uint8_t>(Nearest_fog_colour(day_lab, lab));
		}
	}
}

float Shaded_cpu::flash_grid_transmittance(const Shaded_grid_volume& vol, const Shaded_point& p, const Shaded_point& target) {
	const Shaded_grid_ray ray{
			{     p.x,      p.y,      p.z, 0.0f},
			{target.x, target.y, target.z, 0.0f}
	};
	return Shaded_grid_cpu::trace(vol, ray, 0).transmittance;
}

/*
 *  The flash tables: each relit DAY colour mixed towards its LIGHTNING
 *  colour in 7 steps, matched back to DAY as the remap tables are.
 */

void Shaded_cpu::build_flash_table(const uint8_t* day768, const uint8_t* lightning768, uint8_t* table) {
	constexpr int first = 1;
	constexpr int last  = shaded_last_lit_index;
	float         day_lab[last + 1][3] = {};
	for (int i = first; i <= last; i++) {
		colour_lab(day768[3 * i], day768[3 * i + 1], day768[3 * i + 2], day_lab[i]);
	}
	for (int k = 0; k < shaded_flash_rows; k++) {
		uint8_t* row = table + k * 256;
		for (int i = 0; i < 256; i++) {
			row[i] = static_cast<uint8_t>(i);
		}
		if (k == 0) {
			continue;
		}
		const float t = static_cast<float>(k) / static_cast<float>(shaded_flash_rows - 1);
		for (int i = first; i <= last; i++) {
			int mixed[3];
			for (int c = 0; c < 3; c++) {
				const float v = static_cast<float>(day768[3 * i + c]) * (1.0f - t) + static_cast<float>(lightning768[3 * i + c]) * t;
				mixed[c]      = static_cast<int>(std::lround(v));
			}
			float lab[3];
			colour_lab(mixed[0], mixed[1], mixed[2], lab);
			row[i] = static_cast<uint8_t>(nearest_colour_lab(day_lab, lab, first, last));
		}
	}
}

/*
 *  The pixels a rain drop or snowflake paints, translucent ones too.
 */

void Shaded_cpu::mark_spans(
		Shaded_scene_buffers& buf, const Shaded_rect& clip, const unsigned char* rle, int xoff, int yoff) {
	const int right  = clip.x + clip.w;
	const int bottom = clip.y + clip.h;
	For_rle_pixels(rle, [&](int x, int y) {
		const int px = xoff + x;
		const int py = yoff + y;
		if (px >= clip.x && px < right && py >= clip.y && py < bottom) {
			buf.particle_mask[static_cast<size_t>(buf.index(px, py))] = 1;
		}
	});
}

/*
 *  Synthetic tests: a roofed room with a doorway, a closed room, and a
 *  hand-made square cloud frame; with the game's palettes, the fog and
 *  flash tables.
 */

int Shaded_weather_selftest(const uint8_t* day768, const uint8_t* fog768, const uint8_t* lightning768) {
	int  passed   = 0;
	int  failures = 0;
	auto report   = [&](bool ok, const std::string& name, const std::string& detail) {
		(ok ? passed : failures)++;
		std::cout << "SHADED-WEATHER: TEST " << (ok ? "PASS " : "FAIL ") << name << " " << detail << std::endl;
	};
	// Room A: walls x, y 4..12 at lifts 0-4, a roof at lift 5 over the
	// inside (5..11), a doorway at (8, 12).  Room B: walls 15..21, roof over
	// 16..20, no way in.
	Shaded_grid_volume vol;
	vol.origin_tx = 1000;
	vol.origin_ty = 2000;
	vol.resize(24, 24, shaded_grid_lifts);
	constexpr uint8_t wall = shaded_cell_solid | shaded_cell_object;
	auto room = [&vol](int lo, int hi) {
		for (int i = lo; i <= hi; i++) {
			vol.stamp_box(i, lo, 0, i, lo, 4, 255, wall);
			vol.stamp_box(i, hi, 0, i, hi, 4, 255, wall);
			vol.stamp_box(lo, i, 0, lo, i, 4, 255, wall);
			vol.stamp_box(hi, i, 0, hi, i, 4, 255, wall);
		}
		vol.stamp_box(lo + 1, lo + 1, 5, hi - 1, hi - 1, 5, 255, wall | shaded_cell_roof);
	};
	room(4, 12);
	room(15, 21);
	for (int z = 0; z <= 4; z++) {
		vol.cells[vol.index(8, 12, z)]     = 0;
		vol.cells[vol.index(8, 12, z) + 1] = 0;
	}
	vol.rebuild_sky(0, 0, vol.size_x, vol.size_y, shaded_grid_sky_threshold);
	constexpr float      fade = 3.0f;
	std::vector<uint8_t> open;
	Shaded_cpu::build_sky_open(vol, fade, open);
	auto at = [&open, &vol](int x, int y) {
		return static_cast<int>(open[static_cast<size_t>(y) * static_cast<size_t>(vol.size_x) + static_cast<size_t>(x)]);
	};
	auto close_to = [](int v, double want) {
		return std::abs(static_cast<double>(v) - want) <= 2.0;
	};
	report(at(2, 2) == 255 && at(8, 13) == 255 && at(8, 12) == 255 && at(23, 0) == 255, "open-outside",
		   "open(2,2)=" + std::to_string(at(2, 2)) + " doorway=" + std::to_string(at(8, 12)));
	report(close_to(at(8, 11), 255.0 - 255.0 / 3.0), "open-doorway-1",
		   "open(8,11)=" + std::to_string(at(8, 11)) + " want 170");
	report(close_to(at(7, 11), 255.0 * (1.0 - 1.41421356 / 3.0)), "open-doorway-diagonal",
		   "open(7,11)=" + std::to_string(at(7, 11)) + " want 135");
	report(at(8, 8) == 0 && at(8, 9) == 0 && at(6, 6) == 0, "open-deep-inside",
		   "open(8,9)=" + std::to_string(at(8, 9)) + " open(8,8)=" + std::to_string(at(8, 8)));
	bool closed = true;
	for (int y = 15; y <= 21; y++) {
		for (int x = 15; x <= 21; x++) {
			closed = closed && at(x, y) == 0;
		}
	}
	report(closed, "open-closed-room", "open(18,18)=" + std::to_string(at(18, 18)));
	// Behind the wall beside the doorway: only the way round through the
	// doorway counts (3.41 tiles: 0), not 2 tiles through the wall (85).
	report(at(5, 11) == 0 && close_to(at(6, 11), 255.0 * (1.0 - 2.41421356 / 3.0)) && at(7, 12) == 0, "open-no-wall-leak",
		   "open(5,11)=" + std::to_string(at(5, 11)) + " open(6,11)=" + std::to_string(at(6, 11)) + " want 0, 50");
	// The per-pixel factor.
	Shaded_weather_set set;
	set.grid    = &vol;
	set.open_w  = vol.size_x;
	set.open_h  = vol.size_y;
	set.open_tx = vol.origin_tx;
	set.open_ty = vol.origin_ty;
	set.open    = open;
	const Shaded_normalf up{0.0f, 0.0f, 1.0f};
	const Shaded_normalf south{0.0f, 1.0f, 0.0f};
	auto o = [&set](float x, float y, float z, const Shaded_normalf& n) {
		return Shaded_weather_open(set, Shaded_point{static_cast<float>(set.open_tx) + x, static_cast<float>(set.open_ty) + y, z}, n);
	};
	const float roof_top = o(8.5f, 8.5f, 6.0f, up);
	const float floor_in = o(8.5f, 6.5f, 0.0f, up);
	const float wall_out = o(6.5f, 13.0f, 2.0f, south);
	const float wall_in  = o(8.5f, 5.0f, 2.0f, south);
	const float ground   = o(2.5f, 2.5f, 0.0f, up);
	report(roof_top == 1.0f, "weather-open-roof-top", "O=" + Fmt(roof_top));
	report(floor_in == 0.0f, "weather-open-floor-inside", "O=" + Fmt(floor_in));
	report(wall_out == 1.0f, "weather-open-south-face-outside", "O=" + Fmt(wall_out));
	report(wall_in == 0.0f, "weather-open-north-wall-inside", "O=" + Fmt(wall_in));
	report(ground == 1.0f, "weather-open-ground", "O=" + Fmt(ground));
	Shaded_weather_set none;
	report(o(8.5f, 6.5f, 0.0f, up) == 0.0f
				   && Shaded_weather_open(none, Shaded_point{1008.5f, 2006.5f, 0.0f}, up) == 1.0f,
		   "weather-open-no-grid", "O(no grid)=" + Fmt(Shaded_weather_open(none, Shaded_point{1008.5f, 2006.5f, 0.0f}, up)));
	// A 16 x 16 square frame round its origin (rows -8..7, plain runs).
	std::vector<unsigned char> rle;
	auto put2 = [&rle](int v) {
		rle.push_back(static_cast<unsigned char>(v & 0xff));
		rle.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
	};
	for (int y = -8; y < 8; y++) {
		put2(16 << 1);
		put2(-8);
		put2(y);
		rle.insert(rle.end(), 16, 5);
	}
	put2(0);
	constexpr int        blur = 3;
	std::vector<uint8_t> data(7, 0);    // Something before it: offsets are honoured.
	Shaded_cloud_mask    mask;
	Shaded_cpu::build_cloud_mask(rle.data(), 8, 8, 16, 16, blur, data, mask);
	auto mval = [&](int x, int y) {
		const int mx = x - mask.x0;
		const int my = y - mask.y0;
		if (mx < 0 || my < 0 || mx >= mask.w || my >= mask.h) {
			return 0;
		}
		return static_cast<int>(data[mask.offset + static_cast<size_t>(my) * static_cast<size_t>(mask.w) + static_cast<size_t>(mx)]);
	};
	const int centre = mval(0, 0);
	const int edge   = mval(7, 0);    // Last painted column.
	const int out    = mval(7 + 2 * blur + 1, 0);
	const int corner = mval(mask.x0, mask.y0);
	bool      mono   = true;
	for (int x = 0; x < mask.x0 + mask.w; x++) {
		mono = mono && mval(x + 1, 0) <= mval(x, 0);
	}
	report(mask.offset == 7 && mask.w == 16 + 4 * blur && mask.x0 == -8 - 2 * blur, "cloud-mask-layout",
		   "offset=" + std::to_string(mask.offset) + " w=" + std::to_string(mask.w) + " x0=" + std::to_string(mask.x0));
	report(centre == 255, "cloud-mask-centre", "centre=" + std::to_string(centre));
	report(out == 0 && corner == 0, "cloud-mask-outside", "outside=" + std::to_string(out) + " corner=" + std::to_string(corner));
	report(edge > 0 && edge < 255, "cloud-mask-edge", "edge=" + std::to_string(edge));
	report(mono, "cloud-mask-monotonic", "row 0 non-increasing towards the edge");
	// The cover lookup: a cloud with its origin at world pixel (800, 1600).
	set.masks.push_back(mask);
	set.mask_data = data;
	set.clouds.push_back(Shaded_cloud{800.0f, 1600.0f, 0, 0});
	const float under = Shaded_cloud_cover(set, Shaded_point{100.0625f, 200.0625f, 0.0f});
	const float away  = Shaded_cloud_cover(set, Shaded_point{110.0f, 200.0f, 0.0f});
	set.clouds.push_back(Shaded_cloud{800.0f, 1600.0f, 0, 0});
	const float twice = Shaded_cloud_cover(set, Shaded_point{100.0625f, 200.0625f, 0.0f});
	report(under == 1.0f && away == 0.0f && twice == 1.0f, "cloud-cover",
		   "under=" + Fmt(under) + " away=" + Fmt(away) + " two clouds=" + Fmt(twice));
	// The fog factor round an Avatar at (100.5, 200.5) on the ground.
	Shaded_light_params fp;
	fp.fog_mix  = 0.5f;
	fp.avatar_x = 100.5f;
	fp.avatar_y = 200.5f;
	auto fog_at = [&fp](float dx, float z, float open) {
		return Shaded_fog_factor(fp, Shaded_point{fp.avatar_x + dx, fp.avatar_y, z}, open);
	};
	const float f_closed = fog_at(20.0f, 0.0f, 0.0f);
	const float f_clear  = fog_at(fp.fog_clear - 0.25f, 0.0f, 1.0f);
	const float f_near   = fog_at(4.0f, 0.0f, 1.0f);
	const float f_mid    = fog_at(8.0f, 0.0f, 1.0f);
	const float f_far    = fog_at(30.0f, 0.0f, 1.0f);
	const float f_high   = fog_at(8.0f, 4.0f, 1.0f);
	const float f_wrap   = Shaded_fog_factor(fp, Shaded_point{fp.avatar_x + 8.0f + c_num_tiles, fp.avatar_y, 0.0f}, 1.0f);
	report(f_closed == 0.0f && f_clear == 0.0f, "fog-factor-clear",
		   "covered=" + Fmt(f_closed) + " within fog_clear=" + Fmt(f_clear));
	report(f_near > 0.0f && f_near < f_mid && f_mid < f_far && f_far <= fp.fog_mix, "fog-factor-distance",
		   "4 tiles=" + Fmt(f_near) + " 8=" + Fmt(f_mid) + " 30=" + Fmt(f_far));
	report(f_high < f_mid && f_high > 0.0f && std::abs(f_wrap - f_mid) < 1e-4f, "fog-factor-height",
		   "8 tiles at lift 4=" + Fmt(f_high) + " at 0=" + Fmt(f_mid) + " wrapped=" + Fmt(f_wrap));
	// The flash's grid transmittance in room A, from the point (x, y) of
	// the volume on the ground towards azimuth deg (0 = east, 90 = south)
	// at the flash's elevation, up to the top of the grid.
	auto flash_t = [&vol](float x, float y, float deg) {
		constexpr float to_rad = 3.14159265f / 180.0f;
		const float     e      = shaded_flash_elevation * to_rad;
		const float     dx     = std::cos(e) * std::cos(deg * to_rad);
		const float     dy     = std::cos(e) * std::sin(deg * to_rad);
		const float     dz     = std::sin(e) / 0.5f;    // Lifts per tile of path (half a tile per lift).
		const Shaded_point p{static_cast<float>(vol.origin_tx) + x, static_cast<float>(vol.origin_ty) + y, 0.0f};
		const float        s = shaded_flash_top / dz;
		return Shaded_cpu::flash_grid_transmittance(vol, p, Shaded_point{p.x + dx * s, p.y + dy * s, p.z + dz * s});
	};
	const float t_open    = flash_t(2.5f, 2.5f, 225.0f);    // North-west, over open ground.
	const float t_roof    = flash_t(8.5f, 8.5f, 90.0f);
	const float t_door    = flash_t(8.5f, 11.5f, 90.0f);    // Under the roof, out through the doorway.
	const float t_back    = flash_t(8.5f, 11.5f, 270.0f);    // The same point, towards the roof.
	const float t_outside = flash_t(8.5f, 13.5f, 90.0f);    // Just outside the doorway.
	report(t_open == 1.0f, "flash-grid-open", "T=" + Fmt(t_open));
	report(t_roof == 0.0f, "flash-grid-under-roof", "T=" + Fmt(t_roof));
	report(t_door == 1.0f && t_back == 0.0f, "flash-grid-through-doorway",
		   "T out=" + Fmt(t_door) + " T back under the roof=" + Fmt(t_back));
	report(t_outside == 1.0f, "flash-grid-outside-doorway", "T=" + Fmt(t_outside));
	// Rain and snow: under cover a drop keeps the surface's level, in the
	// open it takes the scattered light.
	const float p_covered = Shaded_particle_level(0.3f, 0.8f, 0.0f);
	const float p_open    = Shaded_particle_level(0.3f, 0.8f, 1.0f);
	const float p_half    = Shaded_particle_level(0.3f, 0.8f, 0.5f);
	report(p_covered == 0.3f && p_open == 0.8f && std::abs(p_half - 0.55f) < 1e-6f, "particle-level",
		   "covered " + Fmt(p_covered) + " open " + Fmt(p_open) + " half " + Fmt(p_half));
	if (day768 && lightning768) {
		std::vector<uint8_t> table(shaded_flash_table_size);
		Shaded_cpu::build_flash_table(day768, lightning768, table.data());
		bool identity = true;
		for (int k = 0; k < shaded_flash_rows; k++) {
			for (int i = 0; i < 256; i++) {
				const bool kept = k == 0 || i == 0 || i > shaded_last_lit_index;
				identity        = identity && (!kept || table[static_cast<size_t>(k) * 256 + static_cast<size_t>(i)] == i);
			}
		}
		// Row 7 is about as bright as LIGHTNING (relative to DAY).
		auto luma = [](const uint8_t* pal, int i) {
			return Shaded_luminance(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]);
		};
		double y_day = 0.0, y_row7 = 0.0, y_lightning = 0.0;
		for (int i = 1; i <= shaded_last_lit_index; i++) {
			y_day += luma(day768, i);
			y_row7 += luma(day768, table[7 * 256 + i]);
			y_lightning += luma(lightning768, i);
		}
		report(identity, "flash-table-identity", "row 0, index 0 and cycling colours unchanged");
		report(y_row7 >= y_day && std::abs(y_row7 - y_lightning) <= 0.15 * y_lightning, "flash-table-brightens",
			   "mean Y day " + Fmt(y_day / shaded_last_lit_index / 63.0, 1) + " row 7 "
					   + Fmt(y_row7 / shaded_last_lit_index / 63.0, 1) + " lightning "
					   + Fmt(y_lightning / shaded_last_lit_index / 63.0, 1));
	}
	if (day768 && fog768) {
		std::vector<uint8_t> table(shaded_fog_table_size);
		float                mix = 0.0f;
		float                colour[3];
		Shaded_cpu::build_fog_tables(day768, fog768, mix, colour, table.data());
		bool identity = true;
		for (int code = 0; code < shaded_fog_rows; code++) {
			for (int i = 0; i < 256; i++) {
				const bool kept = (code & 7) == 0 || i == 0 || i > shaded_last_lit_index;
				identity        = identity && (!kept || table[static_cast<size_t>(code) * 256 + static_cast<size_t>(i)] == i);
			}
		}
		report(identity, "fog-table-identity", "rows fk=0, index 0 and cycling colours unchanged");
		// The far fog on the ground (fk = round(8 w), full light) against
		// FOG: the mix itself (before matching), the table's DAY colours,
		// and the best DAY can do (each FOG colour's own nearest DAY colour;
		// FOG's pale greys are not in DAY, so the table cannot beat it).
		const int fk   = std::clamp(static_cast<int>(std::lround(8.0f * mix)), 1, 7);
		const int code = fk + 8 * 7;
		auto lab_of = [](const uint8_t* pal, int i, float lab[3]) {
			Shaded_cpu::colour_lab(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2], lab);
		};
		auto dist = [](const float a[3], const float b[3]) {
			return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
		};
		float day_lab[shaded_last_lit_index + 1][3] = {};
		for (int i = 1; i <= shaded_last_lit_index; i++) {
			lab_of(day768, i, day_lab[i]);
		}
		double de_mix = 0.0, de_table = 0.0, de_best = 0.0;
		for (int i = 1; i <= shaded_last_lit_index; i++) {
			float want[3], mixed[3];
			lab_of(fog768, i, want);
			int m[3];
			for (int c = 0; c < 3; c++) {
				const float amount = static_cast<float>(fk) / 8.0f;
				m[c] = static_cast<int>(std::lround(static_cast<float>(day768[3 * i + c]) * (1.0f - amount) + colour[c] * amount));
			}
			Shaded_cpu::colour_lab(m[0], m[1], m[2], mixed);
			de_mix += dist(mixed, want);
			de_table += dist(day_lab[table[static_cast<size_t>(code) * 256 + static_cast<size_t>(i)]], want);
			de_best += dist(day_lab[Nearest_fog_colour(day_lab, want)], want);
		}
		de_mix /= shaded_last_lit_index;
		de_table /= shaded_last_lit_index;
		de_best /= shaded_last_lit_index;
		report(de_mix < 6.0 && de_table < de_best + 1.0, "fog-table-fits-fog",
			   "mean dE mix " + Fmt(de_mix, 2) + " table " + Fmt(de_table, 2) + " (FOG to nearest DAY " + Fmt(de_best, 2)
					   + ") row fk " + std::to_string(fk) + " fl 7, mix " + Fmt(mix) + " colour " + Fmt(colour[0], 1) + ","
					   + Fmt(colour[1], 1) + "," + Fmt(colour[2], 1));
	}
	std::cout << "SHADED-WEATHER: self-test " << passed << " passed, " << failures << " failed" << std::endl;
	return failures;
}
