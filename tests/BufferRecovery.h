#pragma once
// Included by the isolated Worker recovery executable; never part of runtime.
inline void exerciseBufferRecovery() {
    const auto root = qEnvironmentVariable("FLORA_FAULT_ROOT");
    if (root.isEmpty()) QSKIP("Runs only inside the parent-owned temporary executable directory");
    QCOMPARE(QFileInfo(root).canonicalFilePath(), QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath());
    const auto mode = qEnvironmentVariable("FLORA_FAULT_MODE");
    const bool asynchronous = mode.startsWith("buffer-async-");
    const auto worker = QDir(root).filePath("FloraGPA.Worker.exe");
    QVERIFY(!QFile::exists(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
    auto capture = testing::msaaOutputCapture(false);
    const uint32_t size = asynchronous ? 32 * 1024 * 1024 : 64;
    auto resource = std::vector<uint8_t>(16);
    testing::append(resource, D3D11_BUFFER_DESC{size, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0});
    testing::append(resource, Id(81));
    capture.add(80, 5, 0x83, resource);
    auto initial = testing::word(size);
    initial.resize(4 + size, 'C');
    capture.add(81, 9, 1, initial);
    const auto path = QDir(root).filePath("buffer.gpa_frame");
    capture.save(path);
    auto owner = std::make_unique<MainWindow>();
    auto &window = *owner;
    QSignalSpy done(&window, &MainWindow::taskFinished);
    window.openCapture(path);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY(done.last()[0].toBool());
    const auto expectedImage = window.findChild<ImageView *>("frameOutput")->image();
    auto resources = window.findChild<QTableView *>("resources");
    bool selected = false;
    for (int row = 0; row < resources->model()->rowCount(); ++row) {
        auto index = resources->model()->index(row, 0);
        if (index.data(Qt::UserRole).toULongLong() == 80) {
            resources->setCurrentIndex(index); selected = true; break;
        }
    }
    QVERIFY(selected);
    auto offset = window.findChild<QLineEdit *>("bufferOffset");
    auto length = window.findChild<QLineEdit *>("bufferLength");
    auto read = window.findChild<QAction *>("readBuffer");
    QVERIFY(offset && length && read);
    offset->setText("3"); length->setText("17");
    done.clear(); read->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY(done.last()[0].toBool());
    auto model = static_cast<BufferModel *>(window.findChild<QTableView *>("bufferTable")->model());
    QCOMPARE(model->bytes(), QByteArray(17, 'C'));
    QCOMPARE(model->offset(), uint64_t(3));

    const auto fixture = QDir(root).filePath("fault-output");
    QVERIFY(QDir().mkpath(fixture));
    qputenv("FLORA_FAULT_IMAGE_ROOT", fixture.toUtf8());
    QByteArray bytes(asynchronous ? size - 3 : 17, 'C');
    QJsonObject report{{"completed", true}, {"resource", "80"}, {"offset", 3}, {"length", bytes.size()},
                       {"event", QJsonValue(QJsonValue::Null)}, {"value_time", "capture_initial"},
                       {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
    if (mode == "buffer-short") bytes.chop(1);
    if (mode == "buffer-hash") bytes[0] ^= 1;
    if (mode == "buffer-resource") report["resource"] = "79";
    if (mode == "buffer-range") report["offset"] = 4;
    if (mode == "buffer-boundary") report["value_time"] = "after_event";
    for (const auto &pair : {qMakePair(QString("buffer.bin"), bytes),
                             qMakePair(QString("report.json"), QJsonDocument(report).toJson())}) {
        QFile file(fixture + '/' + pair.first);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(pair.second), pair.second.size());
    }
    QVERIFY(QFile::remove(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_FAULT_WORKER"), worker));
    if (asynchronous) length->clear();
    bool seen = false, heartbeat = false;
    QString validationDirectory, switchedPath;
    QElapsedTimer elapsed;
    const auto observer = QObject::connect(window.statusBar(), &QStatusBar::messageChanged, &window,
        [&](const QString &message) {
            if (message != "Validating buffer…" || seen) return;
            seen = true; elapsed.start();
            QVERIFY(window.busy());
            if (!asynchronous) return;
            QTimer::singleShot(0, &window, [&] {
                heartbeat = true;
                QVERIFY(window.busy());
                if (mode == "buffer-async-destroy") {
                    QFile file(fixture + "/validation-directory.txt");
                    QVERIFY(file.open(QIODevice::ReadOnly));
                    validationDirectory = QString::fromLocal8Bit(file.readAll());
                    QVERIFY(QFileInfo::exists(validationDirectory));
                    owner.reset();
                } else if (mode == "buffer-async-close") window.close();
                else if (mode == "buffer-async-switch") {
                    QVERIFY(QFile::remove(worker));
                    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
                    switchedPath = QDir(root).filePath("second.gpa_frame");
                    testing::msaaOutputCapture(false).save(switchedPath);
                    window.openCapture(switchedPath);
                } else if (mode != "buffer-async-success") {
                    bool cancelled = false;
                    for (auto action : window.findChildren<QAction *>())
                        if (action->shortcut() == QKeySequence(Qt::Key_Escape)) {
                            QVERIFY(action->isEnabled()); action->trigger(); cancelled = true;
                        }
                    QVERIFY(cancelled);
                }
            });
        });
    done.clear(); read->trigger();
    if (mode == "buffer-async-destroy") {
        QTRY_VERIFY_WITH_TIMEOUT(!owner, 30000);
        QVERIFY(seen && heartbeat);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(validationDirectory), 30000);
        QCOMPARE(done.size(), 0);
        return;
    }
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), mode == "buffer-async-switch" ? 2 : 1, 30000);
    QObject::disconnect(observer);
    QVERIFY(seen);
    if (asynchronous) QVERIFY(heartbeat);
    const bool accepted = mode == "buffer-valid" || mode == "buffer-async-success";
    QCOMPARE(done.first()[0].toBool(), accepted);
    QVERIFY(!window.busy());
    if (accepted) {
        QCOMPARE(model->bytes(), bytes);
        QCOMPARE(model->offset(), uint64_t(3));
    } else {
        QVERIFY(model->bytes().isEmpty()); // Pending/failed reads never expose unverified bytes.
        if (!asynchronous) QVERIFY2(window.statusBar()->currentMessage().startsWith("Worker buffer"), qPrintable(window.statusBar()->currentMessage()));
    }
    QCOMPARE(window.findChild<ImageView *>("frameOutput")->image(), expectedImage);
    qInfo() << "Buffer validation" << mode << "elapsed_ms" << elapsed.elapsed();
    if (mode == "buffer-async-switch") {
        QVERIFY(done.last()[0].toBool());
        QCOMPARE(window.capturePath(), switchedPath);
        QTest::qWait(100); QCOMPARE(done.size(), 2);
        return;
    }
    QVERIFY(QFile::remove(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
    length->setText("17");
    done.clear(); read->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY(done.last()[0].toBool());
    QCOMPARE(model->bytes(), QByteArray(17, 'C'));
    QCOMPARE(model->offset(), uint64_t(3));
    QVERIFY(!window.busy());
    QTest::qWait(100); QCOMPARE(done.size(), 1);
}
