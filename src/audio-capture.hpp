#pragma once

#include <cstdio>
#include <optional>
#include <tuple>
#include <set>
#include <vector>

#include <windows.h>
#include <mmreg.h>
#include <wil/resource.h>

#include <obs.h>
#include <util/darray.h>

#include "common.hpp"
#include "audio-capture-helper.hpp"
#include "session-monitor.hpp"

/* clang-format off */

#define SETTING_EXECUTABLE_LIST        "executable_list"

#define SETTING_ACTIVE_SESSION_GROUP   "active_session_group"
#define SETTING_ACTIVE_SESSION_LIST    "active_session_list"
#define SETTING_ACTIVE_SESSION_ADD     "active_session_add"
#define SETTING_ACTIVE_SESSION_REFRESH "active_session_refresh"

#define SETTING_LATENCY                "latency"
#define SETTING_STATUS                 "status"

#define TEXT_NAME                      obs_module_text("Name")

#define TEXT_EXECUTABLE_LIST           obs_module_text("ExecutableList")

#define TEXT_ACTIVE_SESSION_GROUP      obs_module_text("ActiveSession.Group")
#define TEXT_ACTIVE_SESSION_LIST       obs_module_text("ActiveSession.List")
#define TEXT_ACTIVE_SESSION_ADD        obs_module_text("ActiveSession.Add")
#define TEXT_ACTIVE_SESSION_REFRESH    obs_module_text("ActiveSession.Refresh")

#define TEXT_LATENCY                   obs_module_text("Latency")
#define TEXT_LATENCY_NORMAL            obs_module_text("Latency.Normal")
#define TEXT_LATENCY_LOW               obs_module_text("Latency.Low")
#define TEXT_STATUS_CAPTURING          obs_module_text("Status.Capturing")
#define TEXT_STATUS_EXCLUDING         obs_module_text("Status.Excluding")
#define TEXT_STATUS_NONE               obs_module_text("Status.None")
#define TEXT_STATUS_NO_MATCH           obs_module_text("Status.NoMatch")

/* clang-format on */

namespace CaptureEvents {
enum CaptureEvents { Shutdown = WM_USER, Update, SessionAdded, SessionExpired };
}

struct AudioCaptureConfig {
	std::set<std::string> executables;
};

class AudioCapture {
private:
	std::thread worker_thread;
	DWORD worker_tid;
	wil::unique_event worker_ready{wil::EventOptions::ManualReset};

	wil::critical_section config_section;
	AudioCaptureConfig config;

	obs_source_t *source;

	std::optional<Mixer> mixer;

	// Owned by the worker thread; pids_section guards the writes so the UI
	// thread can snapshot it for the status line.
	wil::critical_section pids_section;
	std::set<DWORD> pids;

	// Always true for this plugin: `pids` holds the process tree being
	// excluded and the helper captures everything else.
	bool capture_exclude = false;

	void StartCapture(const std::set<DWORD> &new_pids, bool exclude);
	void StopCapture();

	void WorkerUpdate();

	bool Tick(const MSG &msg);
	void Run();

public:
	obs_source_t *GetSource() { return source; }

	static std::set<DWORD> DeDuplicateCaptureList(const std::set<DWORD> &pids,
						      const std::set<DWORD> &exclude = {});

	void Update(obs_data_t *settings);

	std::tuple<std::string, std::string>
	MakeSessionOptionStrings(std::set<DWORD> pids, const std::string &executable, bool added);

	void FillActiveSessionList(obs_property_t *session_list, obs_property_t *session_add);
	void AppendUnmatchedPatterns(std::string &text,
				     const std::unordered_map<SessionKey, std::string> &sessions);
	void UpdateStatus(obs_properties_t *ps);
	std::set<DWORD> GetCapturedPids();
	bool IsExcludeCapture();
	std::set<std::string> GetExecutables(obs_data_t *settings);
	std::vector<std::string> GetAddableExecutables(obs_data_t *settings);

	AudioCapture(obs_data_t *settings, obs_source_t *source);
	~AudioCapture();
};
