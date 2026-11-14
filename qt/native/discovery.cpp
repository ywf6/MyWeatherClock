/* [阅读说明]
 * UDP 设备发现：电脑广播 WC_DISCOVER1 + 随机 nonce，C3 回带同一 nonce 的 JSON。
 * 发现阶段只获得地址/端口，真正建立 TCP 连接后还要进行设备与 STM32 能力握手。
 */
#include "discovery.h"

DeviceDiscovery::DeviceDiscovery(QObject *parent) : QObject(parent) {
    socket.setProxy(QNetworkProxy::NoProxy);
    deadline.setSingleShot(true);
    connect(&resend, &QTimer::timeout, this, &DeviceDiscovery::send);
    connect(&deadline, &QTimer::timeout, this, [this]{ stop(); emit finished(); });
    connect(&socket, &QUdpSocket::readyRead, this, &DeviceDiscovery::receive);
}
/* [阅读说明]
 * 向活动 IPv4 网卡广播，约 700 ms 重发，5 秒结束；每轮生成新 nonce 过滤旧回复。
 */
void DeviceDiscovery::start(QList<QHostAddress> destinations, quint16 port) {
    stop(); seen.clear(); nonce = QUuid::createUuid().toRfc4122().toHex();
    discoveryPort = port;
    targets = std::move(destinations);
    if (targets.isEmpty()) {
        targets << QHostAddress(QHostAddress::Broadcast);
        for (const auto &interface : QNetworkInterface::allInterfaces()) {
            if (!(interface.flags() & QNetworkInterface::IsUp) || interface.flags() & QNetworkInterface::IsLoopBack) continue;
            for (const auto &entry : interface.addressEntries())
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull() && !targets.contains(entry.broadcast()))
                    targets << entry.broadcast();
        }
    }
    if (!socket.bind(QHostAddress(QHostAddress::AnyIPv4), 0)) { emit error("无法启动设备发现：" + socket.errorString()); emit finished(); return; }
    deadline.start(5000); resend.start(700); send();
}
void DeviceDiscovery::stop() { resend.stop(); deadline.stop(); socket.close(); }
void DeviceDiscovery::send() {
    const QByteArray packet = "WC_DISCOVER1 " + nonce + '\n';
    for (const auto &target : targets) socket.writeDatagram(packet, target, discoveryPort);
}
/* [阅读说明]
 * 检查服务名、协议版本、本轮 nonce、端口和设备 ID，并按地址/端口/ID 去重。
 * nonce 用于关联本轮发现，不等于设备身份认证。
 */
void DeviceDiscovery::receive() {
    while (socket.hasPendingDatagrams()) {
        auto packet = socket.receiveDatagram(512);
        if (packet.senderAddress().protocol() != QAbstractSocket::IPv4Protocol) continue;
        auto object = QJsonDocument::fromJson(packet.data()).object();
        const int port = object["port"].toInt();
        const auto id = object["id"].toString();
        if (object["service"] != "weatherclock-voice" || object["version"].toInt() != 1 ||
            object["nonce"].toString().toLatin1() != nonce || port < 1 || port > 65535 ||
            !QRegularExpression("^wc-[a-fA-F0-9]{12}$").match(id).hasMatch()) continue;
        const auto address = packet.senderAddress().toString();
        const auto key = address + ':' + QString::number(port) + '/' + id;
        if (seen.contains(key)) continue;
        seen.insert(key); emit found(address, quint16(port), id);
    }
}
