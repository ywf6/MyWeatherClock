/* [阅读说明]
 * Qt 设备工作线程：按顺序执行握手、状态查询、问答、点阵上传、播音和 OTA。
 * 阅读顺序：run -> exchange/status -> turn -> helper/display/play -> listen -> upgrade。
 * Qt 管设备通道，Python 后台只管模型、音频处理和图像生成，避免双方争抢串口/TCP。
 */
#include "device.h"
#include "layout.h"
#include <QtEndian>
#include <windows.h>
#include <stdexcept>

static QString backendPath() {
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../qt_backend.py");
}
static void fail(const QString &s) { throw std::runtime_error(s.toUtf8().constData()); }
Channel::Channel(const Config &c) {
    if (!c.host.isEmpty()) {
        socket = std::make_unique<QTcpSocket>();
        socket->setProxy(QNetworkProxy::NoProxy);
        socket->connectToHost(c.host, c.port);
        if (!socket->waitForConnected(3000)) fail("Wi-Fi 连接失败：" + socket->errorString());
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    } else {
        auto name = ("\\\\.\\" + c.serial).toStdWString();
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) fail("串口无法打开，请检查端口或关闭占用程序");
        DCB d{}; d.DCBlength = sizeof(d);
        if (!GetCommState(h, &d)) { CloseHandle(h); fail("无法读取串口配置"); }
        d.BaudRate = 460800; d.ByteSize = 8; d.Parity = NOPARITY; d.StopBits = ONESTOPBIT;
        d.fBinary = TRUE; d.fParity = FALSE; d.fOutxCtsFlow = FALSE; d.fOutxDsrFlow = FALSE;
        d.fDsrSensitivity = FALSE; d.fOutX = FALSE; d.fInX = FALSE; d.fAbortOnError = FALSE;
        d.fDtrControl = DTR_CONTROL_DISABLE; d.fRtsControl = RTS_CONTROL_DISABLE;
        COMMTIMEOUTS t{MAXDWORD, 0, 30, 0, 3000};
        if (!SetCommState(h, &d) || !SetCommTimeouts(h, &t)) { CloseHandle(h); fail("串口配置失败"); }
        SetupComm(h, 8192, 4096); PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR); serial = h;
    }
}
Channel::~Channel() { if (serial) CloseHandle(serial); }
void Channel::write(const QByteArray &data) {
    if (socket) {
        if (socket->write(data) != data.size()) fail("Wi-Fi 写入失败");
        QElapsedTimer t; t.start();
        while (socket->bytesToWrite()) if (t.elapsed() > 3000 || !socket->waitForBytesWritten(3000)) fail("Wi-Fi 发送超时");
    } else {
        DWORD n = 0;
        if (!WriteFile(serial, data.constData(), DWORD(data.size()), &n, nullptr) || n != DWORD(data.size())) fail("串口发送失败");
    }
}
/* [阅读说明]
 * 在总超时内累计读取指定字节数，处理 TCP 分段与串口短读。
 */
QByteArray Channel::read(int count, int timeout) {
    QByteArray out; QElapsedTimer t; t.start();
    while (out.size() < count) {
        int remaining = timeout - int(t.elapsed()); if (remaining <= 0) fail("设备接收超时");
        if (socket) {
            if (!socket->bytesAvailable() && !socket->waitForReadyRead(remaining)) fail("Wi-Fi 接收失败：" + socket->errorString());
            out += socket->read(count - out.size());
        } else {
            char b[4096]; DWORD n = 0;
            if (!ReadFile(serial, b, DWORD(qMin<qsizetype>(sizeof(b), count - out.size())), &n, nullptr)) fail("串口已断开");
            out.append(b, n);
        }
    }
    return out;
}
/* [阅读说明]
 * 逐字节读到 LF，最多 256 字节；音频负载必须调用定长 read，不能走这里。
 */
QByteArray Channel::line(int timeout) {
    QByteArray out; QElapsedTimer t; t.start();
    while (out.size() < 256) {
        out += read(1, qMax(1, timeout - int(t.elapsed())));
        if (out.endsWith('\n')) return out.trimmed();
    }
    fail("设备返回行超长"); return {};
}
QStringList DeviceWorker::serialPorts() {
    QStringList out; HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256], value[256]; DWORD len = 256, bytes = sizeof(value), type;
        LONG code = RegEnumValueW(key, i, name, &len, nullptr, &type, reinterpret_cast<BYTE *>(value), &bytes);
        if (code == ERROR_NO_MORE_ITEMS) break;
        if (code == ERROR_SUCCESS && type == REG_SZ) out << QString::fromWCharArray(value);
    }
    RegCloseKey(key); out.removeDuplicates(); out.sort(); return out;
}
/* [阅读说明]
 * GUI 线程入队，工作线程在 run 中取出；队列上限为 8，不在此直接执行设备操作。
 */
void DeviceWorker::submit(const QString &cmd, const QString &value) {
    QMutexLocker lock(&mutex); if (cmd == "listen") listenStop = false;
    if (commands.size() < 8) commands.enqueue({cmd, value});
}
void DeviceWorker::expect(const QByteArray &expected, int timeout) {
    auto got = channel->line(timeout); if (got != expected) fail("设备应答异常：" + QString::fromUtf8(got));
}
/* [阅读说明]
 * 发送 WC @WC1 命令，收集 @WC1 文本帧直到 WCEND；ERR 帧转为异常。
 */
QList<QVariantMap> DeviceWorker::exchange(const QString &cmd, int timeout) {
    channel->write(("WC @WC1 " + cmd + "\n").toLatin1()); QList<QVariantMap> frames;
    for (int n = 0; n < 5; ++n) {
        auto line = channel->line(timeout);
        if (line == "WCEND") {
            if (frames.isEmpty()) fail("设备返回空回复");
            for (const auto &frame : frames)
                if (frame["kind"] == "ERR") fail("设备通信失败：" + frame["reason"].toString());
            return frames;
        }
        if (!line.startsWith("@WC1 ")) fail("设备返回无效帧");
        auto tokens = line.mid(5).split(' '); QVariantMap f{{"kind", QString::fromLatin1(tokens.takeFirst())}};
        for (auto token : tokens) { int e = token.indexOf('='); if (e > 0) f[QString::fromLatin1(token.left(e))] = QString::fromUtf8(token.mid(e + 1)); }
        frames << f;
    }
    fail("设备缺少结束标记"); return {};
}
QStringList DeviceWorker::capabilities() {
    auto frames = exchange("HELLO");
    const auto hello = frames.takeFirst();
    if (hello["kind"] != "HELLO" || frames.size() > 1 ||
        (!frames.isEmpty() && frames[0]["kind"] != "STATE"))
        fail("STM32 握手回复不符");
    auto caps = hello["caps"].toString().split(',');
    if ((caps.contains("ota_app") || caps.contains("ota_boot")) && hello["board"] != "STM32F407")
        fail("STM32 型号与 OTA 固件不符");
    return caps;
}

static quint32 imageCrc(const QByteArray &image) {
    quint32 crc = ~quint32(0);
    for (unsigned char byte : image) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* [阅读说明]
 * 先验证升级包长度、CRC 和向量表，再经 C3 进入 STM32 Bootloader。
 * 96 字节一块，每块等 ACK；结束后等待新应用返回匹配的大小和 CRC，单应用区没有旧版本回滚。
 */
void DeviceWorker::upgrade(const QString &path) {
    if (config.host.isEmpty()) fail("请通过 Wi-Fi 连接后升级");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 12 + 512 || file.size() > 12 + WC_APP_MAX)
        fail("无法读取升级包，或文件大小不符合 STM32 分区");
    const auto header = file.read(12);
    if (header.size() != 12 || qFromLittleEndian<quint32>(header.constData()) != WC_OTA_MAGIC)
        fail("请选择 WeatherClock 的 .wcota 升级包");
    const auto size = qFromLittleEndian<quint32>(header.constData() + 4);
    const auto crc = qFromLittleEndian<quint32>(header.constData() + 8);
    const auto image = file.read(WC_APP_MAX + 1);
    if (size < 512 || size > WC_APP_MAX || image.size() != size || imageCrc(image) != crc)
        fail("升级包长度或 CRC32 不匹配");
    const auto sp = qFromLittleEndian<quint32>(image.constData());
    const auto reset = qFromLittleEndian<quint32>(image.constData() + 4);
    if (sp <= 0x20000000u || sp > WC_COOKIE_ADDR || (sp & 7) ||
        !(reset & 1) || reset < WC_APP_BASE + 8 || reset >= WC_APP_BASE + size)
        fail("固件的启动地址与当前分区不匹配");
    auto confirm = [&](const QString &command, int timeout = 3000) {
        auto frames = exchange(command, timeout);
        if (frames.size() != 1 || frames[0]["kind"] != "ACK" || frames[0]["cmd"] != command.section(' ', 0, 0))
            fail("升级命令未得到确认：" + command.section(' ', 0, 0));
    };
    channel->write("OTA_LINK\n");
    expect("OTA_LINK1");
    auto caps = capabilities();
    if (!caps.contains("ota_boot")) {
        if (!caps.contains("ota_app")) fail("请先用 ST-Link 安装支持 OTA 的 STM32 固件");
        confirm("OTAENTER");
        QElapsedTimer bootTimer;
        bootTimer.start();
        bool entered = false;
        while (!stop && bootTimer.elapsed() < 8000) {
            msleep(350);
            try {
                entered = capabilities().contains("ota_boot");
            } catch (const std::exception &) {
                // The reset and CRC check can overlap the first handshake.
            }
            if (entered) break;
        }
        if (!entered) fail("未进入 STM32 Bootloader");
    }
    const auto crcText = QString("%1").arg(crc, 8, 16, QLatin1Char('0'));
    emit upgradeProgress(0, "正在准备暂存区，请继续按住 K3");
    confirm(QString("OTABEGIN %1 %2").arg(size).arg(crcText), 23000);
    emit upgradeProgress(0, "开始传输，可以松开 K3");
    for (quint32 offset = 0; offset < size; offset += WC_CHUNK) {
        if (stop) fail("升级传输已停止，可重新发送固件包");
        const auto chunk = image.mid(offset, WC_CHUNK);
        confirm(QString("OTADATA %1 %2").arg(offset).arg(QString::fromLatin1(chunk.toHex())));
        const int progress = (offset + chunk.size()) * 100 / size;
        if (progress != int(offset * 100 / size)) emit upgradeProgress(progress, "正在传输固件");
    }
    if (stop) fail("升级传输已停止，可重新发送固件包");
    emit upgradeProgress(100, "正在校验并安装，请保持供电");
    confirm("OTAEND", 23000);
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 45000 && !stop) {
        msleep(450);
        try {
            if (!capabilities().contains("ota_app")) continue;
            auto frames = exchange("OTAVERIFY");
            if (frames.size() == 1 && frames[0]["kind"] == "IMAGE" &&
                frames[0]["size"].toUInt() == size && frames[0]["crc"].toString() == crcText) {
                emit upgradeProgress(100, "STM32 已启动并确认新固件");
                return;
            }
        } catch (const std::exception &) {
            // STM32 is temporarily unavailable while writing its own flash.
        }
    }
    fail("未确认新固件启动，请重新连接设备检查状态");
}
QVariantMap DeviceWorker::status() {
    auto frames = exchange("STATUS"); if (frames.size() != 1 || frames[0]["kind"] != "STATE") fail("设备未返回状态");
    auto s = frames[0];
    if (!QStringList{"BOOT", "HOME", "FACE", "CHAT"}.contains(s["page"].toString())) fail("页面状态无效");
    for (const QString &key : {"rev", "valid", "uptime_ms", "temp10", "humidity10", "outdoor10", "weather"}) {
        bool ok; int n = s.value(key).toString().toInt(&ok); if (!ok) fail("状态字段无效：" + key); s[key] = n;
    }
    emit state(s); return s;
}
QVariantMap DeviceWorker::ack(const QString &cmd) {
    auto f = exchange(cmd);
    if (f.size() != 1 || f[0]["kind"] != "ACK" || f[0]["cmd"] != cmd.section(' ', 0, 0)) fail("设备未确认命令：" + cmd.section(' ', 0, 0)); return f[0];
}
/* [阅读说明]
 * 请求并轮询 HOME -> FACE -> CHAT，返回页面 revision；物理 K2 唤醒若已被取消则停止。
 */
int DeviceWorker::startChat(bool fromKey) {
    auto s = status(); if (s["page"] == "CHAT") return s["rev"].toInt();
    if (s["page"] == "HOME") { if (fromKey) fail("按键唤醒已取消"); ack("WAKE"); }
    else if (s["page"] != "FACE") fail("设备尚未启动完成");
    QElapsedTimer t; t.start(); bool sawFace = s["page"] == "FACE";
    while (t.elapsed() < 4000 && !stop) {
        s = status(); if (s["page"] == "CHAT") return s["rev"].toInt();
        if (s["page"] == "FACE") sawFace = true;
        if (s["page"] == "HOME" && sawFace) fail("唤醒已取消"); msleep(50);
    }
    fail("未进入对话页"); return 0;
}
bool DeviceWorker::current(int rev) { auto s = status(); return !stop && s["page"] == "CHAT" && s["rev"].toInt() == rev; }
/* [阅读说明]
 * 上传 5616 字节点阵，绑定 revision，并检查每个块的 next 偏移；LCDEND 才提交整图。
 */
void DeviceWorker::display(int rev, const QByteArray &pixels) {
    if (pixels.size() != 5616) fail("中文点阵大小异常"); unsigned sum = 0; for (unsigned char b : pixels) sum += b;
    ack(QString("LCDSTART %1 %1 %2").arg(rev).arg(sum));
    for (int offset = 0; offset < pixels.size(); offset += 96) {
        auto b = pixels.mid(offset, 96); auto f = ack(QString("LCDDATA %1 %2 %3").arg(rev).arg(offset).arg(QString::fromLatin1(b.toHex())));
        if (f["next"].toInt() != offset + b.size()) fail("点阵偏移不一致");
    }
    ack(QString("LCDEND %1").arg(rev));
}
/* [阅读说明]
 * 5 秒 * 16000 样本/秒 * 2 字节 = 160000 字节，严格按头、负载、结束标记读取。
 */
QByteArray DeviceWorker::record() { channel->write("REC\n"); expect("PCM 160000"); auto pcm = channel->read(160000, 8000); expect("DONE"); return pcm; }
void DeviceWorker::play(const QByteArray &pcm) {
    if (pcm.isEmpty() || pcm.size() % 2 || pcm.size() > 960000) fail("播报音频大小异常");
    channel->write("PLAY " + QByteArray::number(pcm.size() / 2) + "\n"); expect("READY");
    for (int i = 0; i < pcm.size(); i += 3840) { channel->write(pcm.mid(i, 3840)); expect("OK"); } expect("DONE");
}
/* [阅读说明]
 * 启动 Python qt_backend.py，通过一行 JSON 传请求，读 JSON 回答；图像和 PCM 用 Base64 承载。
 */
QJsonObject DeviceWorker::helper(QJsonObject request) {
    QProcess p; p.setProgram(config.python);
    p.setArguments({"-B", backendPath(), "--voice-dir", config.voiceDir}); p.start();
    if (!p.waitForStarted(3000)) fail("无法启动 Python，请检查 Python 路径");
    p.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n'); p.closeWriteChannel();
    QElapsedTimer t; t.start();
    while (!p.waitForFinished(100)) if (stop || t.elapsed() > 60000) { p.kill(); p.waitForFinished(3000); fail(stop ? "操作已取消" : "语音组件超时"); }
    auto out = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    if (p.exitCode() != 0 || !out["ok"].toBool()) fail(out.value("error").toString("语音组件运行失败，请检查组件目录及依赖")); return out;
}
/* [阅读说明]
 * 一次问答：进入 CHAT -> THINK -> 文字/录音输入 -> 采集状态快照 -> 调模型 -> 上屏 -> 播音。
 * 模型返回后再次检查 revision，防止用户返回主页后收到迟到回答。
 */
void DeviceWorker::turn(const QString &text, bool fromKey) {
    int rev = startChat(fromKey); ack(QString("THINK %1").arg(rev)); QString label = text;
    QJsonObject request{{"action", "ask"}, {"question", text}};
    if (text.isEmpty()) {
        channel->write("WAKE_REPLY\n"); expect("DONE"); emit message("请说话，录音 5 秒…");
        request["recording"] = QString::fromLatin1(record().toBase64()); label = "语音提问";
    }
    auto s = status(); if (!current(rev)) { emit message("已离开对话页"); return; }
    request["snapshot"] = QJsonObject::fromVariantMap(s); emit message("正在等待百炼回答…"); auto out = helper(request);
    if (!current(rev)) { emit message("已返回主页，丢弃迟到回答"); return; }
    auto pixels = QByteArray::fromBase64(out["bitmap"].toString().toLatin1()); display(rev, pixels);
    emit answer(label, out["answer"].toString(), pixels);
    auto pcm = QByteArray::fromBase64(out["pcm"].toString().toLatin1()); if (!pcm.isEmpty() && current(rev)) play(pcm);
    emit message("问答完成");
}
/* [阅读说明]
 * 启动常驻 Python 唤醒进程并喂 480 字节 PCM 包；词唤醒或 K2 到达后停止流，执行一次 turn。
 */
void DeviceWorker::listen() {
    QProcess p; p.setProgram(config.python);
    p.setArguments({"-B", backendPath(), "--voice-dir", config.voiceDir, "--wake"}); p.start();
    if (!p.waitForStarted(3000)) fail("无法启动唤醒组件");
    if (!p.waitForReadyRead(20000) || p.readLine().trimmed() != "READY") { p.kill(); p.waitForFinished(3000); fail("唤醒模型加载失败"); }
    emit listening(true);
    try {
        while (!stop && !listenStop) {
            p.write(QByteArray(1, 'R')); p.waitForBytesWritten(1000);
            QElapsedTimer resetTimer; resetTimer.start(); bool reset = false;
            while (!reset && resetTimer.elapsed() < 5000) {
                if (!p.canReadLine()) p.waitForReadyRead(100);
                while (p.canReadLine()) if (p.readLine().trimmed() == "RESET") reset = true;
            }
            if (!reset) fail("唤醒组件重置超时");
            channel->write("WC_LISTEN\n"); expect("STREAM");
            QElapsedTimer keepalive; keepalive.start(); bool trigger = false, fromKey = false, ending = false;
            emit message("等待「小爱同学」或 K2");
            while (true) {
                if (!ending && (stop || listenStop)) { channel->write("!"); ending = true; }
                if (!ending && keepalive.elapsed() >= 500) { channel->write("."); keepalive.restart(); }
                auto marker = channel->read(1);
                if (marker[0] == 0) { if (!ending) fail("设备提前停止唤醒流"); break; }
                bool key = marker[0] == 2;
                if (marker[0] == 1) {
                    auto pcm = channel->read(480);
                    if (!ending) { p.write(QByteArray(1, 'P') + pcm); p.waitForBytesWritten(1000); }
                } else if (!key) fail("唤醒音频流损坏");
                if (!ending && !p.bytesAvailable()) p.waitForReadyRead(1);
                if (!ending) {
                    bool keyword = false;
                    while (p.canReadLine()) keyword |= p.readLine().trimmed() == "WAKE";
                    if (key || keyword) { trigger = true; fromKey = key; channel->write("!"); ending = true; }
                    if (p.state() == QProcess::NotRunning) fail("唤醒组件已退出");
                }
            }
            if (trigger && !stop && !listenStop) turn({}, fromKey);
        }
    } catch (...) { p.kill(); p.waitForFinished(3000); emit listening(false); throw; }
    p.kill(); p.waitForFinished(3000); emit listening(false);
}
void DeviceWorker::joinHotspot(const QString &configuration) {
    if (!config.host.isEmpty()) fail("请先通过 USB 串口连接外置 C3 后配网");
    auto data = QJsonDocument::fromJson(configuration.toUtf8()).object();
    auto ssid = data["ssid"].toString().toUtf8(), password = data["password"].toString().toUtf8();
    if (ssid.isEmpty() || ssid.size() > 32 || ssid.contains('\0') || password.contains('\0') ||
        (password.size() && (password.size() < 8 || password.size() > 63))) fail("热点名称或密码长度不正确");
    channel->write("NETJOIN " + ssid.toHex() + ' ' + (password.isEmpty() ? QByteArray("-") : password.toHex()) + '\n');
    password.fill('\0');
    auto reply = channel->line();
    if (reply != "NETJOIN OK") fail("设备不支持配网或配置无效，请确认已更新外置 C3 固件");
    emit message("正在连接电脑热点…");
    QElapsedTimer timer; timer.start();
    while (!stop && timer.elapsed() < 20000) {
        channel->write("NETINFO\n"); auto info = channel->line();
        if (info.startsWith("NET CONNECTED ")) {
            auto address = QString::fromLatin1(info.mid(14)); QHostAddress ip;
            if (!ip.setAddress(address) || ip.isNull() || ip.protocol() != QAbstractSocket::IPv4Protocol) fail("设备返回无效 IP");
            channel->write("NETCFG\n"); auto stored = channel->line();
            if (stored == "NETCFG PENDING") { msleep(300); continue; }
            if (stored != "NETCFG SAVED") fail("已连接热点，但配置保存失败，请重新配网并检查固件版本");
            emit networkReady(address); emit message("热点配置已保存，上电将自动连接。设备 IP：" + address); return;
        }
        if (info != "NET WAITING") fail("设备配网状态无效");
        msleep(300);
    }
    fail(stop ? "配网已取消" : "热点连接超时，请检查热点是否开启、2.4 GHz 频段及密码");
}
/* [阅读说明]
 * 所有设备事务在此工作线程串行执行，避免音频二进制流与文本命令交错。
 * 现状提醒：最外层 catch 覆盖整个循环，单次模型/传输异常也会结束当前连接。
 */
void DeviceWorker::run() {
    try {
        channel = std::make_unique<Channel>(config);
        channel->write("PING\n");
        const auto handshake = channel->line();
        const bool xiaozhi = handshake == "VOICE_XIAOZHI1";
        if (!xiaozhi && handshake != "VOICE1") fail("设备应答异常：" + QString::fromUtf8(handshake));
        auto caps = capabilities();
        bool boot = caps.contains("ota_boot");
        if (!boot && !caps.contains("voice_uart")) fail("STM32 固件不支持语音串口");
        emit firmwareMode(boot, caps.contains("ota_app") || boot, xiaozhi);
        emit connected();
        if (xiaozhi) emit message("已连接小智固件。语音问答由板上 K2 发起。");
        if (boot) emit message("STM32 处于升级模式");
        else if (!xiaozhi) emit message("设备握手成功");
        QElapsedTimer poll; poll.start(); if (!boot) status();
        while (!stop) {
            QPair<QString, QString> task; { QMutexLocker lock(&mutex); if (!commands.isEmpty()) task = commands.dequeue(); }
            if (task.first.isEmpty()) { if (!boot && poll.elapsed() >= 800) { status(); poll.restart(); } msleep(20); continue; }
            emit busy(true);
            if (task.first == "ota") {
                upgrade(task.second);
                boot = false;
                emit firmwareMode(false, true, xiaozhi);
            }
            else if (task.first == "home") ack("HOME");
            else if (xiaozhi) fail("此操作适用于原有 C3 语音固件，小智问答请使用板上 K2");
            else if (task.first == "join-wifi") joinHotspot(task.second);
            else if (task.first == "text") turn(task.second);
            else if (task.first == "record") turn();
            else if (task.first == "listen") listen();
            else if (task.first == "probe") {
                int rev = startChat(); auto out = helper({{"action", "render"}, {"question", "Qt 连接测试"}, {"answer", "通信与中文点阵接收正常。"}});
                auto pixels = QByteArray::fromBase64(out["bitmap"].toString().toLatin1()); display(rev, pixels);
                emit answer("Qt 连接测试", "通信与中文点阵接收正常。", pixels);
                channel->write("WAKE_REPLY\n"); expect("DONE"); emit message("点阵与提示音命令已确认，请检查屏幕和听感");
            }
            emit taskDone(task.first); emit busy(false); if (!boot && !stop) status(); poll.restart();
        }
    } catch (const std::exception &e) { emit message(QString::fromUtf8(e.what())); }
    channel.reset(); emit busy(false); emit listening(false);
}
