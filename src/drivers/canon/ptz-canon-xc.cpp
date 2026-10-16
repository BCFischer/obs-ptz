/* Pan Tilt Zoom Canon XC protocol driver
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Talks to the camera without a session ("sessionless" in the spec): each
 * control.cgi takes the camera's control for itself, so the driver shares
 * the camera with an RC-IP100 or the camera's own web page without having to
 * claim, keep and yield control. As the speed settings are a session's own,
 * each move carries its speed mode and speed with it.
 */

#include <QAuthenticator>
#include <QUrl>
#include <QUrlQuery>
#include <qt-wrappers.hpp>
#include "ptz-canon-xc.hpp"
#include "ptz.h"

using canon_xc::Items;
using canon_xc::Params;

static constexpr int kPollIdleMs = 5000;
static constexpr int kPollMovingMs = 300;
static constexpr int kRequestTimeoutMs = 3000;

static std::string str(const QString &s)
{
	return s.toStdString();
}

static std::string num(long v)
{
	return std::to_string(v);
}

PTZCanonXC::PTZCanonXC(OBSData config, obs_source_t *source) : PTZDevice(config, source)
{
	connect(&m_network, &QNetworkAccessManager::authenticationRequired, this,
		[this](QNetworkReply *, QAuthenticator *auth) {
			auth->setUser(username);
			auth->setPassword(password);
		});
	connect(&m_network, &QNetworkAccessManager::finished, this, &PTZCanonXC::requestFinished);
	m_gapTimer.setSingleShot(true);
	connect(&m_gapTimer, &QTimer::timeout, this, &PTZCanonXC::pump);
	m_pollTimer.setInterval(kPollIdleMs);
	connect(&m_pollTimer, &QTimer::timeout, this, &PTZCanonXC::pollPosition);
	m_pollTimer.start();
	m_tallyTimer.setSingleShot(true);
	m_tallyTimer.setInterval(0);
	connect(&m_tallyTimer, &QTimer::timeout, this, &PTZCanonXC::applyTally);
	update(config);
}

PTZCanonXC::~PTZCanonXC()
{
	m_queue.clear();
	if (m_inFlight) {
		QNetworkReply *reply = m_inFlight;
		m_inFlight = nullptr;
		reply->abort();
	}
}

/*
 * Requests
 */

void PTZCanonXC::enqueue(const QString &command, const Params &params, Done done, bool poll)
{
	/* A poll waiting already will read everything another would */
	if (poll)
		for (const auto &r : m_queue)
			if (r.poll)
				return;
	m_queue.append({command, params, std::move(done), poll});
	pump();
}

void PTZCanonXC::pump()
{
	if (m_inFlight || effectiveHost().isEmpty())
		return;
	if (m_sinceLast.isValid() && m_sinceLast.elapsed() < kMinGapMs) {
		if (!m_gapTimer.isActive())
			m_gapTimer.start(kMinGapMs - (int)m_sinceLast.elapsed());
		return;
	}
	/* Movement goes first, but takes turns with what is waiting, so that a
	 * stick that never stops changing doesn't hold up a recall or a poll */
	if (!(m_lastWasMovement && !m_queue.isEmpty()) && sendMovement()) {
		m_lastWasMovement = true;
		return;
	}
	m_lastWasMovement = false;
	if (m_queue.isEmpty())
		return;
	Request r = m_queue.takeFirst();
	send(r.command, r.params, r.done);
}

void PTZCanonXC::send(const QString &command, const Params &params, Done done)
{
	QStringList query;
	for (const auto &[name, value] : params)
		query << QString::fromUtf8(QUrl::toPercentEncoding(QString::fromStdString(name))) + "=" +
				 QString::fromUtf8(QUrl::toPercentEncoding(QString::fromStdString(value)));
	QUrl url;
	url.setScheme("http");
	url.setHost(effectiveHost());
	url.setPort(port);
	url.setPath("/-wvhttp-01-/" + command);
	if (!query.isEmpty())
		url.setQuery(query.join('&'), QUrl::StrictMode);
	ptz_debug_trace("--> %s", QT_TO_UTF8(url.toString(QUrl::RemoveUserInfo)));

	QNetworkRequest request(url);
	request.setTransferTimeout(kRequestTimeoutMs);
	m_inFlightDone = std::move(done);
	m_inFlight = m_network.get(request);
	incrementStatistic("canon_xc_requests");
}

void PTZCanonXC::requestFinished(QNetworkReply *reply)
{
	reply->deleteLater();
	/* One aborted when the host changed, or as the device went */
	if (reply != m_inFlight)
		return;
	m_inFlight = nullptr;
	m_sinceLast.restart();
	Done done = std::move(m_inFlightDone);
	m_inFlightDone = nullptr;

	bool ok = false;
	Items items;
	if (reply->error() != QNetworkReply::NoError) {
		ptz_info("request failed: %s", QT_TO_UTF8(reply->errorString()));
		incrementStatistic("canon_xc_errors");
		/* A camera that answers is there, whatever it said */
		const bool answered = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid();
		if (answered) {
			m_consecutiveFailures = 0;
		} else if (++m_consecutiveFailures >= 3 && isConnected()) {
			setConnected(false);
			m_haveLimits = false;
		}
	} else {
		m_consecutiveFailures = 0;
		if (!isConnected())
			setConnected(true);
		/* Errors in the protocol come back as 200 OK, with the
		 * Livescope-Status header saying what went wrong */
		const QByteArray status = reply->rawHeader("Livescope-Status").trimmed();
		const QByteArray body = reply->readAll();
		ptz_debug_trace("<-- %s %s", status.constData(), body.left(200).constData());
		if (!status.isEmpty() && status != "0") {
			if (status.startsWith("301"))
				ptz_info("camera is being controlled by something else (Livescope status %s)",
					 status.constData());
			else
				ptz_info("camera refused the request: Livescope status %s", status.constData());
			incrementStatistic("canon_xc_errors");
		} else {
			ok = true;
			items = canon_xc::parseItems(body.toStdString());
		}
	}
	if (done)
		done(ok, items);
	pump();
}

/* Sends what the movement API was last told, if it changed, one of pan and
 * tilt, zoom or focus at a time: the spec warns that a request with several
 * mechanical operations may not carry them all out, and a stop must not be
 * lost. Says whether it sent one. */
bool PTZCanonXC::sendMovement()
{
	bool *flag;
	Params p;
	if (pantilt_changed) {
		flag = &pantilt_changed;
		p = canon_xc::panTiltMove(pan_speed, tilt_speed, m_speed, m_limits);
	} else if (zoom_changed) {
		flag = &zoom_changed;
		p = canon_xc::zoomMove(zoom_speed, m_speed.curve, m_limits);
	} else if (focus_changed) {
		flag = &focus_changed;
		p = canon_xc::focusMove(focus_speed, m_limits);
	} else {
		return false;
	}
	*flag = false;
	send("control.cgi", p, [this, flag](bool ok, const Items &items) {
		if (ok) {
			m_moveRetries = 0;
			readState(items);
			return;
		}
		/* Refused or lost: send the latest again, a stop above all, unless
		 * something newer is going anyway. A few times, not for ever */
		if (++m_moveRetries <= kMoveRetries) {
			*flag = true;
		} else {
			ptz_info("camera did not take a move or stop after %d tries", kMoveRetries + 1);
			m_moveRetries = 0;
		}
	});
	return true;
}

/*
 * Reading the camera back
 */

void PTZCanonXC::connectCamera()
{
	m_queue.clear();
	if (m_inFlight) {
		QNetworkReply *reply = m_inFlight;
		m_inFlight = nullptr;
		m_inFlightDone = nullptr;
		reply->abort();
	}
	m_haveLimits = false;
	m_presetsKnown = false;
	m_limits = canon_xc::Limits();
	m_consecutiveFailures = 0;
	m_moveRetries = 0;
	m_recallWatching = false;
	/* The presets being made were the old camera's: none was made */
	const auto creating = m_creatingSlots.keys();
	m_creatingSlots.clear();
	for (const QString &request : creating)
		QMetaObject::invokeMethod(
			this, [this, request]() { cameraPresetCreated(request, QString()); }, Qt::QueuedConnection);
	if (isConnected())
		setConnected(false);
	if (effectiveHost().isEmpty())
		return;
	pollPosition();
}

/* Reads where the camera is pointing, or everything it says of itself if it
 * hasn't said yet */
void PTZCanonXC::pollPosition()
{
	if (effectiveHost().isEmpty())
		return;
	if (!m_haveLimits) {
		/* Everything about the camera, its presets and tally lamp */
		enqueue(
			"info.cgi", {{"item", "c.1"}, {"item", "p"}, {"item", "f"}},
			[this](bool ok, const Items &items) {
				if (!ok || m_haveLimits)
					return;
				canon_xc::readLimits(m_limits, items);
				m_haveLimits = true;
				auto model = items.find("c.1.type");
				ptz_info("connected to %s; zoom-linked speeds %s",
					 model != items.end() ? model->second.c_str() : "a Canon XC camera",
					 m_limits.speedModes ? "supported" : "not supported");
				readState(items);
				readPresets(items);
				featuresChanged();
				m_tallyTimer.start();
			},
			true);
		return;
	}
	Params items = {{"item", "c.1.pan"}, {"item", "c.1.tilt"}, {"item", "c.1.zoom"}, {"item", "c.1.focus"}};
	/* A recall that never seemed to finish is left to the device's backstop */
	if (m_recallWatching && m_recallClock.elapsed() > kRecallGiveUpMs) {
		m_recallWatching = false;
		m_pollTimer.setInterval(kPollIdleMs);
	}
	if (m_recallWatching)
		items.push_back({"item", "p.status"});
	enqueue(
		"info.cgi", items,
		[this](bool ok, const Items &answer) {
			if (!ok)
				return;
			/* The zoom's range changes with the digital zoom mode */
			canon_xc::readInt(answer, "c.1.zoom.min", m_limits.zoomMin);
			canon_xc::readInt(answer, "c.1.zoom.max", m_limits.zoomMax);
			if (m_limits.zoomMin <= 0 || m_limits.zoomMax <= m_limits.zoomMin) {
				m_limits.zoomMin = canon_xc::Limits().zoomMin;
				m_limits.zoomMax = canon_xc::Limits().zoomMax;
			}
			readState(answer);
		},
		true);
}

void PTZCanonXC::pollPositionSoon()
{
	for (int delay_ms : {400, 1500})
		QTimer::singleShot(delay_ms, this, [this]() {
			if (isConnected())
				pollPosition();
		});
}

void PTZCanonXC::readState(const Items &items)
{
	bool changed = false;
	int v;
	if (canon_xc::readInt(items, "c.1.pan", v))
		changed |= setPosition("pan", canon_xc::signedToApi(v, m_limits.panMin, m_limits.panMax));
	if (canon_xc::readInt(items, "c.1.tilt", v))
		changed |= setPosition("tilt", canon_xc::signedToApi(v, m_limits.tiltMin, m_limits.tiltMax));
	if (canon_xc::readInt(items, "c.1.zoom", v))
		changed |= setPosition("zoom", canon_xc::zoomToApi(v, m_limits));
	auto focus = items.find("c.1.focus");
	if (focus != items.end() && (focus->second == "auto" || focus->second == "manual")) {
		bool af = focus->second == "auto";
		if (!obs_data_has_user_value(state, "focus_af_enabled") ||
		    obs_data_get_bool(state, "focus_af_enabled") != af) {
			obs_data_set_bool(state, "focus_af_enabled", af);
			obs_data_set_bool(stateChanged, "focus_af_enabled", af);
			changed = true;
		}
	}
	if (changed)
		notifyStateChanged();

	/* A recall has arrived when the camera stops moving, once it has
	 * started, or if it never seems to */
	auto status = items.find("p.status");
	if (m_recallWatching && status != items.end()) {
		if (status->second == "1") {
			m_recallSawMoving = true;
		} else if (m_recallSawMoving || m_recallClock.elapsed() > 1500) {
			m_recallWatching = false;
			m_pollTimer.setInterval(kPollIdleMs);
			moveFinished();
		}
	}
}

void PTZCanonXC::readPresets(const Items &items)
{
	if (items.find("p.count") == items.end() && items.find("p.1.content") == items.end())
		return;
	QList<QPair<QString, QString>> presets;
	for (const auto &[slot, name] : canon_xc::readPresets(items, m_limits.presetCount))
		presets.append({QString::number(slot), QString::fromStdString(name)});
	m_presetsKnown = true;
	setCameraPresets(presets);
}

/*
 * Movement
 */

void PTZCanonXC::do_update()
{
	/* Follow the position closely while it moves, and read it once more
	 * as it stops, for where it ended up */
	const bool moving = pan_speed != 0.0 || tilt_speed != 0.0 || zoom_speed != 0.0;
	const int interval = (moving || m_recallWatching) ? kPollMovingMs : kPollIdleMs;
	if (m_pollTimer.interval() != interval) {
		m_pollTimer.setInterval(interval);
		if (!moving)
			pollPositionSoon();
	}
	pump();
}

void PTZCanonXC::pantilt_abs(double pan, double tilt)
{
	enqueue("control.cgi", {{"c.1.pan", num(canon_xc::apiToSigned(pan, m_limits.panMin, m_limits.panMax))},
				{"c.1.tilt", num(canon_xc::apiToSigned(tilt, m_limits.tiltMin, m_limits.tiltMax))}});
	pollPositionSoon();
}

void PTZCanonXC::pantilt_rel(double pan, double tilt)
{
	Params p;
	std::string dp = canon_xc::relativeValue(pan, m_limits.panMin, m_limits.panMax);
	std::string dt = canon_xc::relativeValue(tilt, m_limits.tiltMin, m_limits.tiltMax);
	if (!dp.empty())
		p.push_back({"c.1.pan", dp});
	if (!dt.empty())
		p.push_back({"c.1.tilt", dt});
	if (p.empty())
		return;
	enqueue("control.cgi", p);
	pollPositionSoon();
}

void PTZCanonXC::pantilt_home()
{
	enqueue("control.cgi", {{"c.1.pan", "0"}, {"c.1.tilt", "0"}});
	pollPositionSoon();
}

void PTZCanonXC::zoom_abs(double pos)
{
	enqueue("control.cgi", {{"c.1.zoom", num(canon_xc::apiToZoom(pos, m_limits))}});
	pollPositionSoon();
}

void PTZCanonXC::set_autofocus(bool enabled)
{
	enqueue("control.cgi", {{"c.1.focus", enabled ? "auto" : "manual"}}, [this](bool ok, const Items &items) {
		if (ok)
			readState(items);
	});
}

void PTZCanonXC::focus_onetouch()
{
	enqueue("control.cgi", {{"c.1.focus.action", "one_shot"}});
}

PTZDevice::Features PTZCanonXC::features() const
{
	return PanTilt | Zoom | Focus | PanTiltAbs | PanTiltRel | ZoomAbs | Home | AutoFocus | FocusOneTouch | Presets |
	       TallyLight | MoveCompletion;
}

/*
 * Tally: the camera has one lamp, which can be lit red (program) or green
 * (preview)
 */

void PTZCanonXC::setTally(Tally lamp, bool on)
{
	Q_UNUSED(lamp);
	Q_UNUSED(on);
	/* Program and preview often change together: one request for both */
	m_tallyTimer.start();
}

void PTZCanonXC::applyTally()
{
	if (!tally_auto || effectiveHost().isEmpty())
		return;
	if (m_tallyProgram)
		enqueue("control.cgi", {{"f.tally", "on"}, {"f.tally.mode", "program"}});
	else if (m_tallyPreview)
		enqueue("control.cgi", {{"f.tally", "on"}, {"f.tally.mode", "preview"}});
	else
		enqueue("control.cgi", {{"f.tally", "off"}});
}

/*
 * Presets: the camera has numbered slots (1 to p.count) that it keeps names
 * for and can list. A recall is control.cgi with "p".
 */

PTZDevice::CameraPresets PTZCanonXC::cameraPresets() const
{
	CameraPresets camera;
	camera.available = true;
	camera.namesOnCamera = true;
	camera.enumerable = true;
	return camera;
}

int PTZCanonXC::freeSlot() const
{
	for (int slot = 1; slot <= m_limits.presetCount; slot++) {
		const QString key = QString::number(slot);
		bool taken = presetIndex(presetId(QStringLiteral("camera"), key)) >= 0;
		for (const auto &entry : m_cameraPresets)
			taken |= entry.first == key;
		for (int inFlight : m_creatingSlots)
			taken |= inFlight == slot;
		if (!taken)
			return slot;
	}
	return 0;
}

/* Saves where the camera is to `slot`, and names it `name` unless that is
 * null, which keeps the name it has */
void PTZCanonXC::presetSet(int slot, const QString &name, Done done)
{
	Params p = {{"p", num(slot)}};
	if (!name.isNull())
		p.push_back({"name", str(name.left(kMaxNameLength))});
	if (m_presetSaveAll) {
		p.push_back({"all", "enabled"});
	} else {
		p.push_back({"ptz", "enabled"});
		p.push_back({"focus", "enabled"});
	}
	auto speed = [this](int lo, int hi) {
		return num(std::lround(lo + (hi - lo) * std::clamp(m_presetSpeed, 0.0, 1.0)));
	};
	p.push_back({"pan.speed", speed(m_limits.panSpeedMin, m_limits.panSpeedMax)});
	p.push_back({"tilt.speed", speed(m_limits.tiltSpeedMin, m_limits.tiltSpeedMax)});
	p.push_back({"zoom.speed", speed(m_limits.zoomSpeedMin, m_limits.zoomSpeedMax)});
	enqueue("preset/set", p, std::move(done));
}

bool PTZCanonXC::cameraPresetCreate(const QString &request, const QString &name)
{
	/* Until the camera has said what presets it has, a free slot may not be */
	if (effectiveHost().isEmpty() || !m_haveLimits || !m_presetsKnown)
		return false;
	int slot = freeSlot();
	if (slot <= 0)
		return false;
	m_creatingSlots.insert(request, slot);
	presetSet(slot, name.isNull() ? QString("") : name, [this, request, slot](bool ok, const Items &) {
		if (!m_creatingSlots.contains(request))
			return; /* forgotten when the camera changed */
		m_creatingSlots.remove(request);
		cameraPresetCreated(request, ok ? QString::number(slot) : QString());
		cameraPresetRefresh();
	});
	return true;
}

void PTZCanonXC::cameraPresetSave(const QString &key)
{
	presetSet(key.toInt(), QString(), [this](bool, const Items &) { cameraPresetRefresh(); });
}

void PTZCanonXC::cameraPresetRecall(const QString &key)
{
	enqueue("control.cgi", {{"p", str(key)}}, [this](bool ok, const Items &) {
		if (!ok)
			m_recallWatching = false; /* never went; the device's backstop says so */
	});
	m_recallWatching = true;
	m_recallSawMoving = false;
	m_recallClock.start();
	m_pollTimer.setInterval(kPollMovingMs);
}

void PTZCanonXC::cameraPresetDelete(const QString &key)
{
	enqueue("preset/set", {{"p", str(key)}, {"all", "disabled"}},
		[this](bool, const Items &) { cameraPresetRefresh(); });
}

void PTZCanonXC::cameraPresetRename(const QString &key, const QString &name)
{
	enqueue("preset/set", {{"p", str(key)}, {"name", str(name.left(kMaxNameLength))}},
		[this](bool, const Items &) { cameraPresetRefresh(); });
}

void PTZCanonXC::cameraPresetRefresh()
{
	enqueue("info.cgi", {{"item", "p"}}, [this](bool ok, const Items &items) {
		if (ok)
			readPresets(items);
	});
}

/*
 * Settings
 */

void PTZCanonXC::defaults(obs_data_t *config)
{
	PTZDevice::defaults(config);
	obs_data_set_default_string(config, "host", "");
	obs_data_set_default_int(config, "port", 80);
	obs_data_set_default_string(config, "username", "");
	obs_data_set_default_string(config, "password", "");
	obs_data_set_default_string(config, "speed_mode", "auto1");
	obs_data_set_default_double(config, "screens_per_second", 1.0);
	obs_data_set_default_double(config, "speed_curve", 2.0);
	obs_data_set_default_int(config, "preset_speed", 100);
	obs_data_set_default_bool(config, "preset_save_all", false);
	obs_data_set_default_bool(config, "protocol_trace", false);
}

void PTZCanonXC::update(OBSData config)
{
	const bool wasTallyAuto = tally_auto;
	PTZDevice::update(config);
	/* The lamp may have been driven by something else meanwhile */
	if (tally_auto && !wasTallyAuto)
		m_tallyTimer.start();
	QString oldHost = effectiveHost();
	int oldPort = port;
	QString oldUser = username, oldPassword = password;

	host = QString::fromUtf8(obs_data_get_string(config, "host")).trimmed();
	m_sourceHost = parentSourceHost();
	port = (int)obs_data_get_int(config, "port");
	username = obs_data_get_string(config, "username");
	password = obs_data_get_string(config, "password");
	m_speed.mode = canon_xc::speedModeFromName(obs_data_get_string(config, "speed_mode"));
	m_speed.screensPerSecond = std::clamp(obs_data_get_double(config, "screens_per_second"), 0.01, 10.0);
	m_speed.curve = std::clamp(obs_data_get_double(config, "speed_curve"), 1.0, 4.0);
	m_presetSpeed = std::clamp((int)obs_data_get_int(config, "preset_speed"), 1, 100) / 100.0;
	m_presetSaveAll = obs_data_get_bool(config, "preset_save_all");
	protocol_trace = obs_data_get_bool(config, "protocol_trace");

	/* Only a different camera, or a different way in, is connected again */
	if (effectiveHost() != oldHost || port != oldPort || username != oldUser || password != oldPassword) {
		/* Credentials that changed aren't the ones Qt kept */
		m_network.clearAccessCache();
		connectCamera();
	}
}

void PTZCanonXC::onParentHostChanged(const QString &newHost)
{
	if (newHost == m_sourceHost)
		return;
	m_sourceHost = newHost;
	if (host.isEmpty())
		connectCamera();
}

void PTZCanonXC::save(OBSData config) const
{
	PTZDevice::save(config);
	obs_data_set_string(config, "host", QT_TO_UTF8(host));
	obs_data_set_int(config, "port", port);
	obs_data_set_string(config, "username", QT_TO_UTF8(username));
	obs_data_set_string(config, "password", QT_TO_UTF8(password));
	obs_data_set_string(config, "speed_mode", canon_xc::speedModeName(m_speed.mode));
	obs_data_set_double(config, "screens_per_second", m_speed.screensPerSecond);
	obs_data_set_double(config, "speed_curve", m_speed.curve);
	obs_data_set_int(config, "preset_speed", (int)std::lround(m_presetSpeed * 100));
	obs_data_set_bool(config, "preset_save_all", m_presetSaveAll);
	obs_data_set_bool(config, "protocol_trace", protocol_trace);
}

void PTZCanonXC::saveDefaults(obs_data_t *settings) const
{
	/* Greyed-out text in a blank Host field: where it will connect instead */
	obs_data_set_default_string(settings, "host:placeholder", QT_TO_UTF8(m_sourceHost));
}

obs_properties_t *PTZCanonXC::get_obs_properties()
{
	obs_properties_t *props = PTZDevice::get_obs_properties();
	obs_property_t *p = obs_properties_get(props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, obs_module_text("PTZ.CanonXC.Name"));
	obs_properties_add_text(config, "host", obs_module_text("PTZ.Device.Hostname"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", obs_module_text("PTZ.Device.TCPPort"), 1, 65535, 1);
	obs_properties_add_text(config, "username", obs_module_text("PTZ.Device.Username"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(config, "password", obs_module_text("PTZ.Device.Password"), OBS_TEXT_PASSWORD);
	obs_properties_add_bool(config, "protocol_trace", obs_module_text("PTZ.Device.ProtocolTraceToLog"));

	obs_properties_t *general = obs_property_group_content(obs_properties_get(props, "general"));
	obs_property_t *mode = obs_properties_add_list(general, "speed_mode", obs_module_text("PTZ.CanonXC.SpeedMode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, obs_module_text("PTZ.CanonXC.SpeedMode.Auto1"), "auto1");
	obs_property_list_add_string(mode, obs_module_text("PTZ.CanonXC.SpeedMode.Auto2"), "auto2");
	obs_property_list_add_string(mode, obs_module_text("PTZ.CanonXC.SpeedMode.Manual"), "manual");
	if (m_haveLimits && !m_limits.speedModes)
		obs_property_set_long_description(mode, obs_module_text("PTZ.CanonXC.SpeedMode.Unsupported"));
	obs_properties_add_float_slider(general, "screens_per_second", obs_module_text("PTZ.CanonXC.ScreensPerSecond"),
					0.05, 5.0, 0.05);
	obs_properties_add_float_slider(general, "speed_curve", obs_module_text("PTZ.CanonXC.SpeedCurve"), 1.0, 4.0,
					0.1);
	obs_properties_add_int_slider(general, "preset_speed", obs_module_text("PTZ.CanonXC.PresetSpeed"), 1, 100, 1);
	obs_properties_add_bool(general, "preset_save_all", obs_module_text("PTZ.CanonXC.PresetSaveAll"));
	return props;
}

void ptz_canon_xc_register_filter()
{
	struct obs_source_info info = {};
	info.id = "ca.secretlab.obs-ptz.canon-xc";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = [](void *) -> const char * {
		return "Canon XC PTZ Control";
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return ptz_filter_create([&]() -> PTZDevice * { return new PTZCanonXC(settings, source); });
	};
	info.destroy = ptz_filter_destroy;
	info.get_defaults = [](obs_data_t *settings) {
		ptz_filter_defaults_mark(settings);
		obs_data_set_default_string(settings, "type", "canon-xc");
		PTZCanonXC::defaults(settings);
	};
	info.get_properties = ptz_filter_get_properties;
	info.update = ptz_filter_update;
	info.save = ptz_filter_save;
	info.filter_remove = ptz_filter_remove;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.filter_add = ptz_filter_add;
	obs_register_source(&info);
}
