#include "envelope_fixture.h"
#include "stp/assembly_export.h"
#include "stp/document_workspace.h"
#include "stp/main_window.h"
#include "stp/envelope_dialog.h"
#include "stp/viewer_widget.h"
#include "stp/cad_job_queue.h"
#include "app/qtopengl_utils.h"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QTextEdit>
#include <QFileDialog>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QTimer>
#include <QTest>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QThread>
#include <QListWidget>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QTreeWidget>
#include <QMenu>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
void waitFor(const std::function<bool()>& p){QElapsedTimer t;t.start();while(!p()){if(t.elapsed()>90000)throw std::runtime_error("Envelope GUI timeout");QTest::qWait(20);}}
template<class T>T* control(QObject* parent,const char* name){auto p=parent->findChild<T*>(name);if(!p)throw std::runtime_error(name);return p;}
}
int main(int argc,char** argv){
    Mayo::QtOpenGlUtils::platformSetup(argc,argv);QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);QApplication app(argc,argv);
    QTemporaryDir temp;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.path());
    app.setOrganizationName("STPEnvelopeTest");app.setApplicationName("STPEnvelopeTest");
    qputenv("CSF_ShadersDirectory",QByteArray(STP_OCCT_RESOURCE_DIR)+"/Shaders");
    const QString output=argc>1?QString::fromLocal8Bit(argv[1]):temp.filePath("evidence");QDir().mkpath(output);
    QJsonObject report;QString failure;
    try{
        auto fixture=envelopeAssemblyFixture();std::atomic_bool cancel(false);const auto source=output+QStringLiteral("/模拟 壳体装配.step");
        Stp::exportPlacedAssembly(fixture,{},source,{},cancel);
        Stp::DocumentWorkspace workspace;workspace.resize(1350,950);workspace.show();workspace.openFiles({source});
        auto page=workspace.currentDocument();auto action=control<QAction>(page,"customerEnvelope");waitFor([&]{return action->isEnabled();});
        const auto sourceDocument=page->document();
        auto tree=control<QTreeWidget>(page,"assemblyTree");
        const auto treePosition=tree->visualItemRect(tree->topLevelItem(0)).center();
        QMetaObject::invokeMethod(tree,"customContextMenuRequested",Q_ARG(QPoint,treePosition));
        auto popup=control<QMenu>(page,"partAppearanceMenu");
        control<QAction>(popup,"preserveEnvelopePart")->trigger();popup->close();QTest::qWait(20);
        QMetaObject::invokeMethod(tree,"customContextMenuRequested",Q_ARG(QPoint,treePosition));
        popup=control<QMenu>(page,"partAppearanceMenu");control<QAction>(popup,"restoreEnvelopePart")->trigger();popup->close();QTest::qWait(20);
        report["treePreserveAndRestoreActions"]=true;
        auto release=std::make_shared<std::atomic_bool>(false);
        QTimer::singleShot(0,[&]{
            auto dialog=control<Stp::EnvelopeDialog>(page,"customerEnvelopeDialog");
            try{
                auto generate=control<QPushButton>(dialog,"envelopeGenerate");auto save=control<QPushButton>(dialog,"envelopeExport");
                auto review=control<QCheckBox>(dialog,"envelopeReviewed");auto mode=control<QComboBox>(dialog,"envelopePreviewMode");
                auto axis=control<QComboBox>(dialog,"envelopeSectionAxis");auto fill=control<QCheckBox>(dialog,"envelopeFill");
                require(!save->isEnabled(),"export available without result");
                QTest::mouseClick(generate,Qt::LeftButton);waitFor([&]{return generate->isEnabled();});
                require(bool(dialog->result()),"no result: see envelope report");
                report["geometry"]=dialog->result()->report();require(dialog->result()->outputSolids==1&&dialog->result()->filledCavities==1,"wrong shell result");
                auto viewer=control<Stp::ViewerWidget>(dialog,"envelopeViewer");
                require(!viewer->sectionState().enabled,"default off section was enabled");
                for(int i=1;i<=3;++i){axis->setCurrentIndex(i);const auto state=viewer->sectionState();
                    require(state.enabled&&std::abs(Stp::sectionPlane(state).Axis().Direction().Coord(i))>.999,"section axis mapping wrong");
                    require(std::abs(Stp::sectionPlane(state).Location().Coord(i)-10)<1.e-3,"section offset wrong");}
                auto reverse=control<QCheckBox>(dialog,"envelopeSectionReverse");reverse->setChecked(false);
                require(Stp::sectionPlane(viewer->sectionState()).Axis().Direction().Z()>.999,"reverse did not toggle");reverse->setChecked(true);
                axis->setCurrentIndex(0);require(!viewer->sectionState().enabled,"section off did not disable clipping");
                axis->setCurrentIndex(3);QTest::qWait(100);
                require(dialog->grab().save(output+"/envelope-result.png"),"result screenshot");
                require(viewer->saveScreenshot(output+"/envelope-result-model.png"),"model screenshot");
                mode->setCurrentIndex(1);waitFor([&]{return mode->isEnabled();});
                require(viewer->sectionState().enabled&&Stp::sectionPlane(viewer->sectionState()).Axis().Direction().Z()<-.999,"section not restored on preview switch");
                require(dialog->grab().save(output+"/envelope-original.png"),"original screenshot");
                mode->setCurrentIndex(0);waitFor([&]{return mode->isEnabled();});
                require(!save->isEnabled(),"review gate ignored");review->setChecked(true);require(save->isEnabled(),"review not accepted");
                const auto path=output+QStringLiteral("/客户 外包络.step");
                QTimer::singleShot(0,[path]{for(auto w:QApplication::topLevelWidgets())if(auto f=qobject_cast<QFileDialog*>(w)){f->selectFile(path);QMetaObject::invokeMethod(f,"accept",Qt::DirectConnection);}});
                QTest::mouseClick(save,Qt::LeftButton);waitFor([&]{return generate->isEnabled();});require(QFile::exists(path),"GUI export did not write file");
                auto read=Stp::loadStep(path,{},cancel);require(read->parts.size()==1&&read->parts[0].name=="CustomerEnvelope","GUI export leaked assembly");
                report["menuPreviewSectionAndStepExport"]=true;
                report["allSectionAxesOffsetReverseAndOff"]=true;
                auto rules=control<QListWidget>(dialog,"envelopePreserveList");
                rules->item(1)->setCheckState(Qt::Checked);
                require(!dialog->result()&&!save->isEnabled()&&!review->isChecked(),"preserve rule did not invalidate output");
                generate->click();waitFor([&]{return generate->isEnabled();});
                require(dialog->result()&&dialog->result()->preservedOccurrences==1&&dialog->result()->pieces.size()==2,"preserve UI ignored");
                control<QPushButton>(dialog,"envelopeLocateRule")->click();waitFor([&]{return generate->isEnabled();});
                require(!dialog->result()&&!save->isEnabled(),"source/tool preview enabled stale export");
                bool toolEntered=false;
                QTimer::singleShot(0,[&]{auto editor=control<QDialog>(dialog,"envelopeToolEditor");
                    for(int i=3;i<6;++i)control<QDoubleSpinBox>(editor,QString("toolValue%1").arg(i).toUtf8().constData())->setValue(30);
                    toolEntered=true;editor->accept();});
                control<QPushButton>(dialog,"envelopeAddBox")->click();require(toolEntered,"box editor not opened");
                auto tools=control<QListWidget>(dialog,"envelopeProtections");require(tools->count()==1&&!dialog->result(),"tool not added");
                generate->click();waitFor([&]{return generate->isEnabled();});require(bool(dialog->result()),"non-intersecting protection rejected");
                axis->setCurrentIndex(0);mode->setCurrentIndex(2);waitFor([&]{return mode->isEnabled();});
                dialog->findChild<QTabWidget*>()->setCurrentIndex(2);
                require(dialog->grab().save(output+"/envelope-preservation-tools.png"),"protection screenshot failed");
                tools->setCurrentRow(0);control<QPushButton>(dialog,"envelopeRemoveTool")->click();
                require(tools->count()==0&&!dialog->result()&&!save->isEnabled(),"tool removal did not invalidate");
                rules->item(1)->setCheckState(Qt::Unchecked);
                control<QLineEdit>(dialog,"envelopeKeywords")->setText("SecretPart");
                bool candidateChecked=false;
                QTimer::singleShot(0,[&]{auto list=control<QListWidget>(dialog,"envelopeCandidates");
                    require(list->count()==3,"name candidates missing");list->item(0)->setCheckState(Qt::Checked);candidateChecked=true;
                    qobject_cast<QDialog*>(list->parentWidget())->accept();});
                control<QPushButton>(dialog,"envelopeRecommend")->click();
                require(candidateChecked&&rules->item(0)->checkState()==Qt::Checked,"confirmed candidate not applied");
                rules->item(0)->setCheckState(Qt::Unchecked);mode->setCurrentIndex(0);
                report["preservedInstancesAndInvalidation"]=true;report["protectionEditorPreviewAndRemoval"]=true;report["nameRecommendationsRequireConfirmation"]=true;
                fill->setChecked(false);require(!dialog->result()&&!save->isEnabled()&&!review->isChecked(),"changed settings kept stale export");
                QTest::mouseClick(generate,Qt::LeftButton);waitFor([&]{return generate->isEnabled();});
                require(dialog->result()&&dialog->result()->outputSolids==3,"fill option not applied");
                report["optionsInvalidateAndRegenerate"]=true;
                auto scope=control<QComboBox>(dialog,"envelopeScope");scope->setCurrentIndex(1);generate->click();
                require(!dialog->result()&&!save->isEnabled(),"empty selection used entire assembly");
                scope->setCurrentIndex(0);rules->item(1)->setCheckState(Qt::Checked);
                // Keep processing queued so cancel/close paths are deterministic, without a timing race.
                workspace.jobQueue()->enqueue(999999,[release](std::atomic_bool& c,const Stp::ProgressCallback&){while(!release->load()&&!c.load())QThread::msleep(5);},{},{});
                generate->click();control<QPushButton>(dialog,"envelopeCancel")->click();
                waitFor([&]{return generate->isEnabled();});require(!dialog->result()&&!save->isEnabled(),"cancel retained result");
                generate->click();dialog->reject();release->store(true);
                report["queuedCancelAndClose"]=true;
            }catch(const std::exception& e){release->store(true);failure=QString::fromUtf8(e.what());dialog->reject();}
        });
        action->trigger();require(failure.isEmpty(),failure.toUtf8().constData());
        waitFor([&]{return workspace.jobQueue()->idle();});
        require(page->document()==sourceDocument&&sourceDocument->parts.size()==3&&!page->hasPlacementChanges(),"source session modified");
        report["sourcePreserved"]=true;
        bool persisted=false;QTimer::singleShot(0,[&]{auto dialog=control<Stp::EnvelopeDialog>(page,"customerEnvelopeDialog");
            persisted=control<QListWidget>(dialog,"envelopePreserveList")->item(1)->checkState()==Qt::Checked;dialog->reject();});
        action->trigger();require(persisted,"rules lost on dialog reopen");report["rulesPersistWithinSession"]=true;
        page->beginLoadDocument(source);waitFor([&]{return action->isEnabled();});
        bool cleared=false;QTimer::singleShot(0,[&]{auto dialog=control<Stp::EnvelopeDialog>(page,"customerEnvelopeDialog");
            cleared=dialog->options().preservedOccurrences.empty()&&dialog->options().protections.empty();dialog->reject();});
        action->trigger();require(cleared,"rules leaked into reloaded model");report["rulesClearOnReload"]=true;
        workspace.closeAll();waitFor([&]{return workspace.documentCount()==0&&workspace.jobQueue()->idle();});
        report["status"]="passed";
    }catch(const Standard_Failure& e){failure=QString::fromUtf8(e.GetMessageString());}catch(const std::exception& e){failure=QString::fromUtf8(e.what());}
    if(!failure.isEmpty()){report["status"]="failed";report["error"]=failure;}
    QFile f(output+"/envelope-report.json");if(f.open(QIODevice::WriteOnly))f.write(QJsonDocument(report).toJson());
    std::cout<<QJsonDocument(report).toJson().toStdString();return failure.isEmpty()?0:1;
}
