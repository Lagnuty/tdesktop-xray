/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/xray_proxy_manager.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "settings.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QTcpServer>

#include <algorithm>
#include <optional>

namespace Core::XrayProxy {
namespace {

constexpr auto kLoopbackHost = "127.0.0.1";

[[nodiscard]] bool PlatformSupported() {
#if defined Q_OS_WIN || defined Q_OS_MAC
	return true;
#else // Q_OS_WIN || Q_OS_MAC
	return false;
#endif // Q_OS_WIN || Q_OS_MAC
}

struct State {
	std::unique_ptr<QProcess> process;
	uint32 port = 0;
	QString link;
	XrayProxyMode mode = XrayProxyMode::Proxy;
	XrayProxyFragmentSettings fragment;
};

[[nodiscard]] State &GlobalState() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString ConfigPath() {
	return cWorkingDir() + u"tdata/xray/telegram-xray.json"_q;
}

[[nodiscard]] QString TestConfigPath() {
	return cWorkingDir() + u"tdata/xray/telegram-xray-test.json"_q;
}

[[nodiscard]] QString LogPath() {
	return cWorkingDir() + u"tdata/xray/telegram-xray.log"_q;
}

[[nodiscard]] QString XrayPath() {
	const auto custom = Core::App().settings().proxy().xrayProxyBinaryPath();
	if (!custom.isEmpty() && QFile::exists(custom)) {
		return custom;
	}
#ifdef Q_OS_WIN
	const auto exePath = cExeDir() + u"xray.exe"_q;
	if (QFile::exists(exePath)) {
		return exePath;
	}
	return cWorkingDir() + u"xray.exe"_q;
#elif defined Q_OS_MAC
	const auto bundle = cExeDir() + cExeName();
	const auto paths = {
		bundle + u"/Contents/Frameworks/xray"_q,
		bundle + u"/Contents/MacOS/xray"_q,
		bundle + u"/Contents/Resources/xray"_q,
		cExeDir() + u"xray"_q,
		cWorkingDir() + u"xray"_q,
	};
	for (const auto &path : paths) {
		if (QFile::exists(path)) {
			return path;
		}
	}
	return QString();
#else // Q_OS_WIN || Q_OS_MAC
	return QString();
#endif // Q_OS_WIN || Q_OS_MAC
}

[[nodiscard]] QString ExtractVersion(QString output) {
	output = output.trimmed();
	const auto match = QRegularExpression(
		uR"(Xray\s+([0-9]+(?:\.[0-9]+)*))"_q
	).match(output);
	return match.hasMatch() ? match.captured(1) : QString();
}

[[nodiscard]] uint32 AllocatePort() {
	auto server = QTcpServer();
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		return 0;
	}
	return server.serverPort();
}

[[nodiscard]] bool IsNumberRange(const QString &text, bool allowZero) {
	const auto match = QRegularExpression(
		uR"(^([0-9]+)-([0-9]+)$)"_q
	).match(text);
	if (!match.hasMatch()) {
		return false;
	}
	const auto from = match.captured(1).toInt();
	const auto to = match.captured(2).toInt();
	return from <= to && (allowZero || from > 0);
}

[[nodiscard]] bool IsFragmentPackets(const QString &text) {
	return (text == u"tlshello"_q) || IsNumberRange(text, false);
}

[[nodiscard]] bool IsValidFragment(
		const XrayProxyFragmentSettings &fragment) {
	return !fragment.enabled
		|| (IsFragmentPackets(fragment.packets)
			&& IsNumberRange(fragment.length, false)
			&& IsNumberRange(fragment.interval, true));
}

[[nodiscard]] bool IsSameFragment(
		const XrayProxyFragmentSettings &a,
		const XrayProxyFragmentSettings &b) {
	return a.enabled == b.enabled
		&& a.packets == b.packets
		&& a.length == b.length
		&& a.interval == b.interval;
}

[[nodiscard]] QJsonObject StreamSettings(const QUrl &url) {
	const auto query = QUrlQuery(url);
	auto result = QJsonObject();
	const auto type = query.queryItemValue(u"type"_q);
	const auto security = query.queryItemValue(u"security"_q);
	if (!type.isEmpty()) {
		result.insert(u"network"_q, type);
	}
	if (!security.isEmpty()) {
		result.insert(u"security"_q, security);
	}
	if (type == u"ws"_q) {
		const auto path = query.queryItemValue(u"path"_q);
		const auto host = query.queryItemValue(u"host"_q);
		auto ws = QJsonObject();
		if (!path.isEmpty()) {
			ws.insert(u"path"_q, path);
		}
		if (!host.isEmpty()) {
			ws.insert(u"headers"_q, QJsonObject{ { u"Host"_q, host } });
		}
		result.insert(u"wsSettings"_q, ws);
	} else if (type == u"grpc"_q) {
		const auto serviceName = query.queryItemValue(u"serviceName"_q);
		if (!serviceName.isEmpty()) {
			result.insert(
				u"grpcSettings"_q,
				QJsonObject{ { u"serviceName"_q, serviceName } });
		}
	}
	if (security == u"tls"_q || security == u"reality"_q) {
		auto tls = QJsonObject();
		const auto sni = query.queryItemValue(u"sni"_q);
		const auto fp = query.queryItemValue(u"fp"_q);
		const auto pbk = query.queryItemValue(u"pbk"_q);
		const auto sid = query.queryItemValue(u"sid"_q);
		const auto spx = query.queryItemValue(u"spx"_q);
		if (!sni.isEmpty()) {
			tls.insert(u"serverName"_q, sni);
		}
		if (!fp.isEmpty()) {
			tls.insert(u"fingerprint"_q, fp);
		}
		if (!pbk.isEmpty()) {
			tls.insert(u"publicKey"_q, pbk);
		}
		if (!sid.isEmpty()) {
			tls.insert(u"shortId"_q, sid);
		}
		if (!spx.isEmpty()) {
			tls.insert(u"spiderX"_q, spx);
		}
		result.insert(
			(security == u"reality"_q)
				? u"realitySettings"_q
				: u"tlsSettings"_q,
			tls);
	}
	return result;
}

void ApplyFragmentDialer(QJsonObject &outbound) {
	outbound.insert(u"tag"_q, u"telegram-proxy-out"_q);
	auto stream = outbound.value(u"streamSettings"_q).toObject();
	auto sockopt = stream.value(u"sockopt"_q).toObject();
	sockopt.insert(u"dialerProxy"_q, u"telegram-fragment-out"_q);
	sockopt.insert(u"tcpNoDelay"_q, true);
	stream.insert(u"sockopt"_q, sockopt);
	outbound.insert(u"streamSettings"_q, stream);
}

[[nodiscard]] QJsonObject FragmentOutbound(
		const XrayProxyFragmentSettings &fragment) {
	return {
		{ u"tag"_q, u"telegram-fragment-out"_q },
		{ u"protocol"_q, u"freedom"_q },
		{ u"settings"_q, QJsonObject{
			{ u"fragment"_q, QJsonObject{
				{ u"packets"_q, fragment.packets },
				{ u"length"_q, fragment.length },
				{ u"interval"_q, fragment.interval },
			} },
		} },
		{ u"streamSettings"_q, QJsonObject{
			{ u"sockopt"_q, QJsonObject{
				{ u"tcpNoDelay"_q, true },
			} },
		} },
	};
}

[[nodiscard]] QJsonObject VlessOutbound(const QUrl &url) {
	const auto query = QUrlQuery(url);
	const auto encryption = query.queryItemValue(u"encryption"_q);
	auto user = QJsonObject{
		{ u"id"_q, url.userName() },
		{ u"encryption"_q, encryption.isEmpty()
			? u"none"_q
			: encryption },
	};
	const auto flow = query.queryItemValue(u"flow"_q);
	if (!flow.isEmpty()) {
		user.insert(u"flow"_q, flow);
	}
	auto result = QJsonObject{
		{ u"protocol"_q, u"vless"_q },
		{ u"settings"_q, QJsonObject{
			{ u"vnext"_q, QJsonArray{ QJsonObject{
				{ u"address"_q, url.host() },
				{ u"port"_q, url.port(443) },
				{ u"users"_q, QJsonArray{ user } },
			} } },
		} },
	};
	const auto stream = StreamSettings(url);
	if (!stream.isEmpty()) {
		result.insert(u"streamSettings"_q, stream);
	}
	return result;
}

[[nodiscard]] QJsonObject TrojanOutbound(const QUrl &url) {
	auto result = QJsonObject{
		{ u"protocol"_q, u"trojan"_q },
		{ u"settings"_q, QJsonObject{
			{ u"servers"_q, QJsonArray{ QJsonObject{
				{ u"address"_q, url.host() },
				{ u"port"_q, url.port(443) },
				{ u"password"_q, url.userName() },
			} } },
		} },
	};
	const auto stream = StreamSettings(url);
	if (!stream.isEmpty()) {
		result.insert(u"streamSettings"_q, stream);
	}
	return result;
}

[[nodiscard]] QJsonObject Hysteria2Outbound(const QUrl &url) {
	return {
		{ u"protocol"_q, u"hysteria2"_q },
		{ u"settings"_q, QJsonObject{
			{ u"servers"_q, QJsonArray{ QJsonObject{
				{ u"address"_q, url.host() },
				{ u"port"_q, url.port(443) },
				{ u"password"_q, url.userName() },
			} } },
		} },
	};
}

[[nodiscard]] QJsonObject VmessOutbound(const QString &link) {
	auto encoded = link.mid(u"vmess://"_q.size()).toUtf8();
	encoded.replace('-', '+').replace('_', '/');
	while (encoded.size() % 4) {
		encoded.append('=');
	}
	const auto json = QByteArray::fromBase64(encoded);
	const auto source = QJsonDocument::fromJson(json).object();
	auto user = QJsonObject{
		{ u"id"_q, source.value(u"id"_q).toString() },
		{ u"alterId"_q, source.value(u"aid"_q).toString().toInt() },
		{ u"security"_q, source.value(u"scy"_q).toString(u"auto"_q) },
	};
	auto result = QJsonObject{
		{ u"protocol"_q, u"vmess"_q },
		{ u"settings"_q, QJsonObject{
			{ u"vnext"_q, QJsonArray{ QJsonObject{
				{ u"address"_q, source.value(u"add"_q).toString() },
				{ u"port"_q, source.value(u"port"_q).toString().toInt() },
				{ u"users"_q, QJsonArray{ user } },
			} } },
		} },
	};
	auto stream = QJsonObject();
	const auto network = source.value(u"net"_q).toString();
	const auto tls = source.value(u"tls"_q).toString();
	const auto sni = source.value(u"sni"_q).toString();
	const auto path = source.value(u"path"_q).toString();
	const auto host = source.value(u"host"_q).toString();
	if (!network.isEmpty()) {
		stream.insert(u"network"_q, network);
	}
	if (!tls.isEmpty()) {
		stream.insert(u"security"_q, tls);
	}
	if (!sni.isEmpty()) {
		stream.insert(
			u"tlsSettings"_q,
			QJsonObject{ { u"serverName"_q, sni } });
	}
	if (network == u"ws"_q) {
		auto ws = QJsonObject();
		if (!path.isEmpty()) {
			ws.insert(u"path"_q, path);
		}
		if (!host.isEmpty()) {
			ws.insert(u"headers"_q, QJsonObject{ { u"Host"_q, host } });
		}
		stream.insert(u"wsSettings"_q, ws);
	} else if (network == u"grpc"_q && !path.isEmpty()) {
		stream.insert(
			u"grpcSettings"_q,
			QJsonObject{ { u"serviceName"_q, path } });
	}
	if (!stream.isEmpty()) {
		result.insert(u"streamSettings"_q, stream);
	}
	return result;
}

[[nodiscard]] std::optional<QJsonObject> OutboundFromLink(const QString &link) {
	const auto trimmed = link.trimmed();
	const auto url = QUrl(trimmed);
	const auto scheme = url.scheme().toLower();
	if (scheme == u"vless"_q
		&& !url.userName().isEmpty()
		&& !url.host().isEmpty()) {
		return VlessOutbound(url);
	} else if (scheme == u"trojan"_q
		&& !url.userName().isEmpty()
		&& !url.host().isEmpty()) {
		return TrojanOutbound(url);
	} else if ((scheme == u"hysteria2"_q || scheme == u"hy2"_q)
		&& !url.userName().isEmpty()
		&& !url.host().isEmpty()) {
		return Hysteria2Outbound(url);
	} else if (scheme == u"vmess"_q) {
		auto outbound = VmessOutbound(trimmed);
		const auto settings = outbound.value(u"settings"_q).toObject();
		const auto vnextList = settings.value(u"vnext"_q).toArray();
		const auto vnext = vnextList.isEmpty()
			? QJsonObject()
			: vnextList.at(0).toObject();
		if (!vnext.value(u"address"_q).toString().isEmpty()
			&& vnext.value(u"port"_q).toInt() > 0) {
			return outbound;
		}
	}
	return {};
}

[[nodiscard]] std::optional<QByteArray> BuildConfig(
		const QString &link,
		uint32 port,
		XrayProxyMode mode,
		const XrayProxyFragmentSettings &fragment) {
	auto outbound = OutboundFromLink(link);
	if (!outbound) {
		return {};
	}
	auto outbounds = QJsonArray();
	auto proxyOutbound = *outbound;
	if (fragment.enabled) {
		ApplyFragmentDialer(proxyOutbound);
	}
	outbounds.append(proxyOutbound);
	if (fragment.enabled) {
		outbounds.append(FragmentOutbound(fragment));
	}
	const auto inbound = (mode == XrayProxyMode::Vpn)
		? QJsonObject{
			{ u"tag"_q, u"telegram-tun-in"_q },
			{ u"port"_q, 0 },
			{ u"protocol"_q, u"tun"_q },
			{ u"settings"_q, QJsonObject{
				{ u"name"_q, u"telegram-xray"_q },
				{ u"MTU"_q, 1500 },
			} },
		}
		: QJsonObject{
			{ u"tag"_q, u"telegram-socks-in"_q },
			{ u"listen"_q, QString::fromLatin1(kLoopbackHost) },
			{ u"port"_q, int(port) },
			{ u"protocol"_q, u"socks"_q },
			{ u"settings"_q, QJsonObject{
				{ u"auth"_q, u"noauth"_q },
				{ u"udp"_q, false },
			} },
		};
	const auto config = QJsonObject{
		{ u"log"_q, QJsonObject{
			{ u"access"_q, LogPath() },
			{ u"error"_q, LogPath() },
			{ u"loglevel"_q, u"warning"_q },
		} },
		{ u"inbounds"_q, QJsonArray{ inbound } },
		{ u"outbounds"_q, outbounds },
	};
	return QJsonDocument(config).toJson(QJsonDocument::Indented);
}

[[nodiscard]] bool WriteConfig(const QByteArray &config, const QString &path) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	return file.write(config) == config.size();
}

[[nodiscard]] bool WriteConfig(const QByteArray &config) {
	return WriteConfig(config, ConfigPath());
}

void AppendLog(const QString &text) {
	if (text.isEmpty()) {
		return;
	}
	const auto path = LogPath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
		file.write(text.toUtf8());
		if (!text.endsWith('\n')) {
			file.write("\n", 1);
		}
	}
}

void ApplyLocalProxy(uint32 port) {
	Core::App().setCurrentProxy({
		.type = MTP::ProxyData::Type::Socks5,
		.host = QString::fromLatin1(kLoopbackHost),
		.port = port,
	}, MTP::ProxyData::Settings::Enabled);
}

} // namespace

bool IsSupportedLink(const QString &link) {
	const auto scheme = QUrl(link.trimmed()).scheme().toLower();
	return scheme == u"vless"_q
		|| scheme == u"vmess"_q
		|| scheme == u"trojan"_q
		|| scheme == u"hysteria2"_q
		|| scheme == u"hy2"_q;
}

QString BinaryPath() {
	return XrayPath();
}

QString ConfigFilePath() {
	return ConfigPath();
}

QString LogFilePath() {
	return LogPath();
}

QString VersionText() {
	const auto xray = XrayPath();
	if (xray.isEmpty() || !QFile::exists(xray)) {
		return tr::lng_xray_proxy_version_missing(tr::now);
	}
	auto process = QProcess();
	process.setProgram(xray);
	process.setArguments({ u"version"_q });
	process.setWorkingDirectory(QFileInfo(xray).absolutePath());
	process.start();
	if (!process.waitForFinished(1500)) {
		process.kill();
		process.waitForFinished(500);
		return tr::lng_xray_proxy_version_unknown(tr::now);
	}
	const auto output = QString::fromUtf8(
		process.readAllStandardOutput()
			+ process.readAllStandardError());
	const auto version = ExtractVersion(output);
	return version.isEmpty()
		? tr::lng_xray_proxy_version_unknown(tr::now)
		: tr::lng_xray_proxy_version(
			tr::now,
			lt_version,
			version);
}

QString StatusLabel() {
	const auto &settings = Core::App().settings().proxy();
	if (!settings.xrayProxyEnabled()) {
		return tr::lng_xray_proxy_disabled(tr::now);
	}
	const auto &state = GlobalState();
	return (state.process
			&& state.process->state() != QProcess::NotRunning)
		? tr::lng_xray_proxy_active(tr::now)
		: tr::lng_xray_proxy_configured(tr::now);
}

Status CurrentStatus() {
	const auto &settings = Core::App().settings().proxy();
	const auto &state = GlobalState();
	return {
		.supported = PlatformSupported(),
		.enabled = settings.xrayProxyEnabled(),
		.running = (state.process
			&& state.process->state() != QProcess::NotRunning),
		.port = state.port,
		.binaryPath = XrayPath(),
		.configPath = ConfigPath(),
		.logPath = LogPath(),
		.version = VersionText(),
	};
}

StartResult TestConfig(
		const QString &link,
		XrayProxyMode mode,
		const XrayProxyFragmentSettings &fragment) {
	if (!PlatformSupported()) {
		return { false, tr::lng_xray_proxy_platform_unsupported(tr::now), 0 };
	}
	if (!IsSupportedLink(link)) {
		return { false, tr::lng_xray_proxy_invalid_link(tr::now), 0 };
	}
	if (!IsValidFragment(fragment)) {
		return { false, tr::lng_xray_proxy_invalid_fragment(tr::now), 0 };
	}
	const auto xray = XrayPath();
	if (xray.isEmpty() || !QFile::exists(xray)) {
		return { false, tr::lng_xray_proxy_missing_binary(tr::now), 0 };
	}
	const auto port = (mode == XrayProxyMode::Proxy) ? AllocatePort() : 0;
	if (mode == XrayProxyMode::Proxy && !port) {
		return { false, tr::lng_xray_proxy_port_failed(tr::now), 0 };
	}
	const auto config = BuildConfig(link, port, mode, fragment);
	if (!config || !WriteConfig(*config, TestConfigPath())) {
		return { false, tr::lng_xray_proxy_config_failed(tr::now), 0 };
	}
	auto process = QProcess();
	process.setProgram(xray);
	process.setArguments({
		u"run"_q,
		u"-test"_q,
		u"-config"_q,
		TestConfigPath(),
	});
	process.setWorkingDirectory(QFileInfo(xray).absolutePath());
	process.start();
	if (!process.waitForFinished(5000)) {
		process.kill();
		process.waitForFinished(1000);
		return { false, tr::lng_xray_proxy_config_test_failed(tr::now), 0 };
	}
	const auto output = QString::fromUtf8(
		process.readAllStandardOutput()
			+ process.readAllStandardError());
	AppendLog(output);
	if (process.exitStatus() != QProcess::NormalExit
		|| process.exitCode() != 0) {
		return {
			false,
			output.isEmpty()
				? tr::lng_xray_proxy_config_test_failed(tr::now)
				: output.trimmed(),
			0,
		};
	}
	return { true, QString(), port };
}

StartResult Start(
		const QString &link,
		XrayProxyMode mode,
		const XrayProxyFragmentSettings &fragment) {
	if (!PlatformSupported()) {
		return { false, tr::lng_xray_proxy_platform_unsupported(tr::now), 0 };
	}
	if (!IsSupportedLink(link)) {
		return { false, tr::lng_xray_proxy_invalid_link(tr::now), 0 };
	}
	if (!IsValidFragment(fragment)) {
		return { false, tr::lng_xray_proxy_invalid_fragment(tr::now), 0 };
	}
	const auto xray = XrayPath();
	if (xray.isEmpty() || !QFile::exists(xray)) {
		return { false, tr::lng_xray_proxy_missing_binary(tr::now), 0 };
	}
	auto &state = GlobalState();
	if (state.process
		&& state.link == link
		&& state.mode == mode
		&& IsSameFragment(state.fragment, fragment)) {
		return { true, QString(), state.port };
	}
	Stop();
	const auto port = (mode == XrayProxyMode::Proxy) ? AllocatePort() : 0;
	if (mode == XrayProxyMode::Proxy && !port) {
		return { false, tr::lng_xray_proxy_port_failed(tr::now), 0 };
	}
	const auto config = BuildConfig(link, port, mode, fragment);
	if (!config || !WriteConfig(*config)) {
		return { false, tr::lng_xray_proxy_config_failed(tr::now), 0 };
	}
	const auto test = TestConfig(link, mode, fragment);
	if (!test.success) {
		return test;
	}
	auto process = std::make_unique<QProcess>();
	process->setProgram(xray);
	process->setArguments({ u"run"_q, u"-config"_q, ConfigPath() });
	process->setWorkingDirectory(QFileInfo(xray).absolutePath());
	process->setStandardOutputFile(LogPath(), QIODevice::Append);
	process->setStandardErrorFile(LogPath(), QIODevice::Append);
	process->start();
	if (!process->waitForStarted(5000)) {
		return { false, tr::lng_xray_proxy_start_failed(tr::now), 0 };
	}
	if (process->waitForFinished(300)) {
		AppendLog(QString::fromUtf8(
			process->readAllStandardOutput()
				+ process->readAllStandardError()));
		return { false, tr::lng_xray_proxy_start_failed(tr::now), 0 };
	}
	state.process = std::move(process);
	state.port = port;
	state.link = link;
	state.mode = mode;
	state.fragment = fragment;
	if (mode == XrayProxyMode::Proxy) {
		ApplyLocalProxy(port);
	} else {
		auto &proxy = Core::App().settings().proxy();
		Core::App().setCurrentProxy(
			proxy.selected(),
			MTP::ProxyData::Settings::System);
	}
	return { true, QString(), port };
}

StartResult Start(const QString &link) {
	const auto &settings = Core::App().settings().proxy();
	return Start(
		link,
		settings.xrayProxyMode(),
		settings.xrayProxyFragment());
}

StartResult Restart() {
	const auto &settings = Core::App().settings().proxy();
	if (!settings.xrayProxyEnabled()) {
		return { false, tr::lng_xray_proxy_disabled(tr::now), 0 };
	}
	Stop();
	return Start(
		settings.xrayProxyLink(),
		settings.xrayProxyMode(),
		settings.xrayProxyFragment());
}

QString RecentLogText(int maxLines) {
	auto file = QFile(LogPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	const auto lines = QString::fromUtf8(file.readAll()).split('\n');
	const auto from = std::max(0, int(lines.size()) - maxLines);
	return lines.mid(from).join('\n').trimmed();
}

int TestLatency() {
	const auto &state = GlobalState();
	if (!state.process || !state.port) {
		return -1;
	}
	auto timer = QElapsedTimer();
	auto socket = QTcpSocket();
	timer.start();
	socket.connectToHost(QHostAddress::LocalHost, state.port);
	if (!socket.waitForConnected(3000)) {
		return -1;
	}
	socket.disconnectFromHost();
	return int(timer.elapsed());
}

void OpenLogFile() {
	File::ShowInFolder(LogPath());
}

void OpenLatestRelease() {
	File::OpenUrl(u"https://github.com/XTLS/Xray-core/releases/latest"_q);
}

void Stop() {
	auto &state = GlobalState();
	if (state.process) {
		state.process->terminate();
		if (!state.process->waitForFinished(2000)) {
			state.process->kill();
			state.process->waitForFinished(2000);
		}
	}
	state = State();
}

void SyncFromSettings() {
	const auto &settings = Core::App().settings().proxy();
	if (settings.xrayProxyEnabled()) {
		Start(
			settings.xrayProxyLink(),
			settings.xrayProxyMode(),
			settings.xrayProxyFragment());
	} else {
		Stop();
	}
}

} // namespace Core::XrayProxy
