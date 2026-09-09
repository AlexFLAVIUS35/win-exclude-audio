#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <format>
#include <set>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <processthreadsapi.h>
#include <mmreg.h>
#include <audiopolicy.h>
#include <audioclientactivationparams.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <obs.h>
#include <obs-module.h>
#include <obs-data.h>
#include <obs-properties.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/dstr.h>

#include "wil/result.h"
#include "wil/result_macros.h"

#include "audio-capture.hpp"
#include "audio-capture-helper-manager.hpp"

AudioCaptureHelperManager helper_manager;

static std::wstring Utf8ToLowerWide(const char *utf8)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
	if (n <= 1)
		return {};

	std::wstring wide(static_cast<std::size_t>(n) - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), n);
	CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
	return wide;
}

static bool WildcardMatch(const wchar_t *pattern, const wchar_t *str)
{
	const wchar_t *star = nullptr;
	const wchar_t *star_str = nullptr;

	while (*str) {
		if (*pattern == *str || *pattern == L'?') {
			++pattern;
			++str;
		} else if (*pattern == L'*') {
			star = pattern++;
			star_str = str;
		} else if (star) {
			pattern = star + 1;
			str = ++star_str;
		} else {
			return false;
		}
	}

	while (*pattern == L'*')
		++pattern;

	return *pattern == L'\0';
}

static bool MatchesAnyExecutable(const std::set<std::string> &patterns,
					 const std::string &executable)
{
	auto folded = Utf8ToLowerWide(executable.c_str());

	for (const auto &pattern : patterns) {
		if (WildcardMatch(Utf8ToLowerWide(pattern.c_str()).c_str(), folded.c_str()))
			return true;
	}

	return false;
}

static std::unordered_map<DWORD, DWORD> GetProcessParents()
{
	std::unordered_map<DWORD, DWORD> parent_map;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		warn("CreateToolhelp32Snapshot failed (%lu)", GetLastError());
		return parent_map;
	}

	wil::unique_handle handle{snapshot};
	PROCESSENTRY32W info;
	info.dwSize = sizeof(PROCESSENTRY32W);

	for (bool ret = Process32FirstW(handle.get(), &info); ret;
	     ret = Process32NextW(handle.get(), &info))
		parent_map[info.th32ProcessID] = info.th32ParentProcessID;

	return parent_map;
}

static bool AnyAncestor(const std::unordered_map<DWORD, DWORD> &parents, DWORD pid,
				const std::function<bool(DWORD)> &visit)
{
	std::set<DWORD> seen;
	DWORD current = pid;

	for (int depth = 0; depth < 64; ++depth) {
		auto it = parents.find(current);
		if (it == parents.end())
			return false;

		DWORD parent = it->second;
		if (parent == 0 || parent == current || !seen.insert(parent).second)
			return false;

		if (visit(parent))
			return true;

		current = parent;
	}

	return false;
}

std::set<DWORD> AudioCapture::DeDuplicateCaptureList(
	const std::set<DWORD> &pids, const std::set<DWORD> &exclude_pids)
{
	auto parents = GetProcessParents();
	std::set<DWORD> candidates = pids;

	for (auto excluded : exclude_pids) {
		AnyAncestor(parents, excluded, [&](DWORD ancestor) {
			candidates.erase(ancestor);
			return false;
		});
	}

	std::set<DWORD> roots;
	for (auto pid : candidates) {
		bool covered = AnyAncestor(parents, pid, [&](DWORD ancestor) {
			return candidates.contains(ancestor);
		});

		if (!covered)
			roots.insert(pid);
	}

	if (roots.empty() && !candidates.empty())
		return candidates;

	return roots;
}

void AudioCapture::StartCapture(const std::set<DWORD> &new_pids, bool exclude)
{
	if (exclude != capture_exclude)
		StopCapture();

	for (auto pid : pids) {
		if (new_pids.contains(pid))
			continue;

		helper_manager.UnRegisterMixer(pid, capture_exclude, &mixer.value());
	}

	for (auto new_pid : new_pids) {
		if (pids.contains(new_pid))
			continue;

		helper_manager.RegisterMixer(new_pid, exclude, &mixer.value());
	}

	auto lock = pids_section.lock();
	pids = new_pids;
	capture_exclude = exclude;
}

void AudioCapture::StopCapture()
{
	for (auto pid : pids)
		helper_manager.UnRegisterMixer(pid, capture_exclude, &mixer.value());

	auto lock = pids_section.lock();
	pids.clear();
	capture_exclude = false;
}

std::set<DWORD> AudioCapture::GetCapturedPids()
{
	auto lock = pids_section.lock();
	return pids;
}

bool AudioCapture::IsExcludeCapture()
{
	auto lock = pids_section.lock();
	return capture_exclude;
}

void AudioCapture::WorkerUpdate()
{
	auto config_lock = config_section.lock();
	auto config = this->config;
	config_lock.reset();

	auto *monitor = SessionMonitor::Instance();
	if (!monitor) {
		StopCapture();
		return;
	}

	auto sessions = monitor->GetSessions();
	std::set<DWORD> capture_pids;
	std::set<DWORD> exclude_pids;

	for (auto &[key, executable] : sessions) {
		if (key.pid == GetCurrentProcessId())
			continue;

		if (MatchesAnyExecutable(config.executables, executable))
			exclude_pids.insert(key.pid);
		else
			capture_pids.insert(key.pid);
	}

	if (!exclude_pids.empty()) {
		auto excluded_roots = AudioCapture::DeDuplicateCaptureList(exclude_pids);

		if (excluded_roots.size() == 1) {
			StartCapture(excluded_roots, true);
			return;
		}
	} else {
		StartCapture({GetCurrentProcessId()}, true);
		return;
	}

	if (capture_pids.empty()) {
		StopCapture();
		return;
	}

	StartCapture(AudioCapture::DeDuplicateCaptureList(capture_pids, exclude_pids), false);
}

bool AudioCapture::Tick(const MSG &msg)
{
	bool shutdown = false;

	switch (msg.message) {
	case CaptureEvents::Shutdown:
		debug("shutting down");
		shutdown = true;
		break;

	case CaptureEvents::Update:
	case CaptureEvents::SessionAdded:
	case CaptureEvents::SessionExpired:
		WorkerUpdate();
		break;

	default:
		warn("unexpected event id, ignoring");
		break;
	}

	return shutdown;
}

void AudioCapture::Run()
{
	MSG msg;
	PeekMessageA(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

	worker_ready.SetEvent();
	PostThreadMessageA(GetCurrentThreadId(), CaptureEvents::Update, NULL, NULL);

	bool shutdown = false;
	while (!shutdown) {
		auto ret = GetMessage(&msg, reinterpret_cast<HWND>(-1), 0, 0);
		if (ret == 0 || ret == -1) {
			debug("shutting down");
			break;
		}

		try {
			shutdown = Tick(msg);
		} catch (const wil::ResultException &e) {
			error("failed to process event %u: %s", msg.message, e.what());
		} catch (const std::exception &e) {
			error("failed to process event %u: %s", msg.message, e.what());
		}
	}

	StopCapture();
}

void AudioCapture::Update(obs_data_t *settings)
{
	AudioCaptureConfig new_config;
	new_config.executables = GetExecutables(settings);

	auto lock = config_section.lock();
	config = std::move(new_config);
	lock.reset();

	PostThreadMessageA(worker_tid, CaptureEvents::Update, NULL, NULL);
}

static void audio_capture_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	ctx->Update(settings);
}

AudioCapture::AudioCapture(obs_data_t *settings, obs_source_t *source) : source{source}
{
	mixer.emplace(source, helper_manager.GetFormat());

	worker_thread = std::thread(&AudioCapture::Run, this);
	worker_tid = GetThreadId(worker_thread.native_handle());
	worker_ready.wait();

	if (auto *monitor = SessionMonitor::Instance())
		monitor->RegisterEvent(worker_tid, CaptureEvents::SessionAdded,
				       CaptureEvents::SessionExpired);

	Update(settings);
}

static void *audio_capture_create(obs_data_t *settings, obs_source_t *source)
{
	try {
		return new AudioCapture(settings, source);
	} catch (const wil::ResultException &e) {
		error("failed to create context: %s", e.what());
	} catch (const std::exception &e) {
		error("failed to create context: %s", e.what());
	}

	return nullptr;
}

AudioCapture::~AudioCapture()
{
	if (auto *monitor = SessionMonitor::Instance())
		monitor->UnRegisterEvent(worker_tid);

	if (!worker_thread.joinable())
		return;

	worker_ready.wait();
	PostThreadMessageA(worker_tid, CaptureEvents::Shutdown, NULL, NULL);
	worker_thread.join();
}

static void audio_capture_destroy(void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	delete ctx;
}

std::tuple<std::string, std::string> AudioCapture::MakeSessionOptionStrings(
	std::set<DWORD> pids, const std::string &executable, bool added)
{
	std::string pids_string;

	if (!pids.empty()) {
		auto it = std::begin(pids);
		pids_string.append(std::format("{}", *it));
		++it;
		for (auto end = std::end(pids); it != end; ++it)
			pids_string.append(std::format(", {}", *it));
	} else {
		pids_string = "*";
	}

	if (!added)
		return {std::format("[{}] {}", pids_string, executable), executable};

	static int id = 0;
	return {std::format("[{}] {} (added)", pids_string, executable), std::format("{}", id++)};
}

static bool executable_list_callback(void *data, obs_properties_t *ps, obs_property_t *,
				     obs_data_t *)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	if (!ctx)
		return true;

	auto *active_session_list = obs_properties_get(ps, SETTING_ACTIVE_SESSION_LIST);
	auto *active_session_add = obs_properties_get(ps, SETTING_ACTIVE_SESSION_ADD);

	obs_property_list_clear(active_session_list);
	ctx->FillActiveSessionList(active_session_list, active_session_add);
	ctx->UpdateStatus(ps);
	return true;
}

static bool session_refresh_callback(obs_properties_t *ps, obs_property_t *, void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	if (!ctx)
		return true;

	auto *active_session_list = obs_properties_get(ps, SETTING_ACTIVE_SESSION_LIST);
	auto *active_session_add = obs_properties_get(ps, SETTING_ACTIVE_SESSION_ADD);

	obs_property_list_clear(active_session_list);
	ctx->FillActiveSessionList(active_session_list, active_session_add);
	ctx->UpdateStatus(ps);
	return true;
}

static bool session_add_callback(obs_properties_t *ps, obs_property_t *, void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	if (!ctx)
		return false;

	auto *source = ctx->GetSource();
	auto *settings = obs_source_get_settings(source);
	auto addable = ctx->GetAddableExecutables(settings);
	std::string executable = obs_data_get_string(settings, SETTING_ACTIVE_SESSION_LIST);

	if (std::find(addable.begin(), addable.end(), executable) == addable.end())
		executable = addable.empty() ? std::string() : addable.front();

	if (!executable.empty()) {
		auto *executable_list_array = obs_data_get_array(settings, SETTING_EXECUTABLE_LIST);

		if (obs_data_array_count(executable_list_array) == 0) {
			obs_data_array_release(executable_list_array);
			executable_list_array = obs_data_array_create();
			obs_data_set_array(settings, SETTING_EXECUTABLE_LIST, executable_list_array);
		}

		auto *executable_obj = obs_data_create();
		obs_data_set_bool(executable_obj, "hidden", false);
		obs_data_set_bool(executable_obj, "selected", false);
		obs_data_set_string(executable_obj, "value", executable.c_str());
		obs_data_array_push_back(executable_list_array, executable_obj);

		obs_data_release(executable_obj);
		obs_data_array_release(executable_list_array);
		obs_source_update(source, nullptr);
	}

	auto *active_session_list = obs_properties_get(ps, SETTING_ACTIVE_SESSION_LIST);
	auto *active_session_add = obs_properties_get(ps, SETTING_ACTIVE_SESSION_ADD);
	obs_property_list_clear(active_session_list);
	ctx->FillActiveSessionList(active_session_list, active_session_add);
	ctx->UpdateStatus(ps);

	obs_data_release(settings);
	return true;
}

std::set<std::string> AudioCapture::GetExecutables(obs_data_t *settings)
{
	auto *executable_list_array = obs_data_get_array(settings, SETTING_EXECUTABLE_LIST);
	auto count = obs_data_array_count(executable_list_array);
	std::set<std::string> executables;

	for (std::size_t i = 0; i < count; ++i) {
		auto *item = obs_data_array_item(executable_list_array, i);
		executables.insert(std::string(obs_data_get_string(item, "value")));
	}

	obs_data_array_release(executable_list_array);
	return executables;
}

std::vector<std::string> AudioCapture::GetAddableExecutables(obs_data_t *settings)
{
	auto *monitor = SessionMonitor::Instance();
	auto sessions = monitor ? monitor->GetSessions()
				: std::unordered_map<SessionKey, std::string>{};
	auto patterns = GetExecutables(settings);

	std::set<std::string> unique;
	for (auto &[key, executable] : sessions) {
		if (!MatchesAnyExecutable(patterns, executable))
			unique.insert(executable);
	}

	std::vector<std::string> addable(unique.begin(), unique.end());
	std::sort(addable.begin(), addable.end(), [](const std::string &a, const std::string &b) {
		return astrcmpi(a.c_str(), b.c_str()) < 0;
	});
	return addable;
}

void AudioCapture::FillActiveSessionList(obs_property_t *session_list, obs_property_t *session_add)
{
	auto *settings = obs_source_get_settings(GetSource());
	auto *monitor = SessionMonitor::Instance();
	auto sessions = monitor ? monitor->GetSessions()
				: std::unordered_map<SessionKey, std::string>{};
	auto executables = GetExecutables(settings);

	std::unordered_map<std::string, std::set<DWORD>> session_options;
	for (auto &[key, executable] : sessions)
		session_options[executable].insert(key.pid);

	std::vector<std::tuple<std::string, std::set<DWORD>>> enabled;
	std::vector<std::tuple<std::string, std::set<DWORD>>> disabled;

	for (auto &[executable, pids] : session_options) {
		if (MatchesAnyExecutable(executables, executable))
			disabled.push_back({executable, pids});
		else
			enabled.push_back({executable, pids});
	}

	auto cmp = [](auto a, auto b) {
		return astrcmpi(std::get<0>(a).c_str(), std::get<0>(b).c_str()) < 0;
	};
	std::sort(enabled.begin(), enabled.end(), cmp);
	std::sort(disabled.begin(), disabled.end(), cmp);

	for (auto &[executable, pids] : enabled) {
		auto [name, val] = MakeSessionOptionStrings(pids, executable);
		obs_property_list_add_string(session_list, name.c_str(), val.c_str());
	}

	for (auto &[executable, pids] : disabled) {
		auto [name, val] = MakeSessionOptionStrings(pids, executable, true);
		auto idx = obs_property_list_add_string(session_list, name.c_str(), val.c_str());
		obs_property_list_item_disable(session_list, idx, true);
	}

	obs_property_set_enabled(session_add, !enabled.empty());
	obs_property_set_enabled(session_list, !enabled.empty());
	obs_data_release(settings);
}

void AudioCapture::AppendUnmatchedPatterns(
	std::string &text, const std::unordered_map<SessionKey, std::string> &sessions)
{
	auto *settings = obs_source_get_settings(GetSource());
	auto patterns = GetExecutables(settings);
	obs_data_release(settings);

	std::string unmatched;
	for (const auto &pattern : patterns) {
		auto folded_pattern = Utf8ToLowerWide(pattern.c_str());
		bool hit = false;

		for (auto &[key, executable] : sessions) {
			if (WildcardMatch(folded_pattern.c_str(),
					  Utf8ToLowerWide(executable.c_str()).c_str())) {
				hit = true;
				break;
			}
		}

		if (!hit) {
			unmatched += unmatched.empty() ? "" : ", ";
			unmatched += pattern;
		}
	}

	if (!unmatched.empty())
		text += std::format("\n{} {}", TEXT_STATUS_NO_MATCH, unmatched);
}

void AudioCapture::UpdateStatus(obs_properties_t *ps)
{
	auto *status = obs_properties_get(ps, SETTING_STATUS);
	if (!status)
		return;

	auto *monitor = SessionMonitor::Instance();
	auto sessions = monitor ? monitor->GetSessions()
				: std::unordered_map<SessionKey, std::string>{};
	auto captured = GetCapturedPids();

	if (captured.empty()) {
		std::string text = TEXT_STATUS_NONE;
		AppendUnmatchedPatterns(text, sessions);
		obs_property_set_description(status, text.c_str());
		return;
	}

	std::set<std::string> names;
	for (auto &[key, executable] : sessions) {
		if (captured.contains(key.pid))
			names.insert(executable);
	}

	if (names.empty()) {
		for (auto pid : captured) {
			wil::unique_process_handle process{
				OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid)};
			if (!process)
				continue;

			wchar_t path[MAX_PATH] = {L'\0'};
			if (!GetProcessImageFileNameW(process.get(), path, MAX_PATH))
				continue;

			char utf8[MAX_PATH * 4] = {'\0'};
			os_wcs_to_utf8(path, 0, utf8, sizeof(utf8));
			std::string name{utf8};
			auto slash = name.find_last_of('\\');
			if (slash != std::string::npos)
				name = name.substr(slash + 1);
			if (!name.empty())
				names.insert(name);
		}
	}

	std::string text = IsExcludeCapture() ? TEXT_STATUS_EXCLUDING : TEXT_STATUS_CAPTURING;
	if (names.empty()) {
		text += std::format(" {} pid(s)", captured.size());
	} else {
		bool first = true;
		for (auto &name : names) {
			text += first ? " " : ", ";
			text += name;
			first = false;
		}
	}

	AppendUnmatchedPatterns(text, sessions);
	obs_property_set_description(status, text.c_str());
}

static obs_properties_t *audio_capture_properties(void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	obs_properties_t *ps = obs_properties_create();

	obs_properties_add_text(ps, SETTING_STATUS, TEXT_STATUS_NONE, OBS_TEXT_INFO);

	auto *executable_list = obs_properties_add_editable_list(
		ps, SETTING_EXECUTABLE_LIST, TEXT_EXECUTABLE_LIST,
		OBS_EDITABLE_LIST_TYPE_STRINGS, NULL, NULL);
	obs_property_set_modified_callback2(executable_list, executable_list_callback, ctx);

	obs_properties_t *active_session_group = obs_properties_create();
	auto *active_session_list = obs_properties_add_list(
		active_session_group, SETTING_ACTIVE_SESSION_LIST, TEXT_ACTIVE_SESSION_LIST,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	auto *active_session_add = obs_properties_add_button2(
		active_session_group, SETTING_ACTIVE_SESSION_ADD, TEXT_ACTIVE_SESSION_ADD,
		session_add_callback, ctx);

	obs_properties_add_button2(active_session_group, SETTING_ACTIVE_SESSION_REFRESH,
				   TEXT_ACTIVE_SESSION_REFRESH, session_refresh_callback, ctx);

	if (ctx)
		ctx->FillActiveSessionList(active_session_list, active_session_add);

	obs_properties_add_group(ps, SETTING_ACTIVE_SESSION_GROUP, TEXT_ACTIVE_SESSION_GROUP,
				 OBS_GROUP_NORMAL, active_session_group);

	if (ctx)
		ctx->UpdateStatus(ps);

	return ps;
}

static void audio_capture_defaults(obs_data_t *settings)
{
	auto *executable_list = obs_data_array_create();
	obs_data_set_default_array(settings, SETTING_EXECUTABLE_LIST, executable_list);
	obs_data_array_release(executable_list);
}

static const char *audio_capture_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return TEXT_NAME;
}

struct obs_source_info audio_capture_info = {
	.id = "audio_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_DO_NOT_SELF_MONITOR,
	.get_name = audio_capture_get_name,
	.create = audio_capture_create,
	.destroy = audio_capture_destroy,
	.get_defaults = audio_capture_defaults,
	.get_properties = audio_capture_properties,
	.update = audio_capture_update,
	.icon_type = OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT,
};