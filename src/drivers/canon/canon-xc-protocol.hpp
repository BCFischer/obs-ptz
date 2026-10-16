/* Pan Tilt Zoom Canon XC protocol helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * What the Canon XC driver needs of the protocol that doesn't need Qt or OBS:
 * reading the camera's "name:=value" answers, its limits, and turning the
 * movement API's normalized speeds and positions into XC parameters. Kept
 * free of both so that tests/unit can check it on its own.
 *
 * Parameter names and ranges are from "Canon XC Control Protocol
 * Specifications" 1.1, appendix iii, "Parameter List of info.cgi and
 * control.cgi". Every command is under /-wvhttp-01-/.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace canon_xc {

/* A request's parameters, in order, unencoded */
using Params = std::vector<std::pair<std::string, std::string>>;
/* An answer's items, by name */
using Items = std::map<std::string, std::string>;

/* Reads an info.cgi or control.cgi answer: a line per item, "name:=value" for
 * a value that changed, or "name==value" for one that did not. "name==?" is
 * a value the camera ignored, which is kept as "?" */
inline Items parseItems(const std::string &body)
{
	Items items;
	size_t start = 0;
	while (start < body.size()) {
		size_t end = body.find('\n', start);
		if (end == std::string::npos)
			end = body.size();
		std::string line = body.substr(start, end - start);
		start = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.pop_back();
		/* The first operator: a value (a preset's name) may have either */
		size_t op = std::min(line.find(":="), line.find("=="));
		if (op == std::string::npos || op == 0)
			continue;
		items[line.substr(0, op)] = line.substr(op + 2);
	}
	return items;
}

inline bool readInt(const Items &items, const std::string &name, int &out)
{
	auto it = items.find(name);
	if (it == items.end() || it->second.empty() || it->second == "?")
		return false;
	char *end = nullptr;
	long v = std::strtol(it->second.c_str(), &end, 10);
	if (end == it->second.c_str())
		return false;
	out = (int)v;
	return true;
}

/* Pan/Tilt speed modes, c.1.pan.speed.mode and c.1.tilt.speed.mode:
 * Manual: c.1.pan.speed, a constant speed whatever the zoom
 * Auto1:  c.1.pan.speed.ratio, in screens a second (100 = one screen width a
 *         second), so the picture moves at the same rate at any zoom
 * Auto2:  c.1.pan.speed, the speed at the wide end, slowed as it zooms in
 *         (the camera's default) */
enum class SpeedMode { Manual, Auto1, Auto2 };

inline const char *speedModeName(SpeedMode mode)
{
	switch (mode) {
	case SpeedMode::Manual:
		return "manual";
	case SpeedMode::Auto1:
		return "auto1";
	default:
		return "auto2";
	}
}

inline SpeedMode speedModeFromName(const std::string &name)
{
	if (name == "manual")
		return SpeedMode::Manual;
	if (name == "auto1")
		return SpeedMode::Auto1;
	return SpeedMode::Auto2;
}

/* What the camera says it can do. The defaults are the spec's, for a camera
 * that has yet to say, or doesn't say some of it */
struct Limits {
	/* 0.01 degrees, 0 is straight ahead; right and up are positive */
	int panMin = -17000, panMax = 17000;
	int tiltMin = -3000, tiltMax = 9000;
	/* Horizontal angle of view in 0.01 degrees: zoomMin is the telephoto
	 * end, zoomMax the wide end */
	int zoomMin = 570, zoomMax = 7300;
	int panSpeedMin = 10, panSpeedMax = 10000;
	int tiltSpeedMin = 10, tiltSpeedMax = 10000;
	int panRatioMin = 1, panRatioMax = 1000;
	int tiltRatioMin = 1, tiltRatioMax = 1000;
	int zoomSpeedMin = 0, zoomSpeedMax = 15;
	int focusSpeedMin = 0, focusSpeedMax = 2;
	int presetCount = 100;
	/* Whether it has the angle of view linkage, the speed modes; one that
	 * doesn't is sent c.1.pan.speed alone */
	bool speedModes = false;
};

/* Takes what `items` says of the limits; leaves the rest as it was */
inline void readLimits(Limits &l, const Items &items)
{
	readInt(items, "c.1.pan.min", l.panMin);
	readInt(items, "c.1.pan.max", l.panMax);
	readInt(items, "c.1.tilt.min", l.tiltMin);
	readInt(items, "c.1.tilt.max", l.tiltMax);
	readInt(items, "c.1.zoom.min", l.zoomMin);
	readInt(items, "c.1.zoom.max", l.zoomMax);
	readInt(items, "c.1.pan.speed.min", l.panSpeedMin);
	readInt(items, "c.1.pan.speed.max", l.panSpeedMax);
	readInt(items, "c.1.tilt.speed.min", l.tiltSpeedMin);
	readInt(items, "c.1.tilt.speed.max", l.tiltSpeedMax);
	readInt(items, "c.1.pan.speed.ratio.min", l.panRatioMin);
	readInt(items, "c.1.pan.speed.ratio.max", l.panRatioMax);
	readInt(items, "c.1.tilt.speed.ratio.min", l.tiltRatioMin);
	readInt(items, "c.1.tilt.speed.ratio.max", l.tiltRatioMax);
	readInt(items, "c.1.zoom.speed.min", l.zoomSpeedMin);
	readInt(items, "c.1.zoom.speed.max", l.zoomSpeedMax);
	readInt(items, "c.1.focus.speed.min", l.focusSpeedMin);
	readInt(items, "c.1.focus.speed.max", l.focusSpeedMax);
	readInt(items, "p.count", l.presetCount);
	auto modes = items.find("c.1.pan.speed.mode.list");
	if (modes != items.end())
		l.speedModes = modes->second.find("auto1") != std::string::npos;
	/* Guard against a camera that says nonsense, which the conversions
	 * below would divide by */
	if (l.panMax <= 0)
		l.panMax = 17000;
	if (l.panMin >= 0)
		l.panMin = -17000;
	if (l.tiltMax <= 0)
		l.tiltMax = 9000;
	if (l.tiltMin >= 0)
		l.tiltMin = -3000;
	if (l.zoomMin <= 0 || l.zoomMax <= l.zoomMin) {
		l.zoomMin = 570;
		l.zoomMax = 7300;
	}
}

/* How the driver turns a joystick into speeds */
struct SpeedSettings {
	SpeedMode mode = SpeedMode::Auto1;
	/* In Auto1, the speed at full deflection, in screens a second */
	double screensPerSecond = 1.0;
	/* The response curve: speed grows as deflection^curve, so that a
	 * small push moves the camera slowly. 1.0 is linear. */
	double curve = 2.0;
};

/* A speed in [lo, hi] for a deflection in (0, 1]: lo for the least, hi for full */
inline int scaleSpeed(double magnitude, int lo, int hi, double curve)
{
	magnitude = std::clamp(std::fabs(magnitude), 0.0, 1.0);
	if (curve < 0.1)
		curve = 0.1;
	if (hi < lo)
		std::swap(hi, lo);
	return (int)std::lround(lo + (hi - lo) * std::pow(magnitude, curve));
}

/* The speed parameters of one axis ("pan" or "tilt") at deflection `speed` */
inline void addAxisSpeed(Params &p, const std::string &axis, double speed, const SpeedSettings &s, const Limits &l)
{
	const bool pan = axis == "pan";
	const std::string prefix = "c.1." + axis + ".speed";
	if (l.speedModes && s.mode == SpeedMode::Auto1) {
		int lo = pan ? l.panRatioMin : l.tiltRatioMin;
		int top = pan ? l.panRatioMax : l.tiltRatioMax;
		int hi = std::clamp((int)std::lround(s.screensPerSecond * 100.0), lo, top);
		p.push_back({prefix + ".mode", speedModeName(s.mode)});
		p.push_back({prefix + ".ratio", std::to_string(scaleSpeed(speed, lo, hi, s.curve))});
		return;
	}
	int lo = pan ? l.panSpeedMin : l.tiltSpeedMin;
	int hi = pan ? l.panSpeedMax : l.tiltSpeedMax;
	if (l.speedModes)
		p.push_back({prefix + ".mode", speedModeName(s.mode)});
	p.push_back({prefix, std::to_string(scaleSpeed(speed, lo, hi, s.curve))});
}

/* A continuous move, or a stop, of pan and tilt at once: positive is right
 * and up, magnitude the deflection in [0, 1] */
inline Params panTiltMove(double pan, double tilt, const SpeedSettings &s, const Limits &l)
{
	Params p;
	if (pan == 0.0) {
		p.push_back({"c.1.pan", "stop"});
	} else {
		p.push_back({"c.1.pan", pan > 0 ? "right" : "left"});
		addAxisSpeed(p, "pan", pan, s, l);
	}
	if (tilt == 0.0) {
		p.push_back({"c.1.tilt", "stop"});
	} else {
		p.push_back({"c.1.tilt", tilt > 0 ? "up" : "down"});
		addAxisSpeed(p, "tilt", tilt, s, l);
	}
	return p;
}

/* A continuous zoom, or a stop: positive is toward telephoto */
inline Params zoomMove(double speed, double curve, const Limits &l)
{
	Params p;
	if (speed == 0.0) {
		p.push_back({"c.1.zoom", "stop"});
		return p;
	}
	p.push_back({"c.1.zoom", speed > 0 ? "tele" : "wide"});
	p.push_back({"c.1.zoom.speed", std::to_string(scaleSpeed(speed, l.zoomSpeedMin, l.zoomSpeedMax, curve))});
	return p;
}

/* A continuous focus, or a stop: positive is toward near. It is a manual
 * focus, so autofocus is turned off with it. Its speed is left alone:
 * c.1.focus.speed is the camera's own setting, not a session's, and would
 * change it for every other controller too. */
inline Params focusMove(double speed, const Limits &l)
{
	(void)l;
	Params p;
	if (speed == 0.0) {
		p.push_back({"c.1.focus.action", "stop"});
		return p;
	}
	p.push_back({"c.1.focus", "manual"});
	p.push_back({"c.1.focus.action", speed > 0 ? "near" : "far"});
	return p;
}

/* Positions, between the camera's units and the movement API's: pan and tilt
 * in [-1, 1] with 0 straight ahead (each side scaled to its own limit, as the
 * range isn't symmetrical), zoom in [0, 1] from wide to telephoto */
inline double signedToApi(int raw, int min, int max)
{
	return raw >= 0 ? (double)raw / max : (double)raw / -min;
}

inline int apiToSigned(double v, int min, int max)
{
	v = std::clamp(v, -1.0, 1.0);
	return (int)std::lround(v >= 0 ? v * max : v * -min);
}

/* The angle of view shrinks geometrically as it zooms, so the API's zoom is
 * taken as its logarithm: 0.5 is halfway in magnification, which is what a
 * zoom slider ought to feel like */
inline double zoomToApi(int raw, const Limits &l)
{
	raw = std::clamp(raw, l.zoomMin, l.zoomMax);
	return std::log((double)l.zoomMax / raw) / std::log((double)l.zoomMax / l.zoomMin);
}

inline int apiToZoom(double z, const Limits &l)
{
	z = std::clamp(z, 0.0, 1.0);
	return (int)std::lround(l.zoomMax * std::pow((double)l.zoomMin / l.zoomMax, z));
}

/* A move by a distance, as a fraction of the axis's whole range: "d<n>" in
 * 0.01 degrees. Empty if it rounds to nothing, which the camera would refuse
 * ("-0" is a 403) and which would not move anyway. No '+', which a query
 * string would read as a space. */
inline std::string relativeValue(double fraction, int min, int max)
{
	long diff = std::lround(std::clamp(fraction, -1.0, 1.0) * (double)(max - min));
	if (diff == 0)
		return {};
	return "d" + std::to_string(diff);
}

/* The camera's presets, from an info.cgi answer with item "p": the slots in
 * use, by number, and their names ("" for none) */
inline std::vector<std::pair<int, std::string>> readPresets(const Items &items, int count)
{
	std::vector<std::pair<int, std::string>> presets;
	for (int i = 1; i <= count; i++) {
		const std::string base = "p." + std::to_string(i);
		auto content = items.find(base + ".content");
		if (content == items.end() || content->second != "enabled")
			continue;
		auto name = items.find(base + ".name.utf8");
		presets.push_back({i, name == items.end() ? std::string() : name->second});
	}
	return presets;
}

} // namespace canon_xc
