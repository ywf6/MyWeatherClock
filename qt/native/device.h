/* [阅读说明]
 * 上位机设备层接口：Channel 统一 Win32 串口和 TCP 字节流，DeviceWorker 串行执行设备事务。
 * DeviceWorker 对象由界面创建，run() 在工作线程执行；submit() 通过互斥锁保护的队列交接命令。
 */
#pragma once
#include <QtCore>
#include <QtNetwork>
#include <atomic>
#include <memory>
struct Config { QString serial, host, voiceDir, python = "python"; quint16 port = 8765; };
// 本机未安装 QtSerialPort，串口使用 Windows 原生 API。
/* [阅读说明]
 * read(count) 按指定字节数读取，line() 按 LF 分行；TCP 单次 read 不对应一个完整协议消息。
 */
class Channel {
public:
    explicit Channel(const Config &c);
    ~Channel();
    void write(const QByteArray &data);
    QByteArray read(int count, int timeout = 3000);
    QByteArray line(int timeout = 3000);
private:
    std::unique_ptr<QTcpSocket> socket;
    void *serial = nullptr;
};
/* [阅读说明]
 * stop/listenStop 使用原子标志供 GUI 线程请求停止；界面更新通过 Qt signals 回到 GUI 线程。
 */
class DeviceWorker : public QThread {
    Q_OBJECT
public:
    explicit DeviceWorker(Config c) : config(std::move(c)) {}
    void submit(const QString &command, const QString &value = {});
    void disconnectDevice() { stop = true; listenStop = true; }
    void stopListening() { listenStop = true; }
    static QStringList serialPorts();
signals:
    void connected();
    void firmwareMode(bool boot, bool canUpgrade, bool xiaozhi);
    void upgradeProgress(int percent, QString stage);
    void networkReady(QString address);
    void message(QString text);
    void state(QVariantMap values);
    void answer(QString question, QString text, QByteArray bitmap);
    void busy(bool value);
    void taskDone(QString command);
    void listening(bool value);
protected:
    void run() override;
private:
    Config config;
    QMutex mutex;
    QQueue<QPair<QString, QString>> commands;
    std::atomic_bool stop{false}, listenStop{false};
    std::unique_ptr<Channel> channel;
    void expect(const QByteArray &expected, int timeout = 3000);
    QList<QVariantMap> exchange(const QString &command, int timeout = 3000);
    QStringList capabilities();
    QVariantMap status();
    QVariantMap ack(const QString &command);
    int startChat(bool fromKey = false);
    bool current(int rev);
    void display(int rev, const QByteArray &pixels);
    void play(const QByteArray &pcm);
    QByteArray record();
    QJsonObject helper(QJsonObject request);
    void turn(const QString &text = {}, bool fromKey = false);
    void listen();
    void joinHotspot(const QString &configuration);
    void upgrade(const QString &path);
};
