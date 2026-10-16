/* Pan Tilt Zoom Canon XC protocol driver
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Canon's own control protocol for its remote cameras (CR-N100, CR-N300,
 * CR-N500, CR-N700, CR-X300 and others), over HTTP. Its pan and tilt can be
 * linked to the zoom: in "auto1" a speed is in screens a second, so the
 * picture moves at the same rate however far the camera is zoomed in.
 */
#pragma once

#include <functional>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include "ptz-device.hpp"
#include "canon-xc-protocol.hpp"

class PTZCanonXC : public PTZDevice {
	Q_OBJECT

private:
	QString host; /* configured; empty to follow the source's */
	QString m_sourceHost;
	QString effectiveHost() const { return host.isEmpty() ? m_sourceHost : host; }
	int port = 80;
	QString username;
	QString password;

	canon_xc::Limits m_limits;
	canon_xc::SpeedSettings m_speed;
	/* Speed the camera moves to a preset at, as a fraction of its fastest,
	 * saved with each preset */
	double m_presetSpeed = 1.0;
	/* Whether a saved preset has exposure, white balance and the rest too,
	 * or only where the camera points and its focus */
	bool m_presetSaveAll = false;
	/* Whether the camera has said what it can do since it was connected */
	bool m_haveLimits = false;
	/* The camera's own presets, which come from it */
	bool m_presetsKnown = false;

	/* The protocol can't pipeline, and Canon asks for 30 ms between
	 * requests: one request at a time. A move, which is coalesced to the
	 * latest speeds, goes before anything waiting. */
	using Done = std::function<void(bool ok, const canon_xc::Items &items)>;
	struct Request {
		QString command; /* "control.cgi", "info.cgi", "preset/set" */
		canon_xc::Params params;
		Done done;
		bool poll = false;
	};
	QList<Request> m_queue;
	QNetworkReply *m_inFlight = nullptr;
	Done m_inFlightDone;
	QElapsedTimer m_sinceLast;
	QTimer m_gapTimer;
	QNetworkAccessManager m_network;
	int m_consecutiveFailures = 0;
	static constexpr int kMinGapMs = 30;
	bool m_lastWasMovement = false;
	/* A move the camera refused or never got is sent again, this often */
	int m_moveRetries = 0;
	static constexpr int kMoveRetries = 3;
	/* p.<p>.name.utf8 is at most this long, and a longer one fails the save */
	static constexpr int kMaxNameLength = 64;

	void enqueue(const QString &command, const canon_xc::Params &params, Done done = nullptr, bool poll = false);
	void pump();
	void send(const QString &command, const canon_xc::Params &params, Done done);
	bool sendMovement();
	void requestFinished(QNetworkReply *reply);

	/* Log every request and answer, for working out what a camera does */
	bool protocol_trace = false;

	/* Reading the camera back */
	QTimer m_pollTimer;
	/* A recall being followed until the camera stops, for moveFinished() */
	bool m_recallWatching = false;
	bool m_recallSawMoving = false;
	QElapsedTimer m_recallClock;
	static constexpr int kRecallGiveUpMs = 20000;
	void connectCamera();
	void pollPosition();
	void pollPositionSoon();
	void readState(const canon_xc::Items &items);
	void readPresets(const canon_xc::Items &items);

	/* Tally lamps */
	QTimer m_tallyTimer;
	void applyTally();

	/* Presets: the slots the requests in flight have taken */
	QHash<QString, int> m_creatingSlots;
	int freeSlot() const;
	void presetSet(int slot, const QString &name, Done done);

public:
	PTZCanonXC(OBSData config, obs_source_t *source = nullptr);
	~PTZCanonXC();

	static void defaults(obs_data_t *config);
	void update(OBSData ptz_data) override;
	void onParentHostChanged(const QString &newHost) override;
	void save(OBSData ptz_data) const override;
	void saveDefaults(obs_data_t *settings) const override;
	obs_properties_t *get_obs_properties() override;

	Features features() const override;
	void do_update() override;
	void pantilt_rel(double pan, double tilt) override;
	void pantilt_abs(double pan, double tilt) override;
	void pantilt_home() override;
	void zoom_abs(double pos) override;
	void set_autofocus(bool enabled) override;
	void focus_onetouch() override;
	void setTally(Tally lamp, bool on) override;

	CameraPresets cameraPresets() const override;
	bool cameraPresetCreate(const QString &request, const QString &name) override;
	void cameraPresetSave(const QString &key) override;
	void cameraPresetRecall(const QString &key) override;
	void cameraPresetDelete(const QString &key) override;
	void cameraPresetRename(const QString &key, const QString &name) override;
	void cameraPresetRefresh() override;
};

void ptz_canon_xc_register_filter();
