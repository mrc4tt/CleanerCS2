/**
 * =============================================================================
 * CleanerCS2
 * Copyright (C) 2024-2026 Poggu
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include "cleanercs2.h"
#include <engine2/ivengineserver2.h>
#include "khook.hpp"
#include "utils/module.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <atomic>
#include <climits>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif
#include <nlohmann/json.hpp>

// bruh
#undef POSIX
#include <re2/re2.h>

#ifdef _WIN32
#define ROOTBIN "/bin/win64/"
#define GAMEBIN "/csgo/bin/win64/"
#else
#define ROOTBIN "/bin/linuxsteamrt64/"
#define GAMEBIN "/csgo/bin/linuxsteamrt64/"
#endif

CleanerPlugin g_CleanerPlugin;
IVEngineServer2 *engine = NULL;
ICvar *icvar = NULL;

typedef int (*LogDirect_t)(void* loggingSystem, int channel, int severity, LoggingRareOptions_t*, char const*, va_list*);

struct ChannelRule
{
	std::string name;
	LoggingChannelID_t id = -1;
	int maxSeverity; // drop messages with severity <= maxSeverity
};

std::vector<re2::RE2*> g_RegexList;
std::vector<re2::RE2*> g_BlockList;
std::vector<re2::RE2*> g_StderrList;
std::vector<ChannelRule> g_ChannelList;
std::shared_mutex g_RegexMutex;
thread_local bool g_BypassFilter = false;
std::atomic<bool> g_Debug = false;

// State of an active @block. Multi-line dumps such as KeyValues arrive as one LogDirect call per fragment,
// so after the start line matches we drop every fragment on the same channel/severity until the braces balance.
struct BlockState
{
	bool active = false;
	bool opened = false;
	int depth = 0;
	int count = 0;
	LoggingChannelID_t channel;
	int severity;
};

std::mutex g_BlockMutex;
std::atomic<bool> g_BlockActive = false;
BlockState g_Block;

#define DEBUG_PREFIX "[cc2:"

static const char* SeverityName(int severity)
{
	switch (severity)
	{
		case 1: return "detailed";
		case 2: return "message";
		case 3: return "warning";
		case 4: return "assert";
		case 5: return "error";
		default: return "?";
	}
}

static LoggingChannelID_t FindChannelByName(const std::string& name)
{
	if (!V_stricmp(name.c_str(), "General"))
		return LOG_GENERAL;

	return LoggingSystem_FindChannel(name.c_str());
}

// Channels are registered by the modules that own them, some after the plugin has loaded (e.g. SteamNetSockets),
// so names that are not found yet are retried on every level init and config reload.
void ResolveChannels()
{
	std::unique_lock<std::shared_mutex> lock(g_RegexMutex);

	for (auto& rule : g_ChannelList)
	{
		if (rule.id == -1)
			rule.id = FindChannelByName(rule.name);
	}
}

static bool ConsumeBlock(int channel, int severity, const char* text)
{
	std::lock_guard<std::mutex> lock(g_BlockMutex);

	if (!g_Block.active || g_Block.channel != channel || g_Block.severity != severity)
		return false;

	for (const char* c = text; *c; c++)
	{
		if (*c == '{')
		{
			g_Block.depth++;
			g_Block.opened = true;
		}
		else if (*c == '}')
		{
			g_Block.depth--;
		}
	}

	g_Block.count++;

	// End when the outermost brace closes, or bail out if the dump never opens / never closes
	if ((g_Block.opened && g_Block.depth <= 0) || (!g_Block.opened && g_Block.count > 16) || g_Block.count > 4096)
	{
		g_Block.active = false;
		g_BlockActive = false;
	}

	return true;
}

static void StartBlock(int channel, int severity)
{
	std::lock_guard<std::mutex> lock(g_BlockMutex);

	g_Block = BlockState();
	g_Block.active = true;
	g_Block.channel = channel;
	g_Block.severity = severity;
	g_BlockActive = true;
}

KHook::Return<int> Detour_LogDirect(void* loggingSystem, int channel, int severity, LoggingRareOptions_t* rareOptions, char const* str, va_list* args)
{
	if (g_BypassFilter)
		return {KHook::Action::Ignore};

	{
		std::shared_lock<std::shared_mutex> lock(g_RegexMutex);
		for (auto& rule : g_ChannelList)
		{
			if (rule.id == channel && severity <= rule.maxSeverity)
				return {KHook::Action::Supersede, 0};
		}
	}

	char buffer[MAX_LOGGING_MESSAGE_LENGTH];

	if (args)
	{
		va_list args2;
		va_copy(args2, *args);
		V_vsnprintf(buffer, sizeof buffer, str, args2);
		va_end(args2);
	}

	const char* text = args ? buffer : str;

	if (g_BlockActive && ConsumeBlock(channel, severity, text))
		return {KHook::Action::Supersede, 0};

	{
		std::shared_lock<std::shared_mutex> lock(g_RegexMutex);
		for (auto& regex : g_BlockList)
		{
			if (RE2::FullMatch(text, *regex))
			{
				StartBlock(channel, severity);
				return {KHook::Action::Supersede, 0};
			}
		}

		for (auto& regex : g_RegexList)
		{
			if (RE2::FullMatch(text, *regex))
				return {KHook::Action::Supersede, 0};
		}
	}

	// Re-log with a [cc2:channel|severity] tag. Messages that already carry the tag pass through, since LogDirect
	// re-enters itself with the same arguments when called off the main thread.
	if (g_Debug && strncmp(text, DEBUG_PREFIX, sizeof(DEBUG_PREFIX) - 1))
	{
		const CLoggingSystem::LoggingChannel_t* info = LoggingSystem_GetChannel(channel);

		char tagged[MAX_LOGGING_MESSAGE_LENGTH];
		V_snprintf(tagged, sizeof tagged, DEBUG_PREFIX "%s|%s] %s", info ? info->m_Name : "?", SeverityName(severity), text);

		g_BypassFilter = true;
		LoggingSystem_LogDirect(channel, (LoggingSeverity_t)severity, tagged);
		g_BypassFilter = false;

		return {KHook::Action::Supersede, 0};
	}

	return {KHook::Action::Ignore};
}

KHook::Function<int, void*, int, int, LoggingRareOptions_t*, char const*, va_list*> logDirectHook(Detour_LogDirect, nullptr);

#ifdef WIN32
#define GAMEDATA_PLATFORM "windows"
#else
#define GAMEDATA_PLATFORM "linux"
#endif

std::filesystem::path GetAddonPath(const char* file)
{
	CBufferStringGrowable<MAX_PATH> gameDir;
	engine->GetGameDir(gameDir);

	std::filesystem::path path = gameDir.Get();
	path /= "addons/cleanercs2";
	path /= file;
	return path;
}

// Reads signatures.<platform> and signatures.library for an entry in gamedata.json.
// Returns false if the file or entry is missing, so the caller can fall back to the built-in signature.
bool GetGamedataSignature(const char* name, std::string& sig, std::string& library)
{
	std::filesystem::path path = GetAddonPath("gamedata.json");
	std::ifstream file(path);

	if (!file.is_open())
	{
		META_CONPRINTF("[CleanerCS2] %s not found, using built-in signatures\n", path.string().c_str());
		return false;
	}

	nlohmann::json gamedata = nlohmann::json::parse(file, nullptr, false);

	if (gamedata.is_discarded())
	{
		META_CONPRINTF("[CleanerCS2] Failed to parse %s, using built-in signatures\n", path.string().c_str());
		return false;
	}

	auto entry = gamedata.find(name);
	if (entry == gamedata.end() || !entry->contains("signatures"))
		return false;

	const auto& sigs = (*entry)["signatures"];
	if (!sigs.contains(GAMEDATA_PLATFORM) || !sigs[GAMEDATA_PLATFORM].is_string())
		return false;

	sig = sigs[GAMEDATA_PLATFORM].get<std::string>();
	library = sigs.value("library", library);
	return true;
}

bool SetupHook()
{
#ifdef WIN32
	std::string sig = "4C 89 4C 24 ? 44 89 44 24 ? 89 54 24 ? 55";
#else
	std::string sig = "55 89 D0 49 89 FA 89 F7 48 89 E5";
#endif
	std::string library = "tier0";

	if (GetGamedataSignature("LogDirect", sig, library))
		META_CONPRINTF("[CleanerCS2] Using LogDirect signature from gamedata.json\n");

	CModule module(library == "server" ? GAMEBIN : ROOTBIN, library.c_str());

	LogDirect_t pLogDirect = reinterpret_cast<LogDirect_t>(KHook::LookupSignature(module.m_base, module.m_size, sig.c_str()));

	if (!pLogDirect)
	{
		META_CONPRINTF("[CleanerCS2] Failed to find LogDirect signature in %s: %s\n", library.c_str(), sig.c_str());
		return false;
	}

	logDirectHook.Configure(pLogDirect);

	return true;
}

#ifndef _WIN32
// The Steam API writes some messages ("dlopen failed trying to load", "[S_API FAIL]") straight to stderr,
// bypassing tier0. To filter those, fd 2 is redirected into a pipe and a reader thread forwards every
// line that does not match an @stderr rule to the original stderr.
int g_OrigStderr = -1;
int g_StderrStop[2] = {-1, -1};
std::thread g_StderrThread;

static bool StderrLineFiltered(const std::string& line)
{
	std::shared_lock<std::shared_mutex> lock(g_RegexMutex);
	for (auto& regex : g_StderrList)
	{
		if (RE2::FullMatch(line, *regex))
			return true;
	}
	return false;
}

static void StderrForward(const std::string& line)
{
	size_t written = 0;
	while (written < line.size())
	{
		ssize_t n = write(g_OrigStderr, line.data() + written, line.size() - written);
		if (n <= 0)
			return;
		written += n;
	}
}

static void StderrReader(int readFd)
{
	std::string pending;
	char chunk[4096];
	bool stopping = false;

	for (;;)
	{
		if (!stopping)
		{
			pollfd fds[2] = {{readFd, POLLIN, 0}, {g_StderrStop[0], POLLIN, 0}};
			if (poll(fds, 2, -1) < 0)
				continue;

			// On unload, drain whatever is left without blocking, since a child process may still hold the write end
			if (fds[1].revents)
			{
				stopping = true;
				fcntl(readFd, F_SETFL, fcntl(readFd, F_GETFL) | O_NONBLOCK);
			}
		}

		ssize_t n = read(readFd, chunk, sizeof chunk);
		if (n <= 0)
		{
			if (n < 0 && !stopping)
				continue;
			break;
		}

		pending.append(chunk, n);

		size_t newline;
		while ((newline = pending.find('\n')) != std::string::npos)
		{
			std::string line = pending.substr(0, newline);
			if (!StderrLineFiltered(line))
				StderrForward(pending.substr(0, newline + 1));
			pending.erase(0, newline + 1);
		}

		// Don't hold back very long unterminated output forever
		if (pending.size() > sizeof chunk)
		{
			StderrForward(pending);
			pending.clear();
		}
	}

	if (!pending.empty() && !StderrLineFiltered(pending))
		StderrForward(pending);

	close(readFd);
}

static void InstallStderrFilter()
{
	if (g_OrigStderr != -1)
		return;

	int fds[2];
	if (pipe(fds) != 0 || pipe(g_StderrStop) != 0)
	{
		META_CONPRINTF("[CleanerCS2] Failed to create stderr pipe\n");
		return;
	}

	// Keep the pipe out of child processes
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(g_StderrStop[0], F_SETFD, FD_CLOEXEC);
	fcntl(g_StderrStop[1], F_SETFD, FD_CLOEXEC);

	fflush(stderr);
	g_OrigStderr = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
	dup2(fds[1], STDERR_FILENO);
	close(fds[1]);

	g_StderrThread = std::thread(StderrReader, fds[0]);
}

static void RemoveStderrFilter()
{
	if (g_OrigStderr == -1)
		return;

	fflush(stderr);
	dup2(g_OrigStderr, STDERR_FILENO);

	write(g_StderrStop[1], "x", 1);
	g_StderrThread.join();

	close(g_StderrStop[0]);
	close(g_StderrStop[1]);
	close(g_OrigStderr);
	g_OrigStderr = -1;
}
#endif

static void DeleteRegexes(std::vector<re2::RE2*>& list)
{
	for (auto& regex : list)
		delete regex;

	list.clear();
}

static re2::RE2* CompileRegex(const std::string& pattern)
{
	RE2::Options options;
	options.set_dot_nl(true);

	re2::RE2* re = new RE2(pattern, options);

	if (re->ok())
		return re;

	META_CONPRINTF("[CleanerCS2] Failed to parse regex: '%s': %s\n", pattern.c_str(), re->error().c_str());
	delete re;
	return nullptr;
}

// Parses "@channel:Name" (drop everything) or "@channel[N]:Name" (drop severity <= N, 1=detailed 2=message 3=warning)
static bool ParseChannelRule(const std::string& line, ChannelRule& rule)
{
	std::string rest = line.substr(sizeof("@channel") - 1);
	rule.maxSeverity = INT_MAX;

	if (!rest.empty() && rest[0] == '[')
	{
		size_t close = rest.find(']');
		if (close == std::string::npos)
			return false;

		rule.maxSeverity = atoi(rest.substr(1, close - 1).c_str());
		rest = rest.substr(close + 1);
	}

	if (rest.size() < 2 || rest[0] != ':')
		return false;

	rule.name = rest.substr(1);
	return true;
}

static void Trim(std::string& line)
{
	const char* ws = " \t\r\n";
	line.erase(line.find_last_not_of(ws) + 1);
	line.erase(0, line.find_first_not_of(ws));
}

void LoadConfig()
{
	std::vector<re2::RE2*> regexes;
	std::vector<re2::RE2*> blocks;
	std::vector<re2::RE2*> stderrs;
	std::vector<ChannelRule> channels;

	std::filesystem::path cfgPath = GetAddonPath("config.cfg");

	if (!std::filesystem::exists(cfgPath))
	{
		std::ofstream cfgFile(cfgPath);
		cfgFile.close();
	}

	std::ifstream cfgFile(cfgPath);

	if (cfgFile.is_open())
	{
		std::string line;
		while (std::getline(cfgFile, line))
		{
			Trim(line);

			if (line.empty() || line.starts_with("//"))
				continue;

			if (line.starts_with("@channel"))
			{
				ChannelRule rule;
				if (ParseChannelRule(line, rule))
					channels.push_back(rule);
				else
					META_CONPRINTF("[CleanerCS2] Invalid channel rule: '%s'\n", line.c_str());
			}
			else if (line.starts_with("@stderr:"))
			{
				if (re2::RE2* re = CompileRegex(line.substr(sizeof("@stderr:") - 1)))
					stderrs.push_back(re);
			}
			else if (line.starts_with("@block:"))
			{
				if (re2::RE2* re = CompileRegex(line.substr(sizeof("@block:") - 1)))
					blocks.push_back(re);
			}
			else if (re2::RE2* re = CompileRegex(line))
			{
				regexes.push_back(re);
			}
		}
		cfgFile.close();
	}
	else
	{
		META_CONPRINTF("[CleanerCS2] Failed to open config file\n");
	}

	{
		std::unique_lock<std::shared_mutex> lock(g_RegexMutex);
		g_RegexList.swap(regexes);
		g_BlockList.swap(blocks);
		g_StderrList.swap(stderrs);
		g_ChannelList.swap(channels);
	}

	DeleteRegexes(regexes);
	DeleteRegexes(blocks);
	DeleteRegexes(stderrs);

#ifndef _WIN32
	// Only touch fd 2 when there is something to filter
	if (!g_StderrList.empty())
		InstallStderrFilter();
#else
	if (!g_StderrList.empty())
		META_CONPRINTF("[CleanerCS2] @stderr rules are only supported on Linux\n");
#endif

	ResolveChannels();

	g_BypassFilter = true;
	META_CONPRINTF("[CleanerCS2] Loaded %d regexes, %d blocks, %d channel rules, %d stderr rules\n", (int)g_RegexList.size(), (int)g_BlockList.size(), (int)g_ChannelList.size(), (int)g_StderrList.size());
	for (auto& rule : g_ChannelList)
	{
		if (rule.id == -1)
			META_CONPRINTF("[CleanerCS2] Channel '%s' is not registered yet, retrying on map load\n", rule.name.c_str());
	}
	g_BypassFilter = false;
}

CON_COMMAND_F(conclear_reload, "Reloads the cleaner config", FCVAR_SPONLY | FCVAR_LINKED_CONCOMMAND)
{
	LoadConfig();
}

CON_COMMAND_F(conclear_debug, "Tag every console message with [cc2:channel|severity]. Usage: conclear_debug <0|1>", FCVAR_SPONLY | FCVAR_LINKED_CONCOMMAND)
{
	if (args.ArgC() > 1)
		g_Debug = atoi(args[1]) != 0;

	META_CONPRINTF("[CleanerCS2] Debug is %s\n", g_Debug ? "on" : "off");
}

PLUGIN_EXPOSE(CleanerPlugin, g_CleanerPlugin);
bool CleanerPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, icvar, ICvar, CVAR_INTERFACE_VERSION);

	g_SMAPI->AddListener( this, this );

	g_pCVar = icvar;
	META_CONVAR_REGISTER( FCVAR_RELEASE | FCVAR_CLIENT_CAN_EXECUTE | FCVAR_GAMEDLL );

	LoadConfig();

	if (!SetupHook())
	{
		META_CONPRINTF("[CleanerCS2] Failed to setup hook\n");
		return false;
	}

	return true;
}

bool CleanerPlugin::Unload(char *error, size_t maxlen)
{
#ifndef _WIN32
	RemoveStderrFilter();
#endif

	std::unique_lock<std::shared_mutex> lock(g_RegexMutex);

	DeleteRegexes(g_RegexList);
	DeleteRegexes(g_BlockList);
	DeleteRegexes(g_StderrList);
	g_ChannelList.clear();

	return true;
}

void CleanerPlugin::AllPluginsLoaded()
{
}

void CleanerPlugin::OnLevelInit( char const *pMapName,
									 char const *pMapEntities,
									 char const *pOldLevel,
									 char const *pLandmarkName,
									 bool loadGame,
									 bool background )
{
	ResolveChannels();
}

void CleanerPlugin::OnLevelShutdown()
{
}

bool CleanerPlugin::Pause(char *error, size_t maxlen)
{
	return true;
}

bool CleanerPlugin::Unpause(char *error, size_t maxlen)
{
	return true;
}

const char *CleanerPlugin::GetLicense()
{
	return "GPLv3";
}

const char *CleanerPlugin::GetVersion()
{
	return "2.1";
}

const char *CleanerPlugin::GetDate()
{
	return __DATE__;
}

const char *CleanerPlugin::GetLogTag()
{
	return "CLEANER";
}

const char *CleanerPlugin::GetAuthor()
{
	return "Poggu";
}

const char *CleanerPlugin::GetDescription()
{
	return "Console regex filter";
}

const char *CleanerPlugin::GetName()
{
	return "CleanerCS2";
}

const char *CleanerPlugin::GetURL()
{
	return "https://poggu.me";
}
