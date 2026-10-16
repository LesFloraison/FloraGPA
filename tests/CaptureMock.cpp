#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QThread>
#include <cstdio>
#include <QDir>
#include <iostream>
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); const auto args=app.arguments();
    const auto command=args.value(1);
    const auto mode=qEnvironmentVariable("FLORA_CAPTURE_MOCK");
    if(mode=="malformed") { std::puts("not-json"); return 0; }
    if(mode=="oversized") { QByteArray data(2*1024*1024,'x'); fwrite(data.data(),1,data.size(),stdout); return 0; }
    if(command=="monitor") {
        auto emitEvent=[](QJsonObject value){auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);fwrite(bytes.data(),1,bytes.size(),stdout);std::puts("");fflush(stdout);};
        if(mode=="monitor-denied"){emitEvent({{"schema",1},{"ok",false},{"event","error"},{"error","Elevation denied"}});return 3;}
        const auto root=args.value(args.indexOf("--out")+1);QDir().mkpath(root);
        emitEvent({{"schema",1},{"ok",true},{"event","ready"}});
        if(mode=="monitor-progress"){
            for(const auto* state:{"Detected","Attaching","Waiting for graphics","Attach failed"})
                emitEvent({{"schema",1},{"ok",true},{"event","attachment_progress"},{"state",state},{"phase","test"},{"pid",42},{"win32_error",5},{"arguments",QJsonArray::fromStringList(args)}});
        }
        for(int i=1;i<=2;++i){
            const auto path=root+QString("/session%1.json").arg(i);
            QJsonObject manifest{{"version",4},{"pid",i},{"process_created",12345},{"session_id",QString("session-%1").arg(i)},
                {"attachment","auto_detect"},{"session_file",path},{"executable",args.contains("--target-exe") && i==2?args.value(args.indexOf("--target-exe")+1):QString("Game%1.exe").arg(i)}};
            QFile file(path);if(!file.open(QIODevice::WriteOnly))return 2;file.write(QJsonDocument(manifest).toJson());file.close();
            emitEvent({{"schema",1},{"ok",true},{"event","session"},{"session",manifest}});
        }
        std::string line;std::getline(std::cin,line);
        emitEvent({{"schema",1},{"ok",true},{"event","stopped"}});return 0;
    }
    if(mode=="slow")QThread::msleep(300);
    QJsonObject result{{"ok",true},{"version",4},{"state","Tracking"},{"capture_available",false}};
    const auto session=args.value(args.indexOf("--session")+1);
    unsigned limit=1024;
    QFile budget(session+".budget");if(budget.open(QIODevice::ReadOnly)){limit=budget.readAll().toUInt();budget.close();}
    if(command=="configure"){
        if(mode=="settings-fail"){result["ok"]=false;result["error"]="configuration_rejected";}
        else{limit=args.value(args.indexOf("--resource-limit-mib")+1).toUInt();if(!budget.open(QIODevice::WriteOnly))return 2;budget.write(QByteArray::number(limit));budget.close();}
    }
    if(mode!="old-settings")result["capture_limits"]=QJsonObject{{"session_resource_limit_mib",int(limit)}};
    if(command=="launch") {
        const auto path=args.value(args.indexOf("--out")+1)+"/session.json";
        QFile file(path);if(!file.open(QIODevice::WriteOnly))return 2;file.write("{}");file.close();
        result["session_file"]=path;
        result["arguments"]=QJsonArray::fromStringList(args);
        result["status"]=QJsonObject{{"capture_limits",result["capture_limits"]},{"state","Tracking"}};
    } else if(command=="capture") {
        result["ok"]=false;result["error"]="capture_capability_incomplete";
    } else if(command=="disconnect")result["state"]="Disconnected";
    if(mode=="settings" || mode=="settings-fail" || mode=="old-settings"){
        result["capture_available"]=true;
        result["swap_chains"]=QJsonObject{{"active",QJsonArray{QJsonObject{{"id",1},{"width",800},{"height",600}}}}};
        result["arguments"]=QJsonArray::fromStringList(args);
    }
    if(mode=="exited"){result["ok"]=false;result["state"]="Exited";result["error"]="target_exited";}
    auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact);fwrite(bytes.data(),1,bytes.size(),stdout);
    if(mode=="contradiction")return 2;
    return result["ok"].toBool()?0:3;
}
