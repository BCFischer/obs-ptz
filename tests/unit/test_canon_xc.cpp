/* Tests for the Canon XC protocol helpers (src/drivers/canon/canon-xc-protocol.hpp)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <catch_amalgamated.hpp>
#include "canon-xc-protocol.hpp"

using namespace canon_xc;

namespace {
std::string get(const Params &p, const std::string &name)
{
	for (const auto &[k, v] : p)
		if (k == name)
			return v;
	return "<absent>";
}

/* What a CR-N500 says, from the spec's sample info.cgi answer */
Limits sampleLimits()
{
	Limits l;
	readLimits(l, parseItems("c.1.pan.min:=-17000\r\n"
				 "c.1.pan.max:=17000\r\n"
				 "c.1.tilt.min:=-3000\n"
				 "c.1.tilt.max:=9000\n"
				 "c.1.zoom.min:=570\n"
				 "c.1.zoom.max:=7300\n"
				 "c.1.zoom.speed.min:=0\n"
				 "c.1.zoom.speed.max:=15\n"
				 "c.1.pan.speed.mode.list:=manual,auto1,auto2\n"
				 "c.1.pan.speed.min:=10\n"
				 "c.1.pan.speed.max:=10000\n"
				 "c.1.pan.speed.ratio.min:=1\n"
				 "c.1.pan.speed.ratio.max:=1000\n"
				 "c.1.tilt.speed.ratio.min:=1\n"
				 "c.1.tilt.speed.ratio.max:=1000\n"
				 "p.count:=100\n"));
	return l;
}
} // namespace

TEST_CASE("canon-xc: parses both answer forms", "[canon-xc]")
{
	auto items = parseItems("c.1.exp==auto\nc.1.ae.brightness:=2\r\nc.1.me.shutter==?\ngarbage\n");
	REQUIRE(items["c.1.exp"] == "auto");
	REQUIRE(items["c.1.ae.brightness"] == "2");
	REQUIRE(items["c.1.me.shutter"] == "?");
	REQUIRE(items.count("garbage") == 0);
	/* A value can have either operator in it */
	REQUIRE(parseItems("p.3.name.utf8==a:=b\n")["p.3.name.utf8"] == "a:=b");
	int v = 0;
	REQUIRE_FALSE(readInt(items, "c.1.me.shutter", v));
	REQUIRE(readInt(items, "c.1.ae.brightness", v));
	REQUIRE(v == 2);
}

TEST_CASE("canon-xc: reads limits and the speed modes", "[canon-xc]")
{
	Limits l = sampleLimits();
	REQUIRE(l.speedModes);
	REQUIRE(l.tiltMax == 9000);
	Limits none;
	readLimits(none, parseItems("c.1.pan.max:=0\n"));
	REQUIRE_FALSE(none.speedModes);
	REQUIRE(none.panMax == 17000); /* nonsense is ignored */
}

TEST_CASE("canon-xc: auto1 pans in screens a second", "[canon-xc]")
{
	Limits l = sampleLimits();
	SpeedSettings s;
	s.mode = SpeedMode::Auto1;
	s.screensPerSecond = 1.5;
	s.curve = 1.0;
	auto p = panTiltMove(1.0, -0.5, s, l);
	REQUIRE(get(p, "c.1.pan") == "right");
	REQUIRE(get(p, "c.1.pan.speed.mode") == "auto1");
	REQUIRE(get(p, "c.1.pan.speed.ratio") == "150");
	REQUIRE(get(p, "c.1.tilt") == "down");
	REQUIRE(get(p, "c.1.tilt.speed.ratio") == "76"); /* 1 + 149 * 0.5, rounded */
	REQUIRE(get(p, "c.1.pan.speed") == "<absent>");
}

TEST_CASE("canon-xc: response curve slows small deflections", "[canon-xc]")
{
	REQUIRE(scaleSpeed(0.5, 0, 100, 2.0) == 25);
	REQUIRE(scaleSpeed(1.0, 0, 100, 2.0) == 100);
	REQUIRE(scaleSpeed(0.0, 10, 100, 2.0) == 10);
	REQUIRE(scaleSpeed(-1.0, 10, 100, 2.0) == 100);
}

TEST_CASE("canon-xc: auto2 and manual use pan.speed", "[canon-xc]")
{
	Limits l = sampleLimits();
	SpeedSettings s;
	s.mode = SpeedMode::Auto2;
	s.curve = 1.0;
	auto p = panTiltMove(-1.0, 0.0, s, l);
	REQUIRE(get(p, "c.1.pan") == "left");
	REQUIRE(get(p, "c.1.pan.speed.mode") == "auto2");
	REQUIRE(get(p, "c.1.pan.speed") == "10000");
	REQUIRE(get(p, "c.1.tilt") == "stop");
	REQUIRE(get(p, "c.1.tilt.speed") == "<absent>");
}

TEST_CASE("canon-xc: a camera without speed modes isn't sent them", "[canon-xc]")
{
	Limits l; /* speedModes false */
	SpeedSettings s;
	auto p = panTiltMove(0.3, 0.0, s, l);
	REQUIRE(get(p, "c.1.pan.speed.mode") == "<absent>");
	REQUIRE(get(p, "c.1.pan.speed.ratio") == "<absent>");
	REQUIRE(get(p, "c.1.pan.speed") != "<absent>");
}

TEST_CASE("canon-xc: zoom and focus moves", "[canon-xc]")
{
	Limits l = sampleLimits();
	auto z = zoomMove(1.0, 1.0, l);
	REQUIRE(get(z, "c.1.zoom") == "tele");
	REQUIRE(get(z, "c.1.zoom.speed") == "15");
	REQUIRE(get(zoomMove(-0.01, 1.0, l), "c.1.zoom") == "wide");
	REQUIRE(get(zoomMove(0.0, 1.0, l), "c.1.zoom") == "stop");
	auto f = focusMove(-1.0, l);
	REQUIRE(get(f, "c.1.focus") == "manual");
	REQUIRE(get(f, "c.1.focus.action") == "far");
	REQUIRE(get(focusMove(0.0, l), "c.1.focus.action") == "stop");
}

TEST_CASE("canon-xc: positions round trip", "[canon-xc]")
{
	Limits l = sampleLimits();
	REQUIRE(signedToApi(9000, l.tiltMin, l.tiltMax) == Catch::Approx(1.0));
	REQUIRE(signedToApi(-3000, l.tiltMin, l.tiltMax) == Catch::Approx(-1.0));
	REQUIRE(apiToSigned(-0.5, l.tiltMin, l.tiltMax) == -1500);
	REQUIRE(apiToSigned(0.5, l.panMin, l.panMax) == 8500);
	REQUIRE(zoomToApi(7300, l) == Catch::Approx(0.0));
	REQUIRE(zoomToApi(570, l) == Catch::Approx(1.0));
	for (double z : {0.0, 0.25, 0.5, 0.9, 1.0})
		REQUIRE(zoomToApi(apiToZoom(z, l), l) == Catch::Approx(z).margin(0.001));
	/* halfway in magnification is the geometric mean of the angles */
	REQUIRE(apiToZoom(0.5, l) == (int)std::lround(std::sqrt(570.0 * 7300.0)));
}

TEST_CASE("canon-xc: relative moves never send -0 or '+'", "[canon-xc]")
{
	REQUIRE(relativeValue(0.0, -17000, 17000).empty());
	REQUIRE(relativeValue(-0.00001, -17000, 17000).empty());
	REQUIRE(relativeValue(0.1, -17000, 17000) == "d3400");
	REQUIRE(relativeValue(-0.1, -3000, 9000) == "d-1200");
}

TEST_CASE("canon-xc: reads the presets in use", "[canon-xc]")
{
	auto items = parseItems("p.count:=100\n"
				"p.1.name.utf8:=Pulpit\np.1.content:=enabled\n"
				"p.2.name.utf8:=\np.2.content:=disabled\n"
				"p.7.content:=enabled\n");
	auto presets = readPresets(items, 100);
	REQUIRE(presets.size() == 2);
	REQUIRE(presets[0] == std::make_pair(1, std::string("Pulpit")));
	REQUIRE(presets[1] == std::make_pair(7, std::string()));
}
