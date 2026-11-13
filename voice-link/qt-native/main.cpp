/* [阅读说明]
 * Windows Qt 界面入口：负责按钮、连接状态、日志、点阵预览和 DeviceWorker 的信号绑定。
 * 先读 device.h 理解接口，再读 device.cpp 的 run/turn/listen；界面对象留在 GUI 线程。
 * 本文件也提供发现、探测、升级等命令行入口，便于联调。
 */
#include "device.h"
#include "discovery.h"
#include <QtWidgets>

/* [阅读说明]
 * 窗口只提交命令并接收结果；阻塞设备 I/O、Python 子进程等待和 OTA 传输由 DeviceWorker::run() 执行。
 */
class Console : public QMainWindow {
public:
    DeviceWorker *worker = nullptr;
    bool online = false, working = false, isListening = false, closing = false;
    bool pendingConnect = false;
    bool bootMode = false, canUpgrade = false, upgrading = false;
    DeviceDiscovery *discovery = new DeviceDiscovery(this);
    QPushButton *findDevice = new QPushButton("查找设备");
    QComboBox *mode = new QComboBox, *serial = new QComboBox;
    QLineEdit *host = new QLineEdit, *question = new QLineEdit;
    QLineEdit *hotspot = new QLineEdit, *password = new QLineEdit;
    QPushButton *joinWifi = new QPushButton("连接电脑热点");
    QPushButton *upgrade = new QPushButton("升级 STM32 固件");
    QProgressBar *upgradeProgress = new QProgressBar;
    QLabel *upgradeStage = new QLabel;
    QLineEdit *voiceDir = new QLineEdit(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../voice-assets"));
    QLineEdit *python = new QLineEdit("python");
    QSpinBox *port = new QSpinBox;
    QPushButton *refresh = new QPushButton("刷新串口"), *connectButton = new QPushButton("连接设备");
    QPushButton *send = new QPushButton("发送"), *record = new QPushButton("录音提问 · 5 秒");
    QPushButton *listen = new QPushButton("开启语音唤醒"), *home = new QPushButton("返回主页");
    QPushButton *probe = new QPushButton("屏幕 / 提示音测试"), *browse = new QPushButton("选择语音组件目录");
    QPlainTextEdit *history = new QPlainTextEdit, *log = new QPlainTextEdit;
    QLabel *connection = new QLabel("未连接 · 选择外置 C3 串口或 Wi-Fi 地址");
    QLabel *telemetry = new QLabel("页面 —     室内 —     湿度 —     西安室外 —"), *preview = new QLabel("尚无回答");
    explicit Console() {
        setWindowTitle("WeatherClock · Qt 语音控制台"); resize(1180, 850); setMinimumSize(1000, 760);
        auto root = new QWidget; setCentralWidget(root); auto layout = new QVBoxLayout(root);
        layout->setContentsMargins(24, 18, 24, 18); layout->setSpacing(12);
        auto title = new QLabel("WeatherClock  /  语音控制台"); title->setObjectName("title"); layout->addWidget(title); layout->addWidget(connection);
        auto box = new QGroupBox("设备连接"); auto row = new QHBoxLayout(box);
        mode->addItems({"USB 串口", "Wi-Fi / TCP"}); serial->setMinimumWidth(160);
        port->setRange(1, 65535); port->setValue(8765); host->setPlaceholderText("自动发现 C3，无需填写 IP"); host->setReadOnly(true);
        connectButton->setObjectName("primary");
        for (QWidget *w : QList<QWidget *>{mode, serial, refresh, host, port, findDevice, connectButton}) row->addWidget(w);
        layout->addWidget(box);
        auto networkRow = new QHBoxLayout;
        hotspot->setPlaceholderText("首次或更换热点时填写（2.4 GHz）"); password->setPlaceholderText("密码保存于设备，上位机不保存");
        password->setEchoMode(QLineEdit::Password);
        networkRow->addWidget(new QLabel("串口配网")); networkRow->addWidget(hotspot); networkRow->addWidget(password); networkRow->addWidget(joinWifi);
        auto updateRow = new QHBoxLayout;
        updateRow->addWidget(upgrade); updateRow->addWidget(upgradeProgress, 1); updateRow->addWidget(upgradeStage);
        upgradeProgress->hide(); upgradeStage->hide();
        layout->addLayout(updateRow);
        layout->addLayout(networkRow);
        auto hint = new QLabel("日常使用：开启电脑热点 → 打开上位机 → 连接设备。仅首次或热点变更时，通过 USB 串口配网；成功后设备保存配置并自动重连。");
        hint->setWordWrap(true); layout->addWidget(hint); telemetry->setObjectName("telemetry"); layout->addWidget(telemetry);
        auto splitter = new QSplitter; auto chatBox = new QGroupBox("对话"); auto chat = new QVBoxLayout(chatBox);
        history->setReadOnly(true); history->setMaximumBlockCount(1500);
        history->setPlaceholderText("连接后输入问题，或开启语音唤醒。回答会同步至开发板屏幕并播报。"); chat->addWidget(history);
        auto input = new QHBoxLayout; question->setMaxLength(96); question->setPlaceholderText("例如：西安现在天气怎么样？");
        send->setObjectName("primary"); input->addWidget(question, 1); input->addWidget(send); chat->addLayout(input);
        auto actions = new QHBoxLayout;
        for (auto b : {record, listen, home, probe}) actions->addWidget(b); chat->addLayout(actions); splitter->addWidget(chatBox);
        auto previewBox = new QGroupBox("设备屏幕点阵预览"); auto pv = new QVBoxLayout(previewBox);
        preview->setMinimumSize(240, 208); preview->setAlignment(Qt::AlignCenter); preview->setObjectName("preview"); pv->addWidget(preview, 1);
        preview->setToolTip("最近提交的回答图像；实际画面请以 LCD 为准。");
        splitter->addWidget(previewBox); splitter->setSizes({790, 280}); layout->addWidget(splitter, 1);
        auto settings = new QFormLayout; auto folder = new QHBoxLayout; folder->addWidget(voiceDir); folder->addWidget(browse);
        settings->addRow("语音组件", folder); settings->addRow("Python 程序", python); layout->addLayout(settings);
        log->setReadOnly(true); log->setMaximumBlockCount(500); log->setMaximumHeight(110); layout->addWidget(log);
        setStyleSheet(R"(
            QWidget { font-family: 'Microsoft YaHei'; font-size: 13px; color: #213c4a; }
            QMainWindow { background: #edf4f6; }
            QLabel#title { font-size: 25px; font-weight: 600; color: #16465b; }
            QGroupBox { background: white; border: 1px solid #c8dce3; border-radius: 8px; margin-top: 12px; padding: 16px 10px 10px; }
            QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; }
            QLineEdit, QComboBox, QSpinBox { background: white; padding: 7px; border: 1px solid #afc9d2; border-radius: 5px; }
            QPushButton { padding: 8px 12px; background: #e1edf1; border: 1px solid #b6cfd8; border-radius: 5px; }
            QPushButton:hover { background: #cce4eb; }
            QPushButton#primary { background: #176b87; color: white; border-color: #176b87; }
            QPushButton:disabled { color: #7d919a; background: #e9eff1; border-color: #d9e3e7; }
            QPlainTextEdit { background: white; border: 1px solid #d4e2e7; border-radius: 5px; padding: 8px; }
            QLabel#telemetry { background: #d9eaf0; padding: 12px; border-left: 4px solid #176b87; }
            QLabel#preview { background: #132e3d; color: #adc6d2; border-radius: 5px; }
        )");
        connect(refresh, &QPushButton::clicked, this, [this]{ refreshPorts(); });
        connect(mode, &QComboBox::currentIndexChanged, this, [this]{ pendingConnect = false; controls(); if (mode->currentIndex() == 1) startDiscovery(); else discovery->stop(); });
        connect(findDevice, &QPushButton::clicked, this, [this]{ startDiscovery(); });
        connect(discovery, &DeviceDiscovery::found, this, [this](QString address, quint16 tcpPort, QString id){
            if (worker || mode->currentIndex() != 1) return;
            host->setText(address); port->setValue(tcpPort);
            connection->setText("已发现 " + id + " · 点击连接设备"); appendLog("已发现设备：" + id + "（" + address + "）");
            discovery->stop();
            if (pendingConnect) { pendingConnect = false; connectDevice(); }
        });
        connect(discovery, &DeviceDiscovery::error, this, [this](QString text){ appendLog(text); });
        connect(discovery, &DeviceDiscovery::finished, this, [this]{
            if (!worker && host->text().isEmpty()) { connection->setText("未发现设备 · 检查电脑热点，首次使用请串口配网"); appendLog("未发现设备，请确认热点已开启；首次或热点变更时选择 USB 串口配网。"); }
            pendingConnect = false;
        });
        connect(connectButton, &QPushButton::clicked, this, [this]{ if (worker) disconnectDevice(); else connectDevice(); });
        connect(browse, &QPushButton::clicked, this, [this]{ auto path = QFileDialog::getExistingDirectory(this, "选择 ESP32VoiceAI 目录", voiceDir->text()); if (!path.isEmpty()) voiceDir->setText(path); });
        connect(send, &QPushButton::clicked, this, [this]{ sendText(); }); connect(question, &QLineEdit::returnPressed, this, [this]{ sendText(); });
        connect(home, &QPushButton::clicked, this, [this]{ submit("home"); }); connect(record, &QPushButton::clicked, this, [this]{ submit("record"); });
        connect(probe, &QPushButton::clicked, this, [this]{ submit("probe"); });
        connect(upgrade, &QPushButton::clicked, this, [this]{
            auto path = QFileDialog::getOpenFileName(this, "选择 STM32 升级包", {}, "WeatherClock 固件 (*.wcota)");
            if (path.isEmpty()) return;
            if (QMessageBox::question(this, "升级 WeatherClock",
                "请按住开发板的 K3，再点击“是”。看到开始传输后可松开 K3，安装期间请保持供电。",
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
            upgrading = true;
            upgradeProgress->setValue(0); upgradeProgress->show();
            upgradeStage->setText("正在检查升级包"); upgradeStage->show();
            submit("ota", path);
        });
        connect(joinWifi, &QPushButton::clicked, this, [this]{
            auto ssid = hotspot->text().toUtf8(), pass = password->text().toUtf8();
            if (ssid.isEmpty() || ssid.size() > 32 || ssid.contains('\0') || pass.contains('\0') || (pass.size() && (pass.size() < 8 || pass.size() > 63))) {
                appendLog("热点名称须为 1–32 字节，密码须为 8–63 字节（开放热点留空）"); return;
            }
            auto config = QJsonDocument(QJsonObject{{"ssid", hotspot->text()}, {"password", password->text()}}).toJson(QJsonDocument::Compact);
            submit("join-wifi", QString::fromUtf8(config)); password->clear();
        });
        connect(listen, &QPushButton::clicked, this, [this]{ if (isListening) { worker->stopListening(); appendLog("正在停止唤醒，当前问答完成后退出"); } else submit("listen"); });
        refreshPorts(); controls();
        QTimer::singleShot(0, this, [this]{ mode->setCurrentIndex(1); });
        auto autoFind = new QTimer(this); autoFind->setInterval(15000);
        connect(autoFind, &QTimer::timeout, this, [this]{ if (!worker && mode->currentIndex() == 1 && host->text().isEmpty() && !discovery->isActive()) startDiscovery(); }); autoFind->start();
    }
    void startDiscovery() {
        if (worker || mode->currentIndex() != 1) return;
        host->clear(); connection->setText("正在自动查找设备…"); discovery->start();
    }
    void appendLog(const QString &s) { log->appendPlainText(QTime::currentTime().toString("HH:mm:ss  ") + s); }
    void refreshPorts() { auto selected = serial->currentText(); serial->clear(); serial->addItems(DeviceWorker::serialPorts()); if (serial->findText(selected) >= 0) serial->setCurrentText(selected); }
    void controls() {
        bool active = worker != nullptr, wifi = mode->currentIndex() == 1, idle = online && !working;
        for (QWidget *w : QList<QWidget *>{mode, voiceDir, browse, python}) w->setEnabled(!active);
        serial->setEnabled(!active && !wifi); refresh->setEnabled(!active && !wifi); host->setEnabled(!active && wifi); port->setEnabled(!active && wifi);
        findDevice->setEnabled(!active && wifi);
        for (QWidget *w : QList<QWidget *>{question, send, record, home, probe}) w->setEnabled(idle && !bootMode);
        upgrade->setEnabled(idle && wifi && canUpgrade);
        connectButton->setEnabled(!upgrading);
        for (QWidget *w : QList<QWidget *>{hotspot, password, joinWifi}) w->setEnabled(idle && !wifi);
        listen->setEnabled((idle && !bootMode) || isListening); listen->setText(isListening ? "停止语音唤醒" : "开启语音唤醒"); connectButton->setText(active ? "断开连接" : "连接设备");
    }
    void connectDevice() {
        bool wifi = mode->currentIndex() == 1;
        if (!wifi && serial->currentText().isEmpty()) { appendLog("请选择外置 C3 的串口"); return; }
        if (wifi && host->text().isEmpty()) { pendingConnect = true; if (!discovery->isActive()) startDiscovery(); appendLog("正在查找设备，找到后将自动连接。"); return; }
        discovery->stop();
        Config c; c.serial = serial->currentText(); c.host = wifi ? host->text().trimmed() : QString(); c.port = quint16(port->value()); c.voiceDir = voiceDir->text(); c.python = python->text().trimmed();
        worker = new DeviceWorker(c); connection->setText("正在连接设备…");
        connect(worker, &DeviceWorker::connected, this, [this, wifi]{ online = true; connection->setText(wifi ? "已连接 · Wi-Fi / TCP" : "已连接 · USB 串口 460800"); controls(); });
        connect(worker, &DeviceWorker::message, this, [this](QString s){ appendLog(s); if (upgrading) upgradeStage->setText(s); });
        connect(worker, &DeviceWorker::firmwareMode, this, [this](bool boot, bool supported){
            bootMode = boot; canUpgrade = supported; controls();
        });
        connect(worker, &DeviceWorker::upgradeProgress, this, [this](int progress, const QString &stage){
            upgradeProgress->setValue(progress); upgradeStage->setText(stage);
        });
        connect(worker, &DeviceWorker::taskDone, this, [this](const QString &task){
            if (task == "ota") { upgrading = false; controls(); }
        });
        connect(worker, &DeviceWorker::networkReady, this, [this](QString address){ host->setText(address); appendLog("配置已保存。断开串口并切换 Wi-Fi；以后上电自动重连，上位机自动查找。"); });
        connect(worker, &DeviceWorker::busy, this, [this](bool b){ working = b; controls(); });
        connect(worker, &DeviceWorker::listening, this, [this](bool b){ isListening = b; controls(); });
        connect(worker, &DeviceWorker::state, this, [this](QVariantMap s){
            int valid = s["valid"].toInt();
            auto value = [&](QString key, int mask, QString unit){ return (valid & mask) ? QString::number(s[key].toInt()/10., 'f', 1) + unit : QString("暂无数据"); };
            telemetry->setText("页面 " + s["page"].toString() + "     室内 " + value("temp10", 4, " °C") + "     湿度 " + value("humidity10", 4, "%") + "     西安室外 " + value("outdoor10", 8, " °C") + "   · 最近有效值");
        });
        connect(worker, &DeviceWorker::answer, this, [this](QString q, QString a, QByteArray bitmap){
            history->appendPlainText("助手：" + a + "\n");
            QImage image(reinterpret_cast<const uchar *>(bitmap.constData()), 216, 208, 27, QImage::Format_Mono);
            image.setColorTable({qRgb(0,0,0), qRgb(255,255,255)}); preview->setPixmap(QPixmap::fromImage(image.copy()));
        });
        connect(worker, &QThread::finished, this, [this]{ worker->deleteLater(); worker = nullptr;
            online = working = isListening = bootMode = canUpgrade = upgrading = false;
            connection->setText("未连接 · 可重新选择连接方式"); connectButton->setEnabled(true); controls(); if (closing) close();
            else if (mode->currentIndex() == 1) QTimer::singleShot(0, this, [this]{ startDiscovery(); }); });
        worker->start(); controls();
    }
    void disconnectDevice() { online = false; worker->disconnectDevice(); connection->setText("正在结束当前操作并断开…"); controls(); connectButton->setEnabled(false); }
    void submit(const QString &cmd, const QString &value = {}) { if (worker && online && !working) { working = true; controls(); worker->submit(cmd, value); } }
    void sendText() { auto q = question->text().trimmed(); if (!q.isEmpty() && online && !working) { history->appendPlainText("我：" + q); submit("text", q); question->clear(); } }
    void closeEvent(QCloseEvent *e) override {
        if (upgrading) { e->ignore(); return; }
        if (worker) { closing = true; disconnectDevice(); e->ignore(); } else e->accept();
    }
};

/* [阅读说明]
 * 根据命令行参数选择自动化入口或图形界面；正常双击由 Start-WeatherClock.cmd 启动本程序。
 */
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei", 10));
    auto args = app.arguments();
    auto value = [&](const QString &name, const QString &fallback = QString()){ int i = args.indexOf(name); return i >= 0 && i+1 < args.size() ? args[i+1] : fallback; };
    if (args.contains("--discover")) {
        DeviceDiscovery discovery; QJsonArray devices;
        auto save = [&](int result){
            if (!value("--result").isEmpty()) { QFile f(value("--result")); if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(QJsonObject{{"devices", devices}}).toJson()); }
            app.exit(result);
        };
        QObject::connect(&discovery, &DeviceDiscovery::found, &app, [&](QString address, quint16 port, QString id){ devices.append(QJsonObject{{"address", address}, {"port", port}, {"id", id}}); discovery.stop(); save(0); });
        QObject::connect(&discovery, &DeviceDiscovery::finished, &app, [&]{ save(devices.isEmpty() ? 1 : 0); });
        QList<QHostAddress> targets; if (!value("--discovery-target").isEmpty()) targets << QHostAddress(value("--discovery-target"));
        QTimer::singleShot(0, &app, [&]{ discovery.start(targets, value("--discovery-port", "8766").toUShort()); });
        return app.exec();
    }
    if (args.contains("--check-host") || args.contains("--check-serial")) {
        Config c; c.host = value("--check-host"); c.serial = value("--check-serial"); c.port = value("--port", "8765").toUShort();
        c.voiceDir = value("--voice-dir"); c.python = value("--python", "python"); DeviceWorker worker(c); bool success = false;
        const auto upgradeFile = value("--ota-file");
        const bool taskRequested = args.contains("--probe") || !upgradeFile.isEmpty();
        QJsonArray messages; QJsonObject lastState;
        QObject::connect(&worker, &DeviceWorker::message, &app, [&](QString s){ messages.append(s); QTextStream(stdout) << s << Qt::endl; });
        QObject::connect(&worker, &DeviceWorker::state, &app, [&](QVariantMap s){ lastState = QJsonObject::fromVariantMap(s); if (!taskRequested) { success = true; worker.disconnectDevice(); } });
        QObject::connect(&worker, &DeviceWorker::connected, &app, [&]{
            if (!upgradeFile.isEmpty()) worker.submit("ota", upgradeFile);
            else if (args.contains("--probe")) worker.submit("probe");
        });
        QObject::connect(&worker, &DeviceWorker::taskDone, &app, [&](QString cmd){ if (cmd == "probe" || cmd == "ota") success = true; });
        QObject::connect(&worker, &DeviceWorker::busy, &app, [&](bool busy){ if (taskRequested && !busy && success) worker.disconnectDevice(); });
        QObject::connect(&worker, &QThread::finished, &app, [&]{
            if (!value("--result").isEmpty()) {
                QFile f(value("--result")); if (f.open(QIODevice::WriteOnly))
                    f.write(QJsonDocument(QJsonObject{{"ok", success}, {"state", lastState}, {"messages", messages}}).toJson());
            }
            app.exit(success ? 0 : 1);
        });
        QTimer::singleShot(upgradeFile.isEmpty() ? 30000 : 240000, &app, [&]{ worker.disconnectDevice(); }); worker.start(); int result = app.exec(); worker.disconnectDevice(); worker.wait(); return result;
    }
    Console window; window.show();
    if (args.contains("--screenshot")) QTimer::singleShot(500, &app, [&]{ bool saved = window.grab().save(value("--screenshot")); app.exit(saved ? 0 : 1); });
    return app.exec();
}
