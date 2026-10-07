#pragma once
#include "DepthStencilCapture.h"
#include <QTabWidget>
#include <QTreeWidgetItemIterator>

inline void exerciseThumbnailRecovery() {
    using Json=nlohmann::json;
    const auto root=qEnvironmentVariable("FLORA_FAULT_ROOT");
    if (root.isEmpty()) QSKIP("Runs only inside the parent-owned temporary executable directory");
    QCOMPARE(QFileInfo(root).canonicalFilePath(),QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath());
    const auto mode=qEnvironmentVariable("FLORA_FAULT_MODE");
    const bool asynchronous=mode.contains("-async-");
    const bool accepted=mode.endsWith("-valid") || mode.endsWith("-success");
    const auto worker=QDir(root).filePath("FloraGPA.Worker.exe");
    QVERIFY(!QFile::exists(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"),worker));
    const auto capture=QDir(root).filePath("thumbnail.gpa_frame");
    testing::depthStencilCapture().save(capture);
    auto owner=std::make_unique<MainWindow>(); auto &window=*owner;
    window.resize(1200,800); window.show();
    QSignalSpy done(&window,&MainWindow::taskFinished);
    window.openCapture(capture);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,30000);
    QVERIFY(done.last()[0].toBool());
    auto browser=window.findChild<ResourceBrowser *>(); QVERIFY(browser);
    window.findChild<QTabWidget *>("analysisTabs")->setCurrentIndex(0);
    auto api=window.findChild<QTableView *>("apiLog"); QVERIFY(api);
    auto selectDraw=[&] {
        for (int row=0;row<api->model()->rowCount();++row)
            if (api->model()->index(row,0).data(Qt::UserRole).toULongLong()==1000) {
                api->setCurrentIndex(api->model()->index(row,0));return;
            }
        QFAIL("Thumbnail Draw is missing");
    };
    bool warmReady=false;
    auto warm=QObject::connect(window.statusBar(),&QStatusBar::messageChanged,&window,[&](const QString &s) {if(s=="Thumbnails ready") warmReady=true;});
    selectDraw();
    QTRY_VERIFY_WITH_TIMEOUT(warmReady && !window.busy(),30000);
    QObject::disconnect(warm);
    const auto bindings=browser->bindings(); QCOMPARE(bindings.size(),size_t(2));
    auto item=[&](const std::string &key) -> QTreeWidgetItem * {
        auto tree=browser->findChild<QTreeWidget *>("drawResources");
        for (QTreeWidgetItemIterator it(tree);*it;++it)
            if ((*it)->data(0,Qt::UserRole).toString()==QString::fromStdString(key)) return *it;
        return nullptr;
    };
    std::map<std::string,QImage> expected;
    for (const auto &b:bindings) {
        QVERIFY(item(b.key)); expected[b.key]=item(b.key)->icon(0).pixmap(32,32).toImage();
    }
    const auto frame=window.findChild<ImageView *>("frameOutput")->image();
    const auto fixture=QDir(root).filePath("fault-output");
    qputenv("FLORA_FAULT_IMAGE_ROOT",fixture.toUtf8());
    Json keys=Json::array();for(const auto &b:bindings) keys.push_back(b.key);
    const auto request=QDir(root).filePath("previews.json");
    {QFile file(request);QVERIFY(file.open(QIODevice::WriteOnly));const auto bytes=QByteArray::fromStdString(Json{{"previews",keys}}.dump());QCOMPARE(file.write(bytes),bytes.size());}
    QProcess producer;
    producer.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args){args->flags|=CREATE_NO_WINDOW;});
    producer.start(worker,{"draw-resources",capture,"--id","1000","--preview-request",request,"--out",fixture});
    QVERIFY(producer.waitForStarted());
    const auto cleanup=qScopeGuard([&]{if(producer.state()!=QProcess::NotRunning){producer.kill();producer.waitForFinished();}});
    QTRY_COMPARE_WITH_TIMEOUT(producer.state(),QProcess::NotRunning,30000);
    QCOMPARE(producer.exitStatus(),QProcess::NormalExit);
    QVERIFY2(producer.exitCode()==0,producer.readAllStandardError().constData());
    QFile reportFile(fixture+"/report.json");QVERIFY(reportFile.open(QIODevice::ReadOnly));
    auto report=Json::parse(reportFile.readAll().toStdString());reportFile.close();
    QImage sentinel(32,32,QImage::Format_RGBA8888);sentinel.fill(QColor(53,97,179,211));
    for (const auto &row:report["bindings"]) {
        QVERIFY(row.contains("preview"));
        QVERIFY(sentinel.save(fixture+'/'+QString::fromStdString(row["preview"])));
    }
    const auto secondName=QString::fromStdString(report["bindings"][1]["preview"]);
    // Corrupt the last key in consumer order so a one-pass publisher would
    // expose an earlier valid row before noticing the identity mismatch.
    auto lastBinding=std::max_element(report["bindings"].begin(),report["bindings"].end(),
        [](const Json &a,const Json &b){return a.at("key").get<std::string>()<b.at("key").get<std::string>();});
    if(mode.endsWith("-missing")) QVERIFY(QFile::remove(fixture+'/'+secondName));
    if(mode.endsWith("-identity")) (*lastBinding)["resource"]=999;
    if(mode.endsWith("-subresource")) (*lastBinding)["mip"]=1;
    if(mode.endsWith("-omitted")) report["bindings"].erase(1);
    if(mode.endsWith("-duplicate")) report["bindings"].push_back(report["bindings"][0]);
    if(mode.endsWith("-oversized")) {QImage image(97,96,QImage::Format_RGBA8888);image.fill(Qt::red);QVERIFY(image.save(fixture+'/'+secondName));}
    if(asynchronous) report["padding"]=std::string(64*1024*1024,'x');
    {QFile file(fixture+"/report.json");QVERIFY(file.open(QIODevice::WriteOnly));const auto bytes=QByteArray::fromStdString(report.dump());QCOMPARE(file.write(bytes),bytes.size());}
    QVERIFY(QFile::remove(worker));QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_FAULT_WORKER"),worker));
    browser->setContext("fault",bindings,1000);browser->select(bindings[0].key);
    std::map<std::string,qint64> oldIcons;
    for(const auto &b:bindings) oldIcons[b.key]=item(b.key)->icon(0).cacheKey();
    bool seen=false,heartbeat=false;
    QElapsedTimer elapsed;QTimer pulse;int ticks=0;qint64 last=0,gap=0;
    QString switched,validationDirectory;
    QObject::connect(&pulse,&QTimer::timeout,&window,[&]{if(window.busy()){const auto now=elapsed.elapsed();gap=std::max(gap,now-last);last=now;++ticks;}});
    auto observer=QObject::connect(window.statusBar(),&QStatusBar::messageChanged,&window,[&](const QString &s){
        if(seen||s!="Reading report…")return;
        seen=true;elapsed.start();QVERIFY(window.busy());
        if(!asynchronous)return;
        pulse.start(5);
        QTimer::singleShot(0,&window,[&]{
            heartbeat=true;QVERIFY(window.busy());
            if(accepted)return;
            browser->hide(); // Stop automatic retries while observing cancellation.
            if(mode.endsWith("-destroy")) {
                QFile file(fixture+"/validation-directory.txt");QVERIFY(file.open(QIODevice::ReadOnly));
                validationDirectory=QString::fromLocal8Bit(file.readAll());QVERIFY(QFileInfo::exists(validationDirectory));owner.reset();
            } else if(mode.endsWith("-close")) window.close();
            else if(mode.endsWith("-switch")) {
                QVERIFY(QFile::remove(worker));QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"),worker));
                switched=QDir(root).filePath("second.gpa_frame");testing::depthStencilCapture().save(switched);window.openCapture(switched);
            } else {
                bool cancelled=false;
                for(auto action:window.findChildren<QAction *>())if(action->shortcut()==QKeySequence(Qt::Key_Escape)){
                    QVERIFY(action->isEnabled());action->trigger();cancelled=true;
                }
                QVERIFY(cancelled);
            }
        });
    });
    done.clear();emit browser->previewsNeeded();
    if(mode.endsWith("-destroy")) {
        QTRY_VERIFY_WITH_TIMEOUT(!owner,30000);QVERIFY(seen&&heartbeat);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(validationDirectory),30000);
        QCOMPARE(done.size(),0);return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(seen&&!window.busy(),30000);
    QObject::disconnect(observer);pulse.stop();
    if(asynchronous)QVERIFY(heartbeat);
    QCOMPARE(window.findChild<ImageView *>("frameOutput")->image(),frame);
    if(!switched.isEmpty()) {
        QCOMPARE(done.size(),1);QVERIFY(done.last()[0].toBool());QCOMPARE(window.capturePath(),switched);
        QVERIFY(browser->bindings().empty());
    } else {
        QCOMPARE(done.size(),0); // Automatic thumbnails are not frame replay completions.
        for(const auto &b:bindings) {
            // PNG/display contract is RGBA8; QColor can retain higher-precision
            // unpremultiplication components despite identical packed pixels.
            if(accepted) QCOMPARE(item(b.key)->icon(0).pixmap(32,32).toImage().pixelColor(16,16).rgba(),QColor(53,97,179,211).rgba());
            else QCOMPARE(item(b.key)->icon(0).cacheKey(),oldIcons.at(b.key));
        }
        if(!accepted&&!asynchronous) {
            const auto error=item(bindings[0].key)->toolTip(0);
            const auto expectedError=mode.endsWith("-identity")||mode.endsWith("-subresource")?QString("identity"):
                mode.endsWith("-omitted")?QString("binding is missing"):
                mode.endsWith("-duplicate")?QString("duplicate"):
                mode.endsWith("-oversized")?QString("dimensions"):secondName;
            QVERIFY2(error.contains(expectedError,Qt::CaseInsensitive),qPrintable(error));
            qInfo().noquote()<<"thumbnail rejection"<<mode<<error;
        }
    }
    if(mode.endsWith("-success")) {QVERIFY(ticks>=3);qInfo()<<"Thumbnail heartbeat ticks"<<ticks<<"maximum_gap_ms"<<gap<<"completion_ms"<<elapsed.elapsed();}
    const auto completions=done.size();QTest::qWait(100);QCOMPARE(done.size(),completions);
    QVERIFY(QFile::remove(worker));QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"),worker));
    // A fresh request, still through the real UI scheduler, must recover.
    browser->hide();
    if(!switched.isEmpty()){selectDraw();QTest::qWait(400);QTRY_VERIFY_WITH_TIMEOUT(!window.busy(),30000);}
    window.show();browser->setContext("retry",bindings,1000);browser->select(bindings[0].key);browser->show();
    bool retryReady=false;
    auto retry=QObject::connect(window.statusBar(),&QStatusBar::messageChanged,&window,[&](const QString &s){if(s=="Thumbnails ready")retryReady=true;});
    emit browser->previewsNeeded();
    QTRY_VERIFY_WITH_TIMEOUT(retryReady&&!window.busy(),30000);
    QObject::disconnect(retry);
    for(const auto &b:bindings) QCOMPARE(item(b.key)->icon(0).pixmap(32,32).toImage(),expected.at(b.key));
}
