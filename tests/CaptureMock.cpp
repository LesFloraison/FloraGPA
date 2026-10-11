#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cstdio>
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); const auto args=app.arguments();
    const auto command=args.value(1);
    const auto mode=qEnvironmentVariable("FLORA_CAPTURE_MOCK");
    if(mode=="malformed") { std::puts("not-json"); return 0; }
    if(mode=="oversized") { QByteArray data(2*1024*1024,'x'); fwrite(data.data(),1,data.size(),stdout); return 0; }
    if(mode=="slow")QThread::msleep(300);
    QJsonObject result{{"ok",true},{"version",1},{"state","Tracking"},{"capture_available",false}};
    if(command=="launch") {
        const auto path=args.value(args.indexOf("--out")+1)+"/session.json";
        QFile file(path);if(!file.open(QIODevice::WriteOnly))return 2;file.write("{}");file.close();
        result["session_file"]=path;
    } else if(command=="capture") {
        result["ok"]=false;result["error"]="capture_capability_incomplete";
    } else if(command=="disconnect")result["state"]="Disconnected";
    if(mode=="exited"){result["ok"]=false;result["state"]="Exited";result["error"]="target_exited";}
    auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact);fwrite(bytes.data(),1,bytes.size(),stdout);
    if(mode=="contradiction")return 2;
    return result["ok"].toBool()?0:3;
}
