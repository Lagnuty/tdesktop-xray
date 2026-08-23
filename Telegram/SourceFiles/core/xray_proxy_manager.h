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

[[nodiscard]] bool IsSupportedLink(const QString &link);
[[nodiscard]] QString VersionText();
[[nodiscard]] QString StatusLabel();

StartResult Start(const QString &link);
StartResult Start(
	const QString &link,
	XrayProxyMode mode,
	const XrayProxyFragmentSettings &fragment);
void Stop();
void SyncFromSettings();

} // namespace Core::XrayProxy
