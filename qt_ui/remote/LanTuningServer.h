#pragma once
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QElapsedTimer>
#include <QJsonObject>

class QTcpServer;
class QTcpSocket;

class LanTuningServer : public QObject {
    Q_OBJECT
public:
    explicit LanTuningServer(QObject* parent = nullptr);
    ~LanTuningServer() override;
    bool start(quint16 port, QString& error);
    void stop();
    bool running() const;
    QStringList urls() const;
signals:
    void activity(const QString& text);
private:
    void acceptConnections();
    void readRequest(QTcpSocket* socket);
    void reply(QTcpSocket* socket, int status, const QByteArray& body,
               const QByteArray& type = "application/json; charset=utf-8");
    QJsonObject state();
    void apply(QTcpSocket* socket, const QJsonObject& request);
    QTcpServer* server_{};
    QSet<QTcpSocket*> sockets_;
    QByteArray token_;
    QElapsedTimer lastWrite_;
};
