#include "CaptureDialog.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QAbstractItemView>

namespace flora::capture_ui {
CaptureDialog::CaptureDialog(const QString &controller, QWidget *parent) : QDialog(parent), client_(this) {
    controller_=controller;
    setWindowTitle("DX11 Capture"); resize(660,400); client_.setController(controller);
    auto layout = new QVBoxLayout(this); auto form = new QFormLayout;
    exe_ = new QLineEdit(this); exe_->setObjectName("captureExecutable");
    auto row = new QHBoxLayout; row->addWidget(exe_); auto browse = new QPushButton("Browse…",this); row->addWidget(browse);
    connect(browse,&QPushButton::clicked,this,[this] {
        const auto path=QFileDialog::getOpenFileName(this,"Launch DX11 application",exe_->text(),"Applications (*.exe)");
        if (!path.isEmpty()) { exe_->setText(path); cwd_->setText(QFileInfo(path).absolutePath()); }
    });
    form->addRow("Application",row);
    arguments_=new QLineEdit(this); arguments_->setObjectName("captureArguments"); arguments_->setToolTip("Use quotes around arguments containing spaces."); form->addRow("Arguments",arguments_);
    cwd_=new QLineEdit(this); cwd_->setObjectName("captureWorkingDirectory"); form->addRow("Working directory",cwd_);
    out_=new QLineEdit(QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(
        "FloraGPA/Captures/"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz")),this);
    out_->setObjectName("captureOutput");
    out_->setToolTip("A new session directory. Existing directories are preserved."); form->addRow("Output",out_);
    swaps_=new QComboBox(this); swaps_->setObjectName("captureSwapChains"); form->addRow("Swap chain",swaps_);
    resourceLimit_=new QComboBox(this);resourceLimit_->setObjectName("captureResourceLimit");
    for(unsigned gib:{1u,2u,4u})resourceLimit_->addItem(QString::number(gib)+" GiB",gib*1024);
    resourceLimit_->setToolTip("Resource budget for Capture Next Frame. Larger scenes may require 4 GiB. HUD hotkeys use the default 1 GiB budget.");
    form->addRow("Capture limit",resourceLimit_);
    layout->addLayout(form);
    auto detection=new QHBoxLayout;
    autoCheck_=new QCheckBox("Auto-Detect",this);autoCheck_->setObjectName("captureAutoDetect");
    autoCheck_->setToolTip("Detect new x64 DX11 applications launched from Steam or other launchers. Administrator access is required.");
    autoStatus_=new QLabel("Off",this);autoStatus_->setObjectName("captureAutoDetectStatus");
    detection->addWidget(autoCheck_);detection->addWidget(autoStatus_);detection->addStretch();layout->addLayout(detection);
    sessions_=new QComboBox(this);sessions_->setObjectName("captureSessions");sessions_->setPlaceholderText("Detected sessions");layout->addWidget(sessions_);
    auto settings=new QGroupBox("HUD settings",this);settings->setObjectName("captureHudSettings");settings->setCheckable(true);settings->setChecked(false);
    auto settingsLayout=new QVBoxLayout(settings);auto settingsBody=new QWidget(settings);auto hudForm=new QFormLayout(settingsBody);
    hudMode_=new QComboBox(settingsBody);hudMode_->setObjectName("captureHudMode");
    hudMode_->addItem("Compact","compact");hudMode_->addItem("Detailed","detailed");hudMode_->addItem("Off","off");
    hudKey_=new QLineEdit("Ctrl+F1",settingsBody);hudKey_->setObjectName("captureHudHotkey");
    captureKey_=new QLineEdit("Ctrl+Shift+C",settingsBody);captureKey_->setObjectName("captureFrameHotkey");
    for(auto field:{hudKey_,captureKey_})field->setToolTip("Ctrl, Shift or Alt plus A–Z, 0–9 or F1–F24. Enter off to disable.");
    hudForm->addRow("HUD",hudMode_);hudForm->addRow("Cycle HUD",hudKey_);hudForm->addRow("Capture frame",captureKey_);
    settingsLayout->addWidget(settingsBody);settingsBody->hide();connect(settings,&QGroupBox::toggled,settingsBody,&QWidget::setVisible);layout->addWidget(settings);
    status_=new QLabel("Ready to launch",this); status_->setObjectName("captureStatus"); status_->setWordWrap(true); layout->addWidget(status_);
    auto actions = new QHBoxLayout;
    launch_=new QPushButton("Launch",this); capture_=new QPushButton("Capture Next Frame",this); cancel_=new QPushButton("Cancel",this);
    disconnect_=new QPushButton("Disconnect",this); open_=new QPushButton("Open Capture",this);
    cancel_->setObjectName("captureCancel"); disconnect_->setObjectName("captureDisconnect");
    launch_->setObjectName("captureLaunch"); capture_->setObjectName("captureNext"); open_->setObjectName("captureOpen");
    for(auto button:{launch_,capture_,cancel_,disconnect_,open_})actions->addWidget(button);
    layout->addLayout(actions);
    auto details=new QGroupBox("Task log",this); details->setCheckable(true); details->setChecked(false);
    auto logs=new QVBoxLayout(details); log_=new QPlainTextEdit(this); log_->setReadOnly(true); log_->setMaximumBlockCount(300);
    log_->hide(); logs->addWidget(log_); connect(details,&QGroupBox::toggled,log_,&QWidget::setVisible); layout->addWidget(details);
    connect(launch_,&QPushButton::clicked,this,[this] {
        completed_.clear(); lastResult_.clear(); available_=false;
        client_.launch(exe_->text(),cwd_->text(),out_->text(),QProcess::splitCommand(arguments_->text()),{hudMode_->currentData().toString(),hudKey_->text(),captureKey_->text()});
    });
    connect(capture_,&QPushButton::clicked,this,[this] { client_.capture(swaps_->currentData().toString(),resourceLimit_->currentData().toUInt()); });
    connect(swaps_,&QComboBox::activated,this,[this](int){if(client_.connected() && !client_.busy())client_.selectSwap(swaps_->currentData().toString());});
    connect(cancel_,&QPushButton::clicked,&client_,&CaptureClient::cancel);
    connect(disconnect_,&QPushButton::clicked,&client_,&CaptureClient::disconnectSession);
    connect(open_,&QPushButton::clicked,this,[this] { if(QFileInfo(completed_).isFile())emit openCaptureRequested(completed_); });
    connect(&client_,&CaptureClient::busyChanged,this,[this] { updateControls(); });
    connect(&client_,&CaptureClient::response,this,&CaptureDialog::showResponse);
    connect(&client_,&CaptureClient::failed,this,[this](const QString &error) { status_->setText(error); log_->appendPlainText(error); updateControls(); });
    connect(autoCheck_,&QCheckBox::toggled,this,[this](bool checked){
        if(checked){
            const auto output=QDir(out_->text()).filePath("auto-"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz"));
            autoDetect_.start(controller_,output,{hudMode_->currentData().toString(),hudKey_->text(),captureKey_->text()});
        }else autoDetect_.stop();updateControls();
    });
    connect(&autoDetect_,&AutoDetectClient::stateChanged,this,[this](const QString& state){
        autoStatus_->setText(state);
        if(state=="Off"){QSignalBlocker block(autoCheck_);autoCheck_->setChecked(false);}updateControls();
    });
    connect(&autoDetect_,&AutoDetectClient::failed,this,[this](const QString& error){status_->setText(error);log_->appendPlainText(error);});
    connect(&autoDetect_,&AutoDetectClient::sessionFound,this,[this](const QJsonObject& manifest){
        const auto file=manifest.value("session_file").toString();if(sessions_->findData(file)>=0)return;
        sessions_->addItem(QFileInfo(manifest.value("executable").toString()).fileName()+" · "+QString::number(manifest.value("pid").toInteger()),file);
        log_->appendPlainText(QString::fromUtf8(QJsonDocument(manifest).toJson(QJsonDocument::Compact)));
        if(!client_.connected() && !client_.busy()){sessions_->setCurrentIndex(sessions_->count()-1);completed_.clear();lastResult_.clear();available_=false;client_.useSession(file);}
        updateControls();
    });
    connect(sessions_,&QComboBox::activated,this,[this](int index){
        if(client_.busy() || activeCapture_)return;
        completed_.clear();lastResult_.clear();lastHudRequest_.clear();available_=false;client_.useSession(sessions_->itemData(index).toString());
    });
    connect(&poll_,&QTimer::timeout,this,[this] {
        // Disabling a combo during a status request closes its popup. Let the
        // user finish selecting before the next background request.
        for(auto combo:{swaps_,sessions_,resourceLimit_})if(combo->view()->isVisible())return;
        if(client_.connected() && !client_.busy())client_.status();
    });
    poll_.start(1000); updateControls();
    QTimer::singleShot(0,this,[this]{autoDetect_.recover(controller_);});
}
void CaptureDialog::showResponse(const QString &command,const QJsonObject &result) {
    const auto state = command=="launch" ? result.value("status").toObject() : result;
    const auto phase=state.value("state").toString();
    activeCapture_=state.value("capture_progress").toObject().value("busy").toBool() || phase=="Armed" || phase=="Capturing" || phase=="Finalizing";
    available_=state.value("capture_available").toBool();
    if(!result.value("ok").toBool())status_->setText(result.value("error").toString("Capture request failed"));
    else status_->setText(available_?phase:phase+" — frame capture unavailable");
    QStringList gaps;for(const auto &gap:state.value("capability_gaps").toArray())gaps<<gap.toString();
    const auto hud=state.value("hud").toObject();
    for(const auto& row:hud.value("swap_chains").toArray()){const auto detail=row.toObject();if(!detail.value("visible").toBool() && !detail.value("reason").toString().isEmpty())gaps<<"HUD: "+detail.value("reason").toString();}
    const auto hudRequest=hud.value("last_request").toObject();if(!hudRequest.isEmpty()){const auto encoded=QString::fromUtf8(QJsonDocument(hudRequest).toJson(QJsonDocument::Compact));if(encoded!=lastHudRequest_){lastHudRequest_=encoded;log_->appendPlainText(encoded);}}
    status_->setToolTip(gaps.join('\n'));
    if(command!="status" || !result.value("ok").toBool())log_->appendPlainText(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    QList<QPair<QString,QString>> choices{{"Automatic (foreground)",QString()}};
    for(const auto &value:state.value("swap_chains").toObject().value("active").toArray()) {
        const auto swap=value.toObject(); const auto id=QString::number(swap.value("id").toInteger());
        choices.append({QString("%1 · %2 × %3").arg(id).arg(swap.value("width").toInt()).arg(swap.value("height").toInt()),id});
    }
    bool changed=swaps_->count()!=choices.size();
    for(int i=0;!changed && i<choices.size();++i)
        changed=swaps_->itemText(i)!=choices[i].first || swaps_->itemData(i).toString()!=choices[i].second;
    if(changed){
        const auto selected=swaps_->currentData().toString();QSignalBlocker block(swaps_);swaps_->clear();
        for(const auto& choice:choices)swaps_->addItem(choice.first,choice.second);
        const int index=swaps_->findData(selected);if(index>=0)swaps_->setCurrentIndex(index);
    }
    // Only an explicitly completed result may cross into replay.
    const auto completion=state.value("last_capture").toObject();
    if(!completion.isEmpty()) {
        const auto encoded=QString::fromUtf8(QJsonDocument(completion).toJson(QJsonDocument::Compact));
        if(encoded!=lastResult_){lastResult_=encoded;log_->appendPlainText(encoded);}
        const auto outcome=completion.value("state").toString();
        if(outcome=="Completed") {
            const auto path=completion.value("path").toString();const QFileInfo file(path);
            if(file.isAbsolute() && file.suffix()=="gpa_frame" && file.isFile()) {
                completed_=path;if(!activeCapture_ && result.value("ok").toBool())status_->setText("Capture completed");
            } else status_->setText("Invalid completed capture path");
        } else if(!activeCapture_ && result.value("ok").toBool()) {
            status_->setText(outcome=="Cancelled"?"Capture cancelled":"Capture failed");
            if(completion.contains("error"))status_->setToolTip(completion.value("error").toString());
        }
    }
    if(command=="disconnect" && result.value("ok").toBool())status_->setText("Disconnected");
    updateControls();
}
void CaptureDialog::updateControls() {
    const bool idle=!client_.busy();
    launch_->setEnabled(idle && !client_.connected() && !autoDetect_.active());
    capture_->setEnabled(idle && client_.connected() && available_ && !activeCapture_ && swaps_->count()>0);
    cancel_->setEnabled(idle && client_.connected() && activeCapture_);
    disconnect_->setEnabled(idle && client_.connected());
    open_->setEnabled(idle && !completed_.isEmpty());
    for(auto field:{exe_,arguments_,cwd_,out_,hudKey_,captureKey_})field->setEnabled(idle && !client_.connected() && !autoDetect_.active());
    hudMode_->setEnabled(idle && !client_.connected() && !autoDetect_.active());
    sessions_->setEnabled(idle && !activeCapture_ && sessions_->count()>0);
    autoCheck_->setEnabled(autoStatus_->text()!="Stopping" && autoStatus_->text()!="Recovering");
    swaps_->setEnabled(idle && client_.connected());
    resourceLimit_->setEnabled(idle && !activeCapture_);
}
void CaptureDialog::closeEvent(QCloseEvent *event) {
    // The application owns this persistent modeless dialog. Hiding it preserves
    // session control; no child game is killed and no live COM DLL is unloaded.
    hide(); event->ignore();
}
CaptureDialog::~CaptureDialog(){
    autoDetect_.stop();
    // Disconnect control/HUD without terminating games or unloading live COM wrappers.
    QStringList manifests;for(int i=0;i<sessions_->count();++i)manifests<<sessions_->itemData(i).toString();
    if(client_.connected())manifests<<client_.sessionFile();manifests.removeDuplicates();
    for(const auto& manifest:manifests){QProcess process;process.start(controller_,{"disconnect","--session",manifest});if(!process.waitForFinished(5500)){process.kill();process.waitForFinished(1000);}}
}
}
