/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtproto_proxy_data.h"
#include "core/core_settings_proxy.h"

namespace Core::XrayProxy {

struct StartResult {
	bool success = false;
	QString error;
	uint32 port = 0;
};

struct Status {
	bool supported = false;
	bool enabled = false;
	bool running = false;
	uint32 port = 0;
	QString binaryPath;
	QString configPath;
	QString logPath;
	QString version;
};

[[nodiscard]] bool IsSupportedLink(const QString &link);
[[nodiscard]] QString BinaryPath();
[[nodiscard]] QString ConfigFilePath();
[[nodiscard]] QString LogFilePath();
[[nodiscard]] QString VersionText();
[[nodiscard]] QString StatusLabel();
[[nodiscard]] Status CurrentStatus();

StartResult Start(const QString &link);
StartResult Start(
	const QString &link,
	XrayProxyMode mode,
	const XrayProxyFragmentSettings &fragment);
StartResult TestConfig(
	const QString &link,
	XrayProxyMode mode,
	const XrayProxyFragmentSettings &fragment);
StartResult Restart();
[[nodiscard]] QString RecentLogText(int maxLines = 300);
[[nodiscard]] int TestLatency();
void OpenLogFile();
void OpenLatestRelease();
void Stop();
void SyncFromSettings();

} // namespace Core::XrayProxy
