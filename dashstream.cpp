// main.cpp
#include <QApplication>
#include <QMainWindow>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QRandomGenerator>
#include <QFontDatabase>
#include <QScreen>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QCloseEvent>

// Helper clamp
template<typename T>
static T clampT(T v, T lo, T hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }

// --- Classe para o Widget de Sensor ---
class SensorWidget : public QFrame
{
    Q_OBJECT
public:
    explicit SensorWidget(const QString &name, const QString &unit, const QString &color, QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setFrameShape(QFrame::StyledPanel);
        setObjectName("sensorFrame");

        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setSpacing(5);
        layout->setContentsMargins(10, 10, 10, 10);

        QHBoxLayout *headerLayout = new QHBoxLayout();
        QLabel* nameLabel = new QLabel(name.toUpper());
        nameLabel->setObjectName("sensorName");
        QLabel *unitLabel = new QLabel(unit);
        unitLabel->setObjectName("sensorUnit");

        headerLayout->addWidget(nameLabel);
        headerLayout->addStretch();
        headerLayout->addWidget(unitLabel);

        valueLabel = new QLabel("0.0");
        valueLabel->setObjectName("sensorValue");
        valueLabel->setAlignment(Qt::AlignCenter);

        progressBar = new QProgressBar();
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBar->setTextVisible(false);
        progressBar->setStyleSheet(QString(
            "QProgressBar { background-color: #2c3e50; border: none; border-radius: 4px; height: 8px; }"
            "QProgressBar::chunk { background-color: %1; border-radius: 4px; }"
        ).arg(color));

        layout->addLayout(headerLayout);
        layout->addWidget(valueLabel);
        layout->addWidget(progressBar);
    }

    void setValue(double value) {
        valueLabel->setText(QString::number(value, 'f', 1));
        // progressBar expects 0..100; assume input scaled appropriately
        int v = static_cast<int>(value);
        if (v < 0) v = 0;
        if (v > progressBar->maximum()) {
            // If value bigger than 100, clamp to max for progress; not all sensors use 0-100 but it's visual
            v = progressBar->maximum();
        }
        progressBar->setValue(v);
    }

private:
    QLabel *valueLabel;
    QProgressBar *progressBar;
};

// --- Classe Principal do Dashboard ---
class Dashboard : public QMainWindow
{
    Q_OBJECT

public:
    Dashboard(QWidget *parent = nullptr);
    ~Dashboard();

protected:
    void closeEvent(QCloseEvent *ev) override;

private slots:
    void updateData();                     // periodic update (simulation or refresh)
    void onTextMessageReceived(const QString &message);
    void onWsConnected();
    void onWsDisconnected();
    void attemptReconnect();

private:
    void setupUI();
    void applyStyles();
    void processJsonMessage(const QJsonObject &obj);
    void startSimulation();
    void stopSimulation();

    QLabel *rpmCentralLabel;
    QProgressBar *rpmTopBar;
    QLabel *gearLabel;
    QLabel *speedLabel;

    SensorWidget *mapSensor;
    SensorWidget *tpsSensor;
    SensorWidget *oilPressureSensor;
    SensorWidget *fuelPressureSensor;
    SensorWidget *batterySensor;
    SensorWidget *oilTempSensor;

    QTimer *timer;             // UI update timer (30ms)
    QTimer *reconnectTimer;    // reconnection attempts

    QWebSocket ws;
    QUrl wsUrl = QUrl("ws://localhost:9090");

    // simulation
    bool m_simulationMode = false;
    bool m_isSweepAnimationRunning = true;
    int m_sweepDirection = 1;
    double m_sweepRpm = 800;

    int reconnectAttempts = 0;
};

Dashboard::Dashboard(QWidget *parent)
    : QMainWindow(parent)
{
    setupUI();
    applyStyles();

    setMinimumSize(800, 480);

    // connect websocket signals
    connect(&ws, &QWebSocket::connected, this, &Dashboard::onWsConnected);
    connect(&ws, &QWebSocket::disconnected, this, &Dashboard::onWsDisconnected);
    connect(&ws, &QWebSocket::textMessageReceived, this, &Dashboard::onTextMessageReceived);

    reconnectTimer = new QTimer(this);
    reconnectTimer->setInterval(3000);
    reconnectTimer->setSingleShot(false);
    connect(reconnectTimer, &QTimer::timeout, this, &Dashboard::attemptReconnect);

    // try connect immediately
    ws.open(wsUrl);
    reconnectTimer->start();

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Dashboard::updateData);
    timer->start(30); // UI refresh / simulation tick
}

Dashboard::~Dashboard() {}

void Dashboard::closeEvent(QCloseEvent *ev) {
    ws.close();
    QMainWindow::closeEvent(ev);
}

void Dashboard::setupUI()
{
    setWindowTitle("Dashboard Pro V2");

    QWidget *centralWidget = new QWidget;
    centralWidget->setObjectName("centralWidget");
    setCentralWidget(centralWidget);

    QGridLayout *mainLayout = new QGridLayout(centralWidget);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    // RPM TOP BAR
    rpmTopBar = new QProgressBar();
    rpmTopBar->setRange(0, 11000);
    rpmTopBar->setValue(0);
    rpmTopBar->setTextVisible(false);
    rpmTopBar->setObjectName("rpmTopBar");
    mainLayout->addWidget(rpmTopBar, 0, 0, 1, 3);

    // Central display frame
    QFrame *centralDisplayFrame = new QFrame();
    centralDisplayFrame->setObjectName("centralDisplay");
    QVBoxLayout *centralDisplayLayout = new QVBoxLayout(centralDisplayFrame);
    centralDisplayLayout->setSpacing(0);
    centralDisplayLayout->addStretch();

    rpmCentralLabel = new QLabel("800");
    rpmCentralLabel->setObjectName("rpmCentralLabel");
    rpmCentralLabel->setAlignment(Qt::AlignCenter);
    rpmCentralLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    speedLabel = new QLabel("0");
    speedLabel->setObjectName("speedLabel");
    speedLabel->setAlignment(Qt::AlignCenter);
    speedLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QLabel* speedUnitLabel = new QLabel("km/h");
    speedUnitLabel->setObjectName("speedUnitLabel");
    speedUnitLabel->setAlignment(Qt::AlignCenter);

    centralDisplayLayout->addWidget(rpmCentralLabel);
    centralDisplayLayout->addWidget(speedLabel);
    centralDisplayLayout->addWidget(speedUnitLabel);
    centralDisplayLayout->addStretch();

    gearLabel = new QLabel("N");
    gearLabel->setObjectName("gearLabel");
    gearLabel->setAlignment(Qt::AlignCenter);
    gearLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QHBoxLayout* bottomLayout = new QHBoxLayout();
    bottomLayout->addStretch();
    bottomLayout->addWidget(gearLabel);
    bottomLayout->addStretch();
    centralDisplayLayout->addLayout(bottomLayout);

    mainLayout->addWidget(centralDisplayFrame, 1, 1, 2, 1);

    // SENSORS
    mapSensor = new SensorWidget("MAP", "kPa", "#f39c12");
    tpsSensor = new SensorWidget("TPS", "%", "#2ecc71");
    batterySensor = new SensorWidget("BATERIA", "V", "#f1c40f");
    oilPressureSensor = new SensorWidget("ÓLEO P.", "bar", "#e74c3c");
    fuelPressureSensor = new SensorWidget("COMB P.", "bar", "#3498db");
    oilTempSensor = new SensorWidget("ÓLEO T.", "°C", "#9b59b6");

    QVBoxLayout *leftSensors = new QVBoxLayout();
    leftSensors->setSpacing(15);
    leftSensors->addWidget(mapSensor);
    leftSensors->addWidget(tpsSensor);
    leftSensors->addWidget(batterySensor);
    mainLayout->addLayout(leftSensors, 1, 0);

    QVBoxLayout *rightSensors = new QVBoxLayout();
    rightSensors->setSpacing(15);
    rightSensors->addWidget(oilPressureSensor);
    rightSensors->addWidget(fuelPressureSensor);
    rightSensors->addWidget(oilTempSensor);
    mainLayout->addLayout(rightSensors, 1, 2);

    // Exit button
    QPushButton *exitButton = new QPushButton("SAIR");
    exitButton->setObjectName("exitButton");
    connect(exitButton, &QPushButton::clicked, &QApplication::quit);
    mainLayout->addWidget(exitButton, 3, 2, Qt::AlignRight | Qt::AlignBottom);

    mainLayout->setColumnStretch(0, 1);
    mainLayout->setColumnStretch(1, 3);
    mainLayout->setColumnStretch(2, 1);
    mainLayout->setRowStretch(1, 1);
}

void Dashboard::applyStyles()
{
    this->setStyleSheet(R"(
        QWidget#centralWidget { background-color: #1a1a1a; }
        QFrame#sensorFrame { background-color: #262626; border: 1px solid #333; border-radius: 8px; }
        QFrame#centralDisplay { border: none; }

        QProgressBar#rpmTopBar {
            border: 1px solid #333; border-radius: 8px;
            height: 40px;
            background-color: #262626;
        }
        QProgressBar#rpmTopBar::chunk {
            border-radius: 6px;
            background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2ecc71, stop:1 #27ae60);
        }

        QLabel#rpmCentralLabel {
            font-weight: bold;
            font-size: 120px;
            color: white;
        }
        QLabel#speedLabel {
            font-weight: bold;
            font-size: 48px;
            color: #3498db;
        }
        QLabel#speedUnitLabel {
            color: #7f8c8d;
            font-size: 20px;
            font-weight: bold;
            padding-bottom: 20px;
        }
        QLabel#gearLabel {
            color: white;
            font-size: 28px;
            font-weight: bold;
            border: 3px solid #3498db;
            border-radius: 15px;
            padding: 10px;
            min-width: 100px;
        }

        QLabel#sensorName { color: #ecf0f1; font-size: 14px; font-weight: bold; }
        QLabel#sensorUnit { color: #7f8c8d; font-size: 12px; }
        QLabel#sensorValue { color: white; font-size: 22px; font-weight: bold; }

        QPushButton#exitButton {
            background-color: #c0392b; color: white;
            font-weight: bold; font-size: 14px;
            border: none; border-radius: 5px;
            padding: 8px 16px;
        }
        QPushButton#exitButton:pressed { background-color: #e74c3c; }
    )");
}

void Dashboard::onWsConnected()
{
    reconnectAttempts = 0;
    m_simulationMode = false;
    qDebug("WebSocket conectado a %s", qPrintable(wsUrl.toString()));
}

void Dashboard::onWsDisconnected()
{
    qDebug("WebSocket desconectado");
    // reconnection attempts handled by reconnectTimer
}

void Dashboard::attemptReconnect()
{
    if (ws.state() == QAbstractSocket::ConnectedState ||
        ws.state() == QAbstractSocket::ConnectingState) {
        return;
    }
    reconnectAttempts++;
    qDebug("Tentativa de reconexão #%d ao %s", reconnectAttempts, qPrintable(wsUrl.toString()));
    ws.open(wsUrl);

    // after a few failed attempts, enable simulation fallback
    if (reconnectAttempts >= 5) {
        qDebug("Ativando modo de simulação (nenhuma conexão)");
        startSimulation();
    }
}

void Dashboard::onTextMessageReceived(const QString &message)
{
    // Parse JSON
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning("Mensagem JSON inválida recebida: %s", qPrintable(err.errorString()));
        return;
    }
    QJsonObject obj = doc.object();
    processJsonMessage(obj);
}

void Dashboard::processJsonMessage(const QJsonObject &obj)
{
    // Whenever we receive a real message, stop simulation mode
    stopSimulation();

    if (obj.contains("rpm")) {
        int rpm = obj.value("rpm").toInt();
        rpmTopBar->setValue(clampT<int>(rpm, 0, rpmTopBar->maximum()));
        rpmCentralLabel->setText(QString::number(rpm));
    }
    if (obj.contains("speed")) {
        int sp = obj.value("speed").toInt();
        speedLabel->setText(QString::number(sp));
    }
    if (obj.contains("tps")) {
        double v = obj.value("tps").toDouble();
        tpsSensor->setValue(clampT<double>(v, 0.0, 100.0));
    }
    if (obj.contains("map")) {
        double v = obj.value("map").toDouble();
        // assume map typical range 0-200 kPa -> scale to 0-100 for progress
        mapSensor->setValue(clampT<double>(v, 0.0, 200.0) * 100.0 / 200.0);
    }
    if (obj.contains("battery")) {
        double v = obj.value("battery").toDouble();
        // 12-15 V -> map to 0-100
        batterySensor->setValue(clampT<double>( (v - 9.0) * (100.0/6.0), 0.0, 100.0));
    }
    if (obj.contains("coolant")) {
        double v = obj.value("coolant").toDouble();
        // coolant -> oilTemp sensor
        oilTempSensor->setValue(clampT<double>(v, -40.0, 200.0));
    }
    if (obj.contains("oil_pressure")) {
        double v = obj.value("oil_pressure").toDouble();
        // 0-10 bar -> scale 0-100
        oilPressureSensor->setValue(clampT<double>(v * 10.0, 0.0, 100.0));
    }
    if (obj.contains("fuel_pressure")) {
        double v = obj.value("fuel_pressure").toDouble();
        fuelPressureSensor->setValue(clampT<double>(v * 10.0, 0.0, 100.0));
    }
}

void Dashboard::startSimulation()
{
    m_simulationMode = true;
    m_isSweepAnimationRunning = true;
    m_sweepDirection = 1;
    m_sweepRpm = 800;
}

void Dashboard::stopSimulation()
{
    if (m_simulationMode) {
        m_simulationMode = false;
        reconnectAttempts = 0;
    }
}

void Dashboard::updateData()
{
    if (m_simulationMode) {
        // Sweep RPM animation until finished then normal fake driving
        double rpm;
        if (m_isSweepAnimationRunning) {
            if (m_sweepDirection == 1) {
                m_sweepRpm += 300;
                if (m_sweepRpm >= 10500) { m_sweepRpm = 10500; m_sweepDirection = -1; }
            } else {
                m_sweepRpm -= 300;
                if (m_sweepRpm <= 800) { m_sweepRpm = 800; m_isSweepAnimationRunning = false; }
            }
            rpm = m_sweepRpm;
            speedLabel->setText("0");
            gearLabel->setText("N");
            mapSensor->setValue(0); tpsSensor->setValue(0);
            oilPressureSensor->setValue(0); fuelPressureSensor->setValue(0);
            batterySensor->setValue(0); oilTempSensor->setValue(0);
        } else {
            static double current_rpm = 800;
            static double speed = 0;
            static int gear = 0;

            // grow rpm slowly
            double step = QRandomGenerator::global()->bounded(100.0, 400.0);
            current_rpm += step;
            if (current_rpm > 5000) {
                current_rpm = 1200 + QRandomGenerator::global()->bounded(2000);
                if (gear < 6) gear++;
            }
            speed = (current_rpm / 110.0) * (gear * 0.6) + QRandomGenerator::global()->bounded(5);
            if (gear == 0) { speed = 0; current_rpm = 800 + QRandomGenerator::global()->bounded(100); }

            rpm = current_rpm;
            speedLabel->setText(QString::number(static_cast<int>(speed)));
            gearLabel->setText(gear == 0 ? "N" : QString::number(gear));
            mapSensor->setValue( clampT<double>(80 + QRandomGenerator::global()->bounded(20.0), 0.0, 100.0) );
            tpsSensor->setValue( clampT<double>((rpm / 110.0) + QRandomGenerator::global()->bounded(5.0), 0.0, 100.0) );
            oilPressureSensor->setValue( clampT<double>(3.5 + (rpm / 5000.0) + QRandomGenerator::global()->boundedDouble(), 0.0, 100.0) );
            fuelPressureSensor->setValue( clampT<double>(3.0 + QRandomGenerator::global()->generateDouble() / 2.0, 0.0, 100.0) );
            batterySensor->setValue( clampT<double>(13.8 + QRandomGenerator::global()->generateDouble() / 2.0, 0.0, 100.0) );
            oilTempSensor->setValue( clampT<double>(90 + (rpm / 1000) + QRandomGenerator::global()->bounded(5.0), -40.0, 200.0) );
        }

        rpmTopBar->setValue(static_cast<int>(rpm));
        rpmCentralLabel->setText(QString::number(static_cast<int>(rpm)));
    } else {
        // not simulation: UI is updated when JSON messages arrive
        // we still can update styles based on current rpm label
        bool ok;
        int rpm = rpmCentralLabel->text().toInt(&ok);
        if (ok) {
            if (rpm > 9500) {
                rpmCentralLabel->setStyleSheet("color: #ff3838;");
                rpmTopBar->setStyleSheet("QProgressBar#rpmTopBar::chunk { background-color: #ff3838; }");
            } else if (rpm > 7000) {
                rpmCentralLabel->setStyleSheet("color: #f1c40f;");
                rpmTopBar->setStyleSheet("QProgressBar#rpmTopBar::chunk { background-color: #f1c40f; }");
            } else {
                rpmCentralLabel->setStyleSheet("color: white;");
                rpmTopBar->setStyleSheet("QProgressBar#rpmTopBar::chunk { background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2ecc71, stop:1 #27ae60); }");
            }
        }
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    Dashboard window;
    window.showFullScreen();
    return app.exec();
}

#include "main.moc"

