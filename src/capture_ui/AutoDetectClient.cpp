#include "AutoDetectClient.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>

namespace flora::capture_ui {
AutoDetectClient::AutoDetectClient(QObject* parent):QObject(parent){
    deadline_.setSingleShot(true);
    connect(&process_,&QProcess::readyReadStandardOutput,this,&AutoDetectClient::consume);
    connect(&process_,&QProcess::readyReadStandardError,this,[this]{process_.readAllStandardError();});
    connect(&deadline_,&QTimer::timeout,this,[this]{reportedError_=true;emit failed("Auto-Detect manager timed out");process_.kill();});
    connect(&process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart){deadline_.stop();active_=false;reportedError_=true;emit failed(process_.errorString());emit stateChanged("Off");}
    });
    connect(&process_,&QProcess::finished,this,[this](int code,QProcess::ExitStatus status){
        consume();deadline_.stop();active_=false;
        if(!reportedError_ && (!stopped_ || code!=0 || status!=QProcess::NormalExit))emit failed("Auto-Detect manager exited unexpectedly");
        emit stateChanged("Off");
    });
}
AutoDetectClient::~AutoDetectClient(){
    stop();if(process_.state()!=QProcess::NotRunning && !process_.waitForFinished(5000)){process_.kill();process_.waitForFinished(2000);}
}
void AutoDetectClient::start(const QString& controller,const QString& output,const CaptureClient::HudSettings& hud,const QString& targetExe){
    if(active_){emit failed("Auto-Detect is already active");return;}
    buffer_.clear();stopped_=reportedError_=false;active_=true;emit stateChanged("Starting");
    QStringList arguments{"monitor","--out",output,"--hud",hud.mode,"--hud-hotkey",hud.modeKey,"--capture-hotkey",hud.captureKey,
        "--owner-pid",QString::number(QCoreApplication::applicationPid())};
    if(!targetExe.trimmed().isEmpty())arguments<<"--target-exe"<<QFileInfo(targetExe.trimmed()).absoluteFilePath();
    process_.start(controller,arguments);deadline_.start(100000);
}
void AutoDetectClient::stop(){if(!active_ || stopped_)return;stopped_=true;process_.write("stop\n");deadline_.start(15000);emit stateChanged("Stopping");}
void AutoDetectClient::recover(const QString& controller){
    const auto root=qEnvironmentVariable("ProgramW6432",qEnvironmentVariable("ProgramFiles"));
    if(active_ || root.isEmpty() || !QFileInfo::exists(root+"/FloraGPA/AutoDetect/recovery.json"))return;
    buffer_.clear();stopped_=reportedError_=false;active_=true;emit stateChanged("Recovering");
    process_.start(controller,{"monitor-recover"});deadline_.start(100000);
}
void AutoDetectClient::consume(){
    buffer_+=process_.readAllStandardOutput();
    if(buffer_.size()>1024*1024){reportedError_=true;emit failed("Auto-Detect response is too large");process_.kill();return;}
    while(buffer_.contains('\n')){
        const auto end=buffer_.indexOf('\n');const auto line=buffer_.left(end);buffer_.remove(0,end+1);
        QJsonParseError error{};const auto document=QJsonDocument::fromJson(line,&error);const auto message=document.object();
        if(error.error!=QJsonParseError::NoError || !document.isObject() || message.value("schema").toInt()!=1 || !message.value("ok").isBool()){
            reportedError_=true;emit failed("Invalid Auto-Detect response");process_.kill();return;
        }
        const auto kind=message.value("event").toString();
        if(kind=="error" || !message.value("ok").toBool()){reportedError_=true;emit failed(message.value("error").toString("Auto-Detect failed"));}
        else if(kind=="ready"){if(!stopped_)emit stateChanged("Watching");deadline_.start(10000);}
        else if(kind=="heartbeat"){if(!stopped_)deadline_.start(10000);}
        else if(kind=="stopped"){stopped_=true;deadline_.stop();}
        else if(kind=="attachment_progress"){
            if(!stopped_){emit stateChanged(message.value("state").toString());emit attachmentProgress(message);}
        }
        else if(kind=="attachment_failed"){
            emit failed(QString("%1 · %2: %3").arg(QFileInfo(message.value("executable").toString()).fileName()).arg(message.value("pid").toInteger()).arg(message.value("error").toString()));
        }else if(kind=="session"){
            const auto manifest=message.value("session").toObject();
            const QFileInfo file(manifest.value("session_file").toString());
            if(manifest.value("version").toInt()!=4 || manifest.value("attachment")!="auto_detect" || !file.isAbsolute() || !file.isFile()){
                emit failed("Invalid detected session manifest");continue;
            }
            emit sessionFound(manifest);
        }else{reportedError_=true;emit failed("Unknown Auto-Detect event");process_.kill();return;}
    }
}
}
