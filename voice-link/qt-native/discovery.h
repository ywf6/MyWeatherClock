#pragma once
#include <QtCore>
#include <QtNetwork>

class DeviceDiscovery : public QObject {
    Q_OBJECT
public:
    explicit DeviceDiscovery(QObject *parent = nullptr);
    void start(QList<QHostAddress> destinations = {}, quint16 port = 8766);
    void stop();
    bool isActive() const { return deadline.isActive(); }
signals:
    void found(QString address, quint16 port, QString id);
    void finished();
    void error(QString message);
private:
    QUdpSocket socket;
    QTimer resend, deadline;
    QByteArray nonce;
    QList<QHostAddress> targets;
    quint16 discoveryPort = 8766;
    QSet<QString> seen;
    void send();
    void receive();
};
