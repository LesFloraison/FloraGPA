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

namespace flora::capture_ui {
CaptureDialog::CaptureDialog(const QString &controller, QWidget *parent) : QDialog(parent), client_(this) {
    setWindowTitle("DX11 Capture"); resize(660,400); client_.setController(controller);
    auto layout = new QVBoxLayout(this); auto form = new QFormLayout;
    exe_ = new QLineEdit(this); exe_->setObjectName("captureExecutable");
    auto row = new QHBoxLayout; row->addWidget(exe_); auto browse = new QPushButton("Browse…",this); row->addWidget(browse);
    connect(browse,&QPushButton::clicked,this,[this] {
        const auto path=QFileDialog::getOpenFileName(this,"Launch DX11 application",exe_->text(),"Applications (*.exe)");
        if (!path.isEmpty()) { exe_->setText(path); cwd_->setText(QFileInfo(path).absolutePath()); }
    });
    form->addRow("Application",row);
    arguments_=new QLineEdit(this); arguments_->setToolTip("Use quotes around arguments containing spaces."); form->addRow("Arguments",arguments_);
    cwd_=new QLineEdit(this); form->addRow("Working directory",cwd_);
    out_=new QLineEdit(QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(
        "FloraGPA/Captures/"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz")),this);
    out_->setToolTip("A new session directory. Existing directories are preserved."); form->addRow("Output",out_);
    swaps_=new QComboBox(this); swaps_->setObjectName("captureSwapChains"); form->addRow("Swap chain",swaps_);
    layout->addLayout(form);
    status_=new QLabel("Ready to launch",this); status_->setObjectName("captureStatus"); status_->setWordWrap(true); layout->addWidget(status_);
    auto actions = new QHBoxLayout;
    launch_=new QPushButton("Launch",this); capture_=new QPushButton("Capture Next Frame",this); cancel_=new QPushButton("Cancel",this);
    disconnect_=new QPushButton("Disconnect",this); open_=new QPushButton("Open Capture",this);
    launch_->setObjectName("captureLaunch"); capture_->setObjectName("captureNext"); open_->setObjectName("captureOpen");
    for(auto button:{launch_,capture_,cancel_,disconnect_,open_})actions->addWidget(button);
    layout->addLayout(actions);
    auto details=new QGroupBox("Task log",this); details->setCheckable(true); details->setChecked(false);
    auto logs=new QVBoxLayout(details); log_=new QPlainTextEdit(this); log_->setReadOnly(true); log_->setMaximumBlockCount(300);
    log_->hide(); logs->addWidget(log_); connect(details,&QGroupBox::toggled,log_,&QWidget::setVisible); layout->addWidget(details);
    connect(launch_,&QPushButton::clicked,this,[this] {
        completed_.clear(); available_=false;
        client_.launch(exe_->text(),cwd_->text(),out_->text(),QProcess::splitCommand(arguments_->text()));
    });
    connect(capture_,&QPushButton::clicked,this,[this] { client_.capture(swaps_->currentData().toString()); });
    connect(cancel_,&QPushButton::clicked,&client_,&CaptureClient::cancel);
    connect(disconnect_,&QPushButton::clicked,&client_,&CaptureClient::disconnectSession);
    connect(open_,&QPushButton::clicked,this,[this] { if(QFileInfo(completed_).isFile())emit openCaptureRequested(completed_); });
    connect(&client_,&CaptureClient::busyChanged,this,[this] { updateControls(); });
    connect(&client_,&CaptureClient::response,this,&CaptureDialog::showResponse);
    connect(&client_,&CaptureClient::failed,this,[this](const QString &error) { status_->setText(error); log_->appendPlainText(error); updateControls(); });
    connect(&poll_,&QTimer::timeout,this,[this] { if(client_.connected() && !client_.busy())client_.status(); });
    poll_.start(1000); updateControls();
}
void CaptureDialog::showResponse(const QString &command,const QJsonObject &result) {
    const auto state = command=="launch" ? result.value("status").toObject() : result;
    const auto phase=state.value("state").toString();
    activeCapture_=phase=="Armed" || phase=="Capturing" || phase=="Finalizing";
    available_=state.value("capture_available").toBool();
    if(!result.value("ok").toBool())status_->setText(result.value("error").toString("Capture request failed"));
    else status_->setText(available_?phase:phase+" — frame capture unavailable");
    QStringList gaps;for(const auto &gap:state.value("capability_gaps").toArray())gaps<<gap.toString();
    status_->setToolTip(gaps.join('\n'));
    if(command!="status" || !result.value("ok").toBool())log_->appendPlainText(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    const auto selected=swaps_->currentData().toString(); swaps_->clear();
    for(const auto &value:state.value("swap_chains").toObject().value("active").toArray()) {
        const auto swap=value.toObject(); const auto id=QString::number(swap.value("id").toInteger());
        swaps_->addItem(QString("%1 · %2 × %3").arg(id).arg(swap.value("width").toInt()).arg(swap.value("height").toInt()),id);
    }
    const int index=swaps_->findData(selected); if(index>=0)swaps_->setCurrentIndex(index);
    // Only an explicitly completed result may cross into replay.
    const auto completion=state.value("last_capture").toObject();
    if(completion.value("state").toString()=="Completed") {
        const auto path=completion.value("path").toString(); if(QFileInfo(path).isFile())completed_=path;
    }
    updateControls();
}
void CaptureDialog::updateControls() {
    const bool idle=!client_.busy();
    launch_->setEnabled(idle && !client_.connected());
    capture_->setEnabled(idle && client_.connected() && available_ && !activeCapture_ && swaps_->count()>0);
    cancel_->setEnabled(idle && client_.connected() && activeCapture_);
    disconnect_->setEnabled(idle && client_.connected());
    open_->setEnabled(idle && !completed_.isEmpty());
    for(auto field:{exe_,arguments_,cwd_,out_})field->setEnabled(idle && !client_.connected());
}
void CaptureDialog::closeEvent(QCloseEvent *event) {
    // The application owns this persistent modeless dialog. Hiding it preserves
    // session control; no child game is killed and no live COM DLL is unloaded.
    hide(); event->ignore();
}
}
