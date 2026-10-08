#include "document_workspace.h"
#include "cad_job_queue.h"
#include "main_window.h"
#include "viewer_widget.h"
#include "../src/app/qtopengl_utils.h"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QGroupBox>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QMessageBox>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QStyleOptionViewItem>
#include <QSurfaceFormat>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTimer>
#include <QToolButton>
#include <QXmlStreamReader>
#include <QtEndian>
#include <zlib.h>
#include <Graphic3d_Camera.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <BRepBndLib.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFPrs_AISObject.hxx>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <thread>
#include <stdexcept>

namespace {
bool waitFor(const std::function<bool()>& condition,int timeout=30000)
{
    QElapsedTimer timer;timer.start();
    while(!condition()&&timer.elapsed()<timeout)QTest::qWait(10);
    return condition();
}
Stp::MainWindow* documentAt(QTabWidget* tabs,int index)
{
    auto page=tabs?tabs->widget(index):nullptr;
    if(auto document=qobject_cast<Stp::MainWindow*>(page))return document;
    return page?page->findChild<Stp::MainWindow*>():nullptr;
}
bool activate(QTabWidget* tabs,Stp::MainWindow* document)
{
    for(int index=0;index<tabs->count();++index)if(documentAt(tabs,index)==document){
        tabs->setCurrentIndex(index);QTest::qWait(100);
        return waitFor([&]{return document->document()&&!document->isBusy();});
    }
    return false;
}
int rowForPart(QTableWidget* table,int part)
{
    for(int row=0;row<table->rowCount();++row)
        if(table->item(row,0)&&table->item(row,0)->data(Qt::UserRole).isValid()
            &&table->item(row,0)->data(Qt::UserRole).toInt()==part)return row;
    return -1;
}
QTableWidgetItem* valueForPart(QTableWidget* table,int part,int column)
{
    const int row=rowForPart(table,part);return row>=0?table->item(row,column):nullptr;
}
QTableWidgetItem* propertyValue(QTableWidget* table,const QString& key)
{
    for(int row=0;row<table->rowCount();++row)
        if(table->item(row,0)&&table->item(row,0)->text()==key)return table->item(row,1);
    return nullptr;
}
bool number(const QTableWidgetItem* item,double expected)
{
    bool ok=false;const double value=item?item->text().toDouble(&ok):0.;
    return ok&&std::abs(value-expected)<std::max(1.e-12,std::abs(expected)*1.e-8);
}
QLineEdit* editorFor(QTableWidget* table)
{
    for(auto editor:table->findChildren<QLineEdit*>())if(editor->isVisible())return editor;
    return nullptr;
}
bool beginEdit(QTableWidget* table,QTableWidgetItem* item,const QString& value)
{
    if(!item||!(item->flags()&Qt::ItemIsEditable))return false;
    table->scrollToItem(item,QAbstractItemView::PositionAtCenter);table->setFocus();
    const auto point=table->visualItemRect(item).center();
    QTest::mouseClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,point);
    QTest::mouseDClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,point);QTest::qWait(30);
    auto editor=editorFor(table);if(!editor)return false;editor->setText(value);return true;
}
QJsonObject fieldState(QTableWidget* table,QTableWidgetItem* item)
{
    if(!item)return {{"exists",false}};
    const auto rect=table->visualItemRect(item);
    return {{"exists",true},{"text",item->text()},{"editable",bool(item->flags()&Qt::ItemIsEditable)},
        {"tableVisible",table->isVisible()},{"tableWidth",table->width()},{"viewportWidth",table->viewport()->width()},
        {"cellRect",QJsonArray{rect.x(),rect.y(),rect.width(),rect.height()}},
        {"clickPointVisible",table->viewport()->rect().contains(rect.center())}};
}
bool edit(QTableWidget* table,QTableWidgetItem* item,const QString& value)
{
    if(!beginEdit(table,item,value))return false;
    QTest::keyClick(editorFor(table),Qt::Key_Return);QTest::qWait(60);return !editorFor(table);
}
int firstOccurrence(const Stp::CadDocument& document,int part)
{
    for(size_t index=0;index<document.occurrences.size();++index)
        if(document.occurrences[index].partIndex==part)return int(index);
    return -1;
}
void selectPart(Stp::MainWindow* page,int part)
{
    const int occurrence=firstOccurrence(*page->document(),part);
    auto view=page->findChild<Stp::ViewerWidget*>();
    view->selectOccurrences({occurrence});view->selectionChanged({occurrence});QTest::qWait(50);
}
QTreeWidgetItem* occurrenceItem(QTreeWidget* tree,int occurrence)
{
    for(QTreeWidgetItemIterator it(tree);*it;++it)
        if((*it)->data(0,Qt::UserRole).toInt()==occurrence)return *it;
    return nullptr;
}
bool uncheck(QTreeWidget* tree,QTreeWidgetItem* item)
{
    if(!item)return false;
    tree->expandAll();tree->scrollToItem(item,QAbstractItemView::PositionAtCenter);QTest::qWait(20);
    QStyleOptionViewItem option;option.initFrom(tree);option.rect=tree->visualItemRect(item);
    option.features=QStyleOptionViewItem::HasCheckIndicator|QStyleOptionViewItem::HasDisplay;
    option.checkState=item->checkState(0);
    const auto indicator=tree->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,&option,tree);
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,indicator.center());QTest::qWait(60);
    return item->checkState(0)==Qt::Unchecked;
}
bool cameraMatches(const Handle(Graphic3d_Camera)& actual,const Handle(Graphic3d_Camera)& expected)
{
    return !actual.IsNull()&&!expected.IsNull()&&actual->Center().Distance(expected->Center())<1.e-6
        &&actual->Eye().Distance(expected->Eye())<1.e-6&&actual->Direction().IsEqual(expected->Direction(),1.e-6)
        &&std::abs(actual->Scale()-expected->Scale())<1.e-6;
}
QJsonArray transformValues(const gp_Trsf& transform)
{
    QJsonArray values;
    for(int row=1;row<=3;++row)for(int column=1;column<=4;++column)values.append(transform.Value(row,column));
    return values;
}
QJsonArray sourceTransforms(const Stp::CadDocument& document)
{
    QJsonArray values;for(const auto& occurrence:document.occurrences)values.append(transformValues(occurrence.worldTransform));return values;
}
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
bool sameTransform(const gp_Trsf& a,const gp_Trsf& b)
{
    for(int row=1;row<=3;++row)for(int column=1;column<=4;++column)
        if(std::abs(a.Value(row,column)-b.Value(row,column))>1.e-6)return false;
    return true;
}
QJsonArray displayTransforms(Stp::ViewerWidget* view)
{
    AIS_ListOfInteractive objects;view->context()->ObjectsInside(objects);std::vector<QByteArray> sorted;
    for(const auto& item:objects){
        const auto shape=Handle(XCAFPrs_AISObject)::DownCast(item);if(shape.IsNull())continue;
        sorted.push_back(QJsonDocument(transformValues(shape->LocalTransformation())).toJson(QJsonDocument::Compact));
    }
    std::sort(sorted.begin(),sorted.end());QJsonArray result;
    for(const auto& values:sorted)result.append(QJsonDocument::fromJson(values).array());return result;
}
QPoint currentPickPoint(Stp::ViewerWidget* view,const Stp::CadDocument& document,int occurrence,const Handle(XCAFPrs_AISObject)& object)
{
    auto viewport=view->findChild<QWidget*>("cadViewport");
    if(!viewport||object.IsNull()||occurrence<0||size_t(occurrence)>=document.occurrences.size())return {-1,-1};
    const int part=document.occurrences[size_t(occurrence)].partIndex;if(part<0)return {-1,-1};
    Bnd_Box box;BRepBndLib::Add(document.parts[size_t(part)].shape,box);if(box.IsVoid())return {-1,-1};
    double x0,y0,z0,x1,y1,z1;box.Get(x0,y0,z0,x1,y1,z1);
    const double low[]{x0,y0,z0},high[]{x1,y1,z1};const double ratio=viewport->devicePixelRatioF();
    const auto world=object->LocalTransformation()*XCAFDoc_ShapeTool::GetLocation(object->GetLabel()).Transformation();
    for(int axis=0;axis<3;++axis)for(double side:{0.,1.})for(double a:{.25,.5,.75})for(double b:{.25,.5,.75}){
        double coordinates[3];for(int i=0;i<3;++i)coordinates[i]=(low[i]+high[i])*.5;
        coordinates[axis]=low[axis]+side*(high[axis]-low[axis]);
        coordinates[(axis+1)%3]=low[(axis+1)%3]+a*(high[(axis+1)%3]-low[(axis+1)%3]);
        coordinates[(axis+2)%3]=low[(axis+2)%3]+b*(high[(axis+2)%3]-low[(axis+2)%3]);
        const auto point=gp_Pnt(coordinates[0],coordinates[1],coordinates[2]).Transformed(world);
        int x,y;view->view()->Convert(point.X(),point.Y(),point.Z(),x,y);const QPoint logical(qRound(x/ratio),qRound(y/ratio));
        if(!viewport->rect().adjusted(4,4,-4,-4).contains(logical))continue;
        view->context()->MoveTo(x,y,view->view(),false);
        if(view->context()->HasDetected()&&view->context()->DetectedInteractive()==object)return logical;
    }
    return {-1,-1};
}
void revealControl(Stp::MainWindow* page,QWidget* widget)
{
    if(auto tabs=page->findChild<QTabWidget*>("viewControlTabs"))for(int index=0;index<tabs->count();++index)
        if(tabs->widget(index)->isAncestorOf(widget)){tabs->setCurrentIndex(index);QTest::qWait(20);break;}
    for(auto parent=widget->parentWidget();parent;parent=parent->parentWidget())
        if(auto scroll=qobject_cast<QScrollArea*>(parent)){scroll->ensureWidgetVisible(widget,0,0);QTest::qWait(20);break;}
}
struct Placement {
    Handle(XCAFPrs_AISObject) object;
    gp_Trsf original;
    int occurrence=-1;
    bool passed=false;
};
Placement placeThroughGrabButton(Stp::MainWindow* page,bool leaveEnabled)
{
    Placement result;auto view=page->findChild<Stp::ViewerWidget*>();auto viewport=view->findChild<QWidget*>("cadViewport");
    auto action=page->findChild<QAction*>("togglePartGrab");auto button=page->findChild<QToolButton*>("partGrabButton");
    if(!action||!button||!viewport||!button->isEnabled())return result;
    const auto priorSelection=view->selectedOccurrences();QPoint point;
    for(size_t index=0;index<page->document()->occurrences.size();++index){
        if(page->document()->occurrences[index].partIndex<0||!view->isOccurrenceDisplayed(int(index)))continue;
        view->selectOccurrences({int(index)});view->context()->InitSelected();if(!view->context()->MoreSelected())continue;
        result.object=Handle(XCAFPrs_AISObject)::DownCast(view->context()->SelectedInteractive());
        point=currentPickPoint(view,*page->document(),int(index),result.object);
        if(point.x()>=0){result.occurrence=int(index);break;}
    }
    if(result.occurrence<0)return result;
    result.original=result.object->LocalTransformation();const Handle(Graphic3d_Camera) camera=new Graphic3d_Camera(view->view()->Camera());
    revealControl(page,button);if(!action->isChecked())QTest::mouseClick(button,Qt::LeftButton);
    const QPoint to=point+QPoint(45,-25);
    QMouseEvent press(QEvent::MouseButtonPress,point,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(viewport,&press);
    QMouseEvent move(QEvent::MouseMove,to,to,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(viewport,&move);
    QMouseEvent release(QEvent::MouseButtonRelease,to,to,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(viewport,&release);QTest::qWait(40);
    result.passed=action->isChecked()&&view->grabMode()&&!sameTransform(result.original,result.object->LocalTransformation())
        &&cameraMatches(view->view()->Camera(),camera)&&view->selectedOccurrences()==std::vector<int>{result.occurrence};
    if(!leaveEnabled)QTest::mouseClick(button,Qt::LeftButton);
    result.passed=result.passed&&action->isChecked()==leaveEnabled&&view->grabMode()==leaveEnabled;
    view->selectOccurrences(priorSelection);view->selectionChanged(priorSelection);return result;
}
#endif
QRect visibleRect(QWidget* widget)
{
    QRect visible=widget->rect();
    for(auto parent=widget->parentWidget();parent;parent=parent->parentWidget())
        visible=visible.intersected(QRect(widget->mapFrom(parent,QPoint()),parent->size()));
    return visible;
}
QJsonObject sessionState(Stp::MainWindow* page)
{
    QJsonObject result;
    for(const auto name:{"explosionSlider","transparencySlider","sectionSlider"})
        result[name]=page->findChild<QSlider*>(name)->value();
    for(const auto name:{"explosionScope","meshQuality","sectionAxis"})
        result[name]=page->findChild<QComboBox*>(name)->currentIndex();
    result["pmiVisible"]=page->findChild<QAction*>("togglePmi")->isChecked();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    result["grabMode"]=page->findChild<QAction*>("togglePartGrab")->isChecked();
    result["displayTransforms"]=displayTransforms(page->findChild<Stp::ViewerWidget*>());
#endif
    result["modelPage"]=page->findChild<QStackedWidget*>("workspacePages")->currentIndex();
    QJsonArray checks,bomRows,selection;
    for(QTreeWidgetItemIterator it(page->findChild<QTreeWidget*>("assemblyTree"));*it;++it)
        checks.append(QJsonArray{(*it)->data(0,Qt::UserRole).toInt(),int((*it)->checkState(0))});
    auto bom=page->findChild<QTableWidget*>("bomTable");
    for(int row=0;row<bom->rowCount();++row){
        QJsonArray cells;
        for(int column=0;column<bom->columnCount();++column)cells.append(bom->item(row,column)?bom->item(row,column)->text():QString());
        bomRows.append(cells);
    }
    for(int occurrence:page->findChild<Stp::ViewerWidget*>()->selectedOccurrences())selection.append(occurrence);
    result["checkedOccurrences"]=checks;result["bomRows"]=bomRows;result["sceneSelection"]=selection;return result;
}
QJsonObject reachableControls(Stp::MainWindow* page)
{
    QJsonArray controls;bool reachable=true;int count=0;
    auto tabs=page->findChild<QTabWidget*>("viewControlTabs");const int originalTab=tabs?tabs->currentIndex():0;
    for(auto group:page->findChildren<QGroupBox*>())for(auto widget:group->findChildren<QWidget*>()){
        if(!qobject_cast<QComboBox*>(widget)&&!qobject_cast<QSlider*>(widget)
            &&!qobject_cast<QCheckBox*>(widget)&&!qobject_cast<QPushButton*>(widget)&&!qobject_cast<QToolButton*>(widget))continue;
        if(tabs)for(int index=0;index<tabs->count();++index)
            if(tabs->widget(index)->isAncestorOf(widget)){tabs->setCurrentIndex(index);QTest::qWait(20);break;}
        // Compact view-control pages above are deliberately activated, while
        // controls on inactive detail tabs are absent from the visible layout.
        // Measurement has its own explicit reachability coverage.
        if(!widget->isVisible())continue;
        const bool initiallyVisible=widget->isVisible()&&visibleRect(widget)==widget->rect();
        QScrollArea* scroll=nullptr;
        for(auto parent=widget->parentWidget();parent;parent=parent->parentWidget())
            if((scroll=qobject_cast<QScrollArea*>(parent)))break;
        if(scroll){scroll->ensureWidgetVisible(widget,0,0);QTest::qWait(20);}
        const bool fullyReachable=widget->isVisible()&&visibleRect(widget)==widget->rect();
        const auto rect=visibleRect(widget);
        controls.append(QJsonObject{{"name",widget->objectName()},{"class",QString::fromLatin1(widget->metaObject()->className())},
            {"group",group->title()},{"initiallyFullyVisible",initiallyVisible},{"scrollAvailable",bool(scroll)},
            {"fullyReachable",fullyReachable},{"visibleRect",QJsonArray{rect.x(),rect.y(),rect.width(),rect.height()}}});
        reachable=reachable&&fullyReachable;++count;
    }
    if(tabs)tabs->setCurrentIndex(originalTab);
    for(auto scroll:page->findChildren<QScrollArea*>()){
        scroll->horizontalScrollBar()->setValue(0);scroll->verticalScrollBar()->setValue(0);
    }
    auto hint=page->findChild<QLabel*>("interactionHint");
    const bool hintVisible=hint&&hint->isVisible()&&visibleRect(hint)==hint->rect();
    return {{"passed",reachable&&count>=11&&hintVisible},{"controlCount",count},
        {"rightSidebarHintFullyVisible",hintVisible},{"controls",controls}};
}
QJsonObject renderedModel(Stp::MainWindow* page,const QString& path)
{
    auto view=page->findChild<Stp::ViewerWidget*>();QTest::qWait(120);
    const bool saved=view&&view->saveScreenshot(path);const QImage image(path);
    int foreground=0;
    // The crop excludes the corner trihedron: a blank CAD viewport with only
    // axes must not pass. The shaded model and its edges differ from the pale
    // background by substantially more than one antialiasing level.
    for(int y=image.height()/5;y<image.height()*4/5;y+=2)
        for(int x=image.width()/5;x<image.width()*4/5;x+=2){
            const auto color=image.pixelColor(x,y);
            if(std::min({color.red(),color.green(),color.blue()})<205)++foreground;
        }
    return {{"passed",saved&&!image.isNull()&&foreground>100},{"saved",saved},
        {"width",image.width()},{"height",image.height()},{"foregroundSamples",foreground},{"path",path}};
}
void require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
QJsonObject exportThroughUi(QAction* action,const QString& target,const QString& successTitle,QWidget* toolbarButton=nullptr,bool waitCompletion=true)
{
    QTimer responder;responder.setInterval(25);QElapsedTimer elapsed;elapsed.start();
    bool fileDialogAccepted=false,completionSeen=false,fileNameEditorPresent=false;
    QString completionTitle,completionText,acceptedPath;int completionIcon=-1;
    QPointer<QFileDialog> configuredDialog;int dialogRounds=0;
    const auto absolutePath=[](const QString& path){return QDir::cleanPath(QFileInfo(path).absoluteFilePath());};
    const QString targetPath=absolutePath(target);
    QObject::connect(&responder,&QTimer::timeout,[&]{
        // Modal acceptance can destroy other top-level widgets. Keep guarded
        // pointers, and queue each file-dialog acceptance only once.
        QList<QPointer<QWidget>> widgets;
        for(auto widget:QApplication::topLevelWidgets())widgets.append(widget);
        for(auto guarded:widgets){
            auto widget=guarded.data();if(!widget)continue;
            if(!widget->isVisible())continue;
            if(auto dialog=qobject_cast<QFileDialog*>(widget)){
                if(fileDialogAccepted)continue;
                if(elapsed.elapsed()>30000){dialog->reject();continue;}
                const bool directory=dialog->fileMode()==QFileDialog::Directory;
                if(configuredDialog!=dialog){
                    configuredDialog=dialog;dialogRounds=0;
                    dialog->setDirectory(directory?targetPath:QFileInfo(targetPath).absolutePath());
                    dialog->selectFile(directory?targetPath:QFileInfo(targetPath).fileName());
                }
                // QFileDialog can restore its initial filename after the first
                // event turn. Give its directory model time to settle, then set
                // the actual filename editor and verify selectedFiles before
                // accepting. This also avoids accepting a parent directory.
                if(++dialogRounds<4)continue;
                if(auto editor=dialog->findChild<QLineEdit*>("fileNameEdit")){
                    fileNameEditorPresent=true;
                    const QString requested=directory?targetPath:QFileInfo(targetPath).fileName();
                    if(editor->text()!=requested){editor->setText(requested);continue;}
                }else dialog->selectFile(directory?targetPath:QFileInfo(targetPath).fileName());
                const auto selected=dialog->selectedFiles();
                if(selected.size()!=1||absolutePath(selected.front())!=targetPath)continue;
                acceptedPath=absolutePath(selected.front());fileDialogAccepted=true;
                QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection);
            }else if(auto message=qobject_cast<QMessageBox*>(widget)){
                // macOS may suppress QMessageBox titles. Its icon and concrete
                // output path distinguish completion from warnings and errors.
                if(message->icon()==QMessageBox::Question){
                    if(auto yes=message->button(QMessageBox::Yes))QMetaObject::invokeMethod(yes,"click",Qt::QueuedConnection);
                    continue;
                }
                completionSeen=true;completionTitle=message->windowTitle();completionText=message->text();completionIcon=int(message->icon());
                QMetaObject::invokeMethod(message,"accept",Qt::QueuedConnection);
            }
        }
    });
    responder.start();
    if(toolbarButton)QTest::mouseClick(toolbarButton,Qt::LeftButton);else action->trigger();
    while(waitCompletion&&!completionSeen&&elapsed.elapsed()<30000)QTest::qWait(20);
    QTest::qWait(50);responder.stop();
    const bool completionMatches=successTitle==QStringLiteral("拆分导出完成")
        ?completionText.contains(QStringLiteral("已导出"))&&completionText.contains(targetPath+"/STP_parts_")
        :absolutePath(completionText)==targetPath;
    return {{"passed",fileDialogAccepted&&(!waitCompletion||(completionSeen&&completionIcon==int(QMessageBox::Information)
            &&completionMatches))&&!QApplication::activeModalWidget()},
        {"dialogAccepted",fileDialogAccepted},{"completionTitle",completionTitle},{"completionText",completionText},
        {"completionIcon",completionIcon},{"completionMatches",completionMatches},{"acceptedPath",acceptedPath},
        {"fileNameEditorPresent",fileNameEditorPresent},{"target",target}};
}

QByteArray xlsxEntry(const QString& path,const QByteArray& entryName) {
    QFile file(path); require(file.open(QIODevice::ReadOnly),"cannot open workbook entry");
    const QByteArray bytes=file.readAll();
    const auto word=[&](qsizetype offset) {
        require(offset>=0 && offset+2<=bytes.size(),"truncated XLSX ZIP word");
        return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(bytes.constData()+offset));
    };
    const auto dword=[&](qsizetype offset) {
        require(offset>=0 && offset+4<=bytes.size(),"truncated XLSX ZIP dword");
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData()+offset));
    };
    const qsizetype end=bytes.lastIndexOf("PK\005\006");
    require(end>=0,"XLSX ZIP end missing");
    qsizetype position=dword(end+16);
    for(int i=0;i<word(end+10);++i) {
        require(bytes.mid(position,4)==QByteArray("PK\001\002",4),"invalid XLSX central entry");
        const auto nameSize=word(position+28), extraSize=word(position+30), commentSize=word(position+32);
        if(bytes.mid(position+46,nameSize)==entryName) {
            const auto method=word(position+10);
            const auto compressedSize=dword(position+20), size=dword(position+24), crc=dword(position+16);
            const qsizetype local=dword(position+42);
            require(bytes.mid(local,4)==QByteArray("PK\003\004",4),"invalid XLSX local entry");
            const qsizetype data=local+30+word(local+26)+word(local+28);
            require(data>=0 && data+compressedSize<=bytes.size() && size<16*1024*1024,"invalid test XLSX entry size");
            QByteArray result;
            if(method==0) result=bytes.mid(data,compressedSize);
            else {
                require(method==8,"unexpected XLSX ZIP compression");
                result.resize(size);
                z_stream stream {};
                stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(bytes.constData()+data));
                stream.avail_in=compressedSize;
                stream.next_out=reinterpret_cast<Bytef*>(result.data()); stream.avail_out=size;
                require(inflateInit2(&stream,-MAX_WBITS)==Z_OK,"XLSX inflate initialization failed");
                const int status=inflate(&stream,Z_FINISH);
                const bool valid=status==Z_STREAM_END && stream.total_out==size;
                inflateEnd(&stream);
                require(valid,"XLSX entry decompression failed");
            }
            require(result.size()==size && crc32(0,reinterpret_cast<const Bytef*>(result.constData()),size)==crc,
                    "XLSX ZIP entry integrity failed");
            return result;
        }
        position+=46+nameSize+extraSize+commentSize;
    }
    throw std::runtime_error("XLSX entry not found");
}

QMap<QString,QString> xlsxSheetCells(const QString& path,const QByteArray& sheetName="xl/worksheets/sheet1.xml") {
    QStringList strings;
    QXmlStreamReader shared(xlsxEntry(path,"xl/sharedStrings.xml"));
    while(!shared.atEnd()) {
        shared.readNext();
        if(shared.isStartElement() && shared.name()==QStringLiteral("si")) {
            QString text;
            while(!shared.atEnd()) {
                shared.readNext();
                if(shared.isEndElement() && shared.name()==QStringLiteral("si")) break;
                if(shared.isStartElement() && shared.name()==QStringLiteral("t")) text+=shared.readElementText();
            }
            strings<<text;
        }
    }
    require(!shared.hasError(),"invalid workbook shared strings XML");
    QMap<QString,QString> cells;
    QXmlStreamReader sheet(xlsxEntry(path,sheetName));
    while(!sheet.atEnd()) {
        sheet.readNext();
        if(sheet.isStartElement() && sheet.name()==QStringLiteral("c")) {
            const QString reference=sheet.attributes().value(QStringLiteral("r")).toString();
            const bool sharedValue=sheet.attributes().value(QStringLiteral("t"))==QStringLiteral("s");
            QString value;
            while(!sheet.atEnd()) {
                sheet.readNext();
                if(sheet.isEndElement() && sheet.name()==QStringLiteral("c")) break;
                if(sheet.isStartElement() && sheet.name()==QStringLiteral("v")) value=sheet.readElementText();
            }
            if(sharedValue) {
                bool ok=false; const int index=value.toInt(&ok);
                require(ok && index>=0 && index<strings.size(),"invalid workbook string index");
                value=strings[index];
            }
            cells.insert(reference,value);
        }
    }
    require(!sheet.hasError(),"invalid workbook worksheet XML");
    return cells;
}

} // namespace

int main(int argc,char** argv)
{
    Mayo::QtOpenGlUtils::platformSetup(argc,argv);
#ifdef Q_OS_MACOS
    auto format=QSurfaceFormat::defaultFormat();format.setVersion(4,1);format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
#endif
    qputenv("CSF_ShadersDirectory",QByteArray(STP_OCCT_RESOURCE_DIR)+"/Shaders");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);app.setStyle(QStyleFactory::create("Fusion"));
    auto palette=app.palette();
    for(const auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText})palette.setColor(role,QColor("#233a56"));
    palette.setColor(QPalette::Window,QColor("#eef2f7"));palette.setColor(QPalette::Base,Qt::white);
    palette.setColor(QPalette::Button,QColor("#f0f4f9"));palette.setColor(QPalette::PlaceholderText,QColor("#60758b"));app.setPalette(palette);
    if(argc<2||argc>3){std::cerr<<"Usage: stp_workspace_tests output-directory [repository-directory]\n";return 2;}
    const QString output=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();QDir().mkpath(output);
    const QDir repository(argc==3?QString::fromLocal8Bit(argv[2]):QDir::currentPath());
    QString cube=repository.absoluteFilePath("samples/cube.step");
    if(!QFileInfo::exists(cube))cube=repository.absoluteFilePath("tests/inputs/cube.step");
    const QString assembly=repository.absoluteFilePath("samples/colored-assembly.step");
    const QString pmi=repository.absoluteFilePath("samples/pmi-basic.step");
    if(!QFileInfo::exists(cube)||!QFileInfo::exists(assembly)||!QFileInfo::exists(pmi)){
        std::cerr<<"Workspace test STEP fixtures missing\n";return 2;
    }
    QJsonObject report;bool passed=true;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    QJsonArray cubeInitialDisplay;
#endif
    Stp::DocumentWorkspace workspace;workspace.resize(1440,831);workspace.show();QTest::qWait(200);
    auto tabs=workspace.findChild<QTabWidget*>("documentTabs");
    auto openAction=workspace.findChild<QAction*>("workspaceOpen");
    auto closeAction=workspace.findChild<QAction*>("workspaceCloseCurrent");
    auto closeAllAction=workspace.findChild<QAction*>("workspaceCloseAll");
    auto shellBom=workspace.findChild<QAction*>("workspaceExportBom");
    auto shellParts=workspace.findChild<QAction*>("workspaceExportParts");
    if(!tabs||!openAction||!closeAction||!closeAllAction||!shellBom||!shellParts||!workspace.jobQueue()){
        std::cerr<<"Workspace navigation or queue missing\n";return 1;
    }
    const bool emptyWorkspace=workspace.documentCount()==0&&!workspace.currentDocument()
        &&!closeAction->isEnabled()&&!closeAllAction->isEnabled()&&openAction->isEnabled();
    report["emptyWorkspace"]=emptyWorkspace;passed=passed&&emptyWorkspace;
    workspace.openFiles({cube,assembly,pmi,assembly});QTest::qWait(30);
    QPointer<Stp::MainWindow> cubePage=documentAt(tabs,0),assemblyPage=documentAt(tabs,1),pmiPage=documentAt(tabs,2);
    const bool openedThree=workspace.documentCount()==3&&tabs->count()==3&&cubePage&&assemblyPage&&pmiPage;
    const auto initiallyCurrent=workspace.currentDocument();
    const bool parsingFinished=waitFor([&]{return workspace.jobQueue()->idle();});
    const bool completionDoesNotStealTab=workspace.currentDocument()==initiallyCurrent;
    bool loaded=openedThree&&parsingFinished;
    for(auto page:{cubePage.data(),assemblyPage.data(),pmiPage.data()})loaded=page&&activate(tabs,page)&&loaded;
    report["multiFileOpen"]=QJsonObject{{"passed",loaded&&completionDoesNotStealTab},{"tabs",workspace.documentCount()},
        {"completionDoesNotStealTab",completionDoesNotStealTab},{"queueDrained",parsingFinished}};
    passed=passed&&loaded&&completionDoesNotStealTab;
#ifdef Q_OS_WIN
    workspace.openFiles({assembly.toUpper()});
    const bool caseAlias=workspace.documentCount()==3&&workspace.currentDocument()==assemblyPage;
    report["windowsCaseInsensitivePathAlias"]=caseAlias;passed=passed&&caseAlias;
#endif
    if(loaded){
        activate(tabs,assemblyPage);auto assemblyView=assemblyPage->findChild<Stp::ViewerWidget*>();
        auto assemblyBom=assemblyPage->findChild<QTableWidget*>("bomTable");
        auto assemblyProperties=assemblyPage->findChild<QTableWidget*>("engineeringProperties");
        auto assemblyTree=assemblyPage->findChild<QTreeWidget*>("assemblyTree");
        const auto assemblySource=sourceTransforms(*assemblyPage->document());
        const int missingPart=1;
        selectPart(assemblyPage,missingPart);
        QJsonObject manualFields;
        manualFields["assemblyMaterialBefore"]=fieldState(assemblyProperties,propertyValue(assemblyProperties,QStringLiteral("材料")));
        const bool assemblyMaterialEdited=edit(assemblyProperties,propertyValue(assemblyProperties,QStringLiteral("材料")),QStringLiteral("钢"));
        manualFields["assemblyMaterialEditorCommitted"]=assemblyMaterialEdited;
        manualFields["assemblyMaterialAfter"]=fieldState(assemblyProperties,propertyValue(assemblyProperties,QStringLiteral("材料")));
        manualFields["assemblyDensityBefore"]=fieldState(assemblyProperties,propertyValue(assemblyProperties,QStringLiteral("密度 kg/m³")));
        const bool assemblyDensityEdited=edit(assemblyProperties,propertyValue(assemblyProperties,QStringLiteral("密度 kg/m³")),QStringLiteral("7800"));
        manualFields["assemblyDensityEditorCommitted"]=assemblyDensityEdited;
        manualFields["assemblyDensityAfter"]=fieldState(assemblyProperties,propertyValue(assemblyProperties,QStringLiteral("密度 kg/m³")));
        manualFields["assemblyBomMass"]=fieldState(assemblyBom,valueForPart(assemblyBom,1,10));
        const bool assemblyManual=assemblyMaterialEdited&&assemblyDensityEdited;
        auto uncheckedItem=occurrenceItem(assemblyTree,firstOccurrence(*assemblyPage->document(),0));
        const bool scoped=uncheck(assemblyTree,uncheckedItem)&&number(valueForPart(assemblyBom,0,2),4.);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        const auto assemblyPlacement=placeThroughGrabButton(assemblyPage,true);
#endif
        assemblyPage->findChild<QSlider*>("explosionSlider")->setValue(54);
        assemblyPage->findChild<QSlider*>("transparencySlider")->setValue(28);
        assemblyView->standardView(5);assemblyView->fitAll();QTest::qWait(80);
        const Handle(Graphic3d_Camera) assemblyCamera=new Graphic3d_Camera(assemblyView->view()->Camera());
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        const gp_Trsf assemblyPlaced=assemblyPlacement.object.IsNull()?gp_Trsf():assemblyPlacement.object->LocalTransformation();
#endif
        assemblyPage->findChild<QAction*>("showBomPage")->setChecked(true);QTest::qWait(50);

        activate(tabs,cubePage);auto cubeView=cubePage->findChild<Stp::ViewerWidget*>();
        auto cubeBom=cubePage->findChild<QTableWidget*>("bomTable");
        auto cubeProperties=cubePage->findChild<QTableWidget*>("engineeringProperties");
        const auto cubeSource=sourceTransforms(*cubePage->document());
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        cubeInitialDisplay=displayTransforms(cubeView);
#endif
        selectPart(cubePage,0);
        manualFields["cubeMaterialBefore"]=fieldState(cubeProperties,propertyValue(cubeProperties,QStringLiteral("材料")));
        const bool cubeManual=edit(cubeProperties,propertyValue(cubeProperties,QStringLiteral("材料")),QStringLiteral("铜"));
        manualFields["cubeMaterialEditorCommitted"]=cubeManual;
        manualFields["cubeMaterialAfter"]=fieldState(cubeProperties,propertyValue(cubeProperties,QStringLiteral("材料")));
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        const auto cubePlacement=placeThroughGrabButton(cubePage,false);
#endif
        cubePage->findChild<QSlider*>("explosionSlider")->setValue(23);
        cubePage->findChild<QSlider*>("transparencySlider")->setValue(6);
        cubeView->standardView(1);cubeView->fitAll();QTest::qWait(80);
        const Handle(Graphic3d_Camera) cubeCamera=new Graphic3d_Camera(cubeView->view()->Camera());
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        const gp_Trsf cubePlaced=cubePlacement.object.IsNull()?gp_Trsf():cubePlacement.object->LocalTransformation();
#endif
        manualFields["cubePendingDensityBefore"]=fieldState(cubeProperties,propertyValue(cubeProperties,QStringLiteral("密度 kg/m³")));
        const bool pendingEditor=beginEdit(cubeProperties,propertyValue(cubeProperties,QStringLiteral("密度 kg/m³")),QStringLiteral("8960"));
        manualFields["pendingCubeDensityEditorOpened"]=pendingEditor;
        activate(tabs,assemblyPage);
        const bool pendingCommitted=pendingEditor&&!editorFor(cubeProperties)&&number(valueForPart(cubeBom,0,4),8960.);
        manualFields["cubeBomDensityAfterSwitch"]=fieldState(cubeBom,valueForPart(cubeBom,0,4));
        bool assemblyGrabPreserved=true;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        assemblyGrabPreserved=assemblyPlacement.passed&&assemblyView->grabMode()
            &&assemblyPage->findChild<QAction*>("togglePartGrab")->isChecked()
            &&sameTransform(assemblyPlaced,assemblyPlacement.object->LocalTransformation())
            &&sourceTransforms(*assemblyPage->document())==assemblySource;
#endif
        const bool assemblyPreserved=assemblyManual&&scoped&&assemblyGrabPreserved&&number(valueForPart(assemblyBom,0,2),4.)
            &&valueForPart(assemblyBom,1,3)&&valueForPart(assemblyBom,1,3)->text()==QStringLiteral("钢")
            &&number(valueForPart(assemblyBom,1,4),7800.)&&number(valueForPart(assemblyBom,1,10),.000975)
            &&assemblyPage->findChild<QSlider*>("explosionSlider")->value()==54
            &&assemblyPage->findChild<QSlider*>("transparencySlider")->value()==28
            &&uncheckedItem->checkState(0)==Qt::Unchecked
            &&assemblyPage->findChild<QStackedWidget*>("workspacePages")->currentIndex()==1
            &&cameraMatches(assemblyView->view()->Camera(),assemblyCamera);
        const int countBeforeDuplicate=workspace.documentCount();
        const QString canonicalAlias=QFileInfo(assembly).absolutePath()+"/../samples/./colored-assembly.step";
        workspace.openFiles({canonicalAlias,assembly});QTest::qWait(100);
        const bool duplicatePreserved=workspace.documentCount()==countBeforeDuplicate&&workspace.currentDocument()==assemblyPage
            &&number(valueForPart(assemblyBom,1,4),7800.)&&number(valueForPart(assemblyBom,0,2),4.)
            &&cameraMatches(assemblyView->view()->Camera(),assemblyCamera)
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
            &&assemblyPlacement.passed&&assemblyView->grabMode()&&sameTransform(assemblyPlaced,assemblyPlacement.object->LocalTransformation())
#endif
            ;
        assemblyPage->findChild<QAction*>("showBomPage")->setChecked(false);
        activate(tabs,cubePage);
        const auto cubeVolume=cubePage->document()->parts.front().volumeMm3;
        bool cubeGrabPreserved=true;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        cubeGrabPreserved=cubePlacement.passed&&!cubeView->grabMode()
            &&!cubePage->findChild<QAction*>("togglePartGrab")->isChecked()
            &&sameTransform(cubePlaced,cubePlacement.object->LocalTransformation())
            &&sourceTransforms(*cubePage->document())==cubeSource;
#endif
        const bool cubePreserved=cubeManual&&pendingCommitted&&cubeGrabPreserved&&valueForPart(cubeBom,0,3)
            &&valueForPart(cubeBom,0,3)->text()==QStringLiteral("铜")&&number(valueForPart(cubeBom,0,4),8960.)
            &&cubeVolume&&number(valueForPart(cubeBom,0,10),*cubeVolume*8960./1.e9)
            &&cubePage->findChild<QSlider*>("explosionSlider")->value()==23
            &&cubePage->findChild<QSlider*>("transparencySlider")->value()==6
            &&cameraMatches(cubeView->view()->Camera(),cubeCamera);
        report["manualFields"]=manualFields;
        // The native menu holds current-document actions only. Triggering its
        // page action must not alter another session's stacked page.
        QMenu* shellViewMenu=nullptr;
#if defined(Q_OS_MACOS)
        const bool nativeMenuOwned=workspace.nativeMenuBar()->isNativeMenuBar()
            &&workspace.nativeMenuBar()->parentWidget()==nullptr;
#else
        const bool nativeMenuOwned=!workspace.nativeMenuBar()->isNativeMenuBar()
            &&workspace.nativeMenuBar()->parentWidget()==&workspace&&workspace.nativeMenuBar()->isVisible();
#endif
        report["nativeMenuOwnership"]=nativeMenuOwned;passed=passed&&nativeMenuOwned;
        for(auto action:workspace.nativeMenuBar()->actions())if(action->menu()&&action->text()==QStringLiteral("视图"))shellViewMenu=action->menu();
        auto cubeBomPageAction=cubePage->findChild<QAction*>("showBomPage");
        const bool currentMenuBound=shellViewMenu&&shellViewMenu->actions().contains(cubeBomPageAction)
            &&!shellViewMenu->actions().contains(assemblyPage->findChild<QAction*>("showBomPage"));
        if(currentMenuBound)cubeBomPageAction->trigger();
        const bool menuForwarded=currentMenuBound&&cubePage->findChild<QStackedWidget*>("workspacePages")->currentIndex()==1
            &&assemblyPage->findChild<QStackedWidget*>("workspacePages")->currentIndex()==0;
        cubeBomPageAction->setChecked(false);report["currentDocumentMenuForwarding"]=menuForwarded;passed=passed&&menuForwarded;
        activate(tabs,pmiPage);auto pmiView=pmiPage->findChild<Stp::ViewerWidget*>();
        const Handle(Graphic3d_Camera) pmiCamera=new Graphic3d_Camera(pmiView->view()->Camera());
        pmiPage->findChild<QAction*>("togglePmi")->setChecked(false);
        QJsonArray frames;bool switchingRenders=true,camerasPreserved=true;
        for(int cycle=0;cycle<3;++cycle)for(auto page:{cubePage.data(),assemblyPage.data(),pmiPage.data()}){
            switchingRenders=activate(tabs,page)&&switchingRenders;
            const QString tag=page==cubePage?"cube":page==assemblyPage?"assembly":"pmi";
            auto frame=renderedModel(page,output+QString("/switch-%1-%2.png").arg(cycle).arg(tag));frames.append(frame);
            switchingRenders=frame["passed"].toBool()&&switchingRenders;
            const auto actual=page->findChild<Stp::ViewerWidget*>()->view()->Camera();
            camerasPreserved=cameraMatches(actual,page==cubePage?cubeCamera:page==assemblyPage?assemblyCamera:pmiCamera)&&camerasPreserved;
        }
        const bool pmiIndependent=!pmiPage->findChild<QAction*>("togglePmi")->isChecked()
            &&cubePage->findChild<QAction*>("togglePmi")->isChecked()&&assemblyPage->findChild<QAction*>("togglePmi")->isChecked();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        const bool grabModesIndependent=assemblyView->grabMode()&&!cubeView->grabMode()&&!pmiView->grabMode();
        const bool placedPositionsRetained=assemblyPlacement.passed&&cubePlacement.passed
            &&sameTransform(assemblyPlaced,assemblyPlacement.object->LocalTransformation())
            &&sameTransform(cubePlaced,cubePlacement.object->LocalTransformation());
        report["partPlacementSessions"]=QJsonObject{{"passed",grabModesIndependent&&placedPositionsRetained},
            {"grabModesIndependent",grabModesIndependent},{"placementSurvivesPageAndTabSwitch",placedPositionsRetained},
            {"sourceTransformsUnchanged",sourceTransforms(*assemblyPage->document())==assemblySource&&sourceTransforms(*cubePage->document())==cubeSource}};
        passed=passed&&grabModesIndependent&&placedPositionsRetained;
#endif
        workspace.grab().save(output+"/workspace-three-documents.png");
        report["sessionIndependence"]=QJsonObject{{"passed",assemblyPreserved&&cubePreserved&&pmiIndependent},
            {"assemblyPreserved",assemblyPreserved},{"cubePreserved",cubePreserved},{"pendingEditorCommittedOnSwitch",pendingCommitted},
            {"pmiVisibilityIndependent",pmiIndependent}};
        report["duplicateCanonicalPath"]=duplicatePreserved;report["switching"]=QJsonObject{{"passed",switchingRenders&&camerasPreserved},
            {"cameraStatePreserved",camerasPreserved},{"frames",frames}};
        passed=passed&&assemblyPreserved&&cubePreserved&&pmiIndependent&&duplicatePreserved&&switchingRenders&&camerasPreserved;

        const auto cubeState=sessionState(cubePage),assemblyState=sessionState(assemblyPage),pmiState=sessionState(pmiPage);
        QJsonArray resizeScenarios;bool resizingPassed=true;
        for(const auto size:{QSize(1280,760),QSize(1000,700)}){
            workspace.resize(size);QTest::qWait(100);
            for(int cycle=0;cycle<2;++cycle)for(auto page:{cubePage.data(),assemblyPage.data(),pmiPage.data()}){
                const bool activated=activate(tabs,page);
                const QString tag=page==cubePage?"cube":page==assemblyPage?"assembly":"pmi";
                const QString screenshotBase=output+QString("/resize-%1x%2-%3-%4").arg(size.width()).arg(size.height()).arg(cycle).arg(tag);
                auto controls=reachableControls(page);
                auto frame=renderedModel(page,screenshotBase+"-model.png");
                const bool windowScreenshot=workspace.grab().save(screenshotBase+"-window.png");
                const auto expectedState=page==cubePage?cubeState:page==assemblyPage?assemblyState:pmiState;
                const auto expectedCamera=page==cubePage?cubeCamera:page==assemblyPage?assemblyCamera:pmiCamera;
                const bool statePreserved=sessionState(page)==expectedState;
                const bool cameraPreserved=cameraMatches(page->findChild<Stp::ViewerWidget*>()->view()->Camera(),expectedCamera);
                const bool sizeHonored=workspace.size()==size;
                const bool scenarioPassed=activated&&controls.value("passed").toBool()&&frame.value("passed").toBool()
                    &&windowScreenshot&&statePreserved&&cameraPreserved&&sizeHonored;
                resizeScenarios.append(QJsonObject{{"passed",scenarioPassed},{"document",tag},{"cycle",cycle},
                    {"requestedSize",QJsonArray{size.width(),size.height()}},
                    {"actualSize",QJsonArray{workspace.width(),workspace.height()}},{"sizeHonored",sizeHonored},
                    {"statePreserved",statePreserved},{"cameraStatePreserved",cameraPreserved},
                    {"controls",controls},{"frame",frame},{"windowScreenshot",windowScreenshot},
                    {"windowScreenshotPath",screenshotBase+"-window.png"}});
                resizingPassed=resizingPassed&&scenarioPassed;
            }
        }
        workspace.resize(1440,831);QTest::qWait(100);
        const bool sizeRestored=workspace.size()==QSize(1440,831);
        bool statesRestored=true;
        for(auto page:{cubePage.data(),assemblyPage.data(),pmiPage.data()}){
            statesRestored=activate(tabs,page)&&statesRestored;
            const auto expectedState=page==cubePage?cubeState:page==assemblyPage?assemblyState:pmiState;
            const auto expectedCamera=page==cubePage?cubeCamera:page==assemblyPage?assemblyCamera:pmiCamera;
            statesRestored=sessionState(page)==expectedState
                &&cameraMatches(page->findChild<Stp::ViewerWidget*>()->view()->Camera(),expectedCamera)&&statesRestored;
        }
        report["embeddedWorkspaceResize"]=QJsonObject{{"passed",resizingPassed&&sizeRestored&&statesRestored},
            {"restored1440x831",sizeRestored},{"allSessionStatesPreservedAfterRestore",statesRestored},{"scenarios",resizeScenarios}};
        passed=passed&&resizingPassed&&sizeRestored&&statesRestored;

        QJsonObject exports;bool exportRouting=true;
        const auto numeric=[](const QString& cell,double expected){
            bool ok=false;const double actual=cell.toDouble(&ok);
            return ok&&std::abs(actual-expected)<std::max(1.e-12,std::abs(expected)*1.e-8);
        };
        for(auto page:{cubePage.data(),assemblyPage.data()}){
            const bool cubeDocument=page==cubePage;const QString tag=cubeDocument?"cube":"assembly";
            const bool currentActivated=activate(tabs,page)&&workspace.currentDocument()==page;
            const QString bomPath=output+"/"+tag+"-current.xlsx";QFile::remove(bomPath);
            auto bomResult=exportThroughUi(shellBom,bomPath,QStringLiteral("导出完成"));
            bool workbookCorrect=false;QString workbookError;
            try {
                const auto cells=xlsxSheetCells(bomPath),positions=xlsxSheetCells(bomPath,"xl/worksheets/sheet3.xml");
                const auto diagnostics=xlsxSheetCells(bomPath,"xl/worksheets/sheet4.xml");
                const auto workbook=xlsxEntry(bomPath,"xl/workbook.xml");
                const bool fourSheets=workbook.contains(QStringLiteral("零件汇总").toUtf8())
                    &&workbook.contains(QStringLiteral("装配层级").toUtf8())&&workbook.contains(QStringLiteral("实例位置").toUtf8())
                    &&workbook.contains(QStringLiteral("导入诊断").toUtf8());
                const bool sourceMatches=QFileInfo(diagnostics.value("B2")).canonicalFilePath()
                    ==QFileInfo(page->document()->sourcePath).canonicalFilePath();
                bool sourcePositions=true;int positionRows=0;
                for(int row=2;positions.contains(QString("A%1").arg(row));++row){
                    const auto id=positions.value(QString("A%1").arg(row));
                    const auto found=std::find_if(page->document()->occurrences.begin(),page->document()->occurrences.end(),
                        [&id](const Stp::Occurrence& occurrence){return occurrence.id==id;});
                    if(found==page->document()->occurrences.end()){sourcePositions=false;continue;}
                    for(int axis=1;axis<=3;++axis)
                        sourcePositions=numeric(positions.value(QString("%1%2").arg(QChar('E'+axis)).arg(row)),found->worldTransform.Value(axis,4))&&sourcePositions;
                    for(int axis=1;axis<=3;++axis)for(int column=1;column<=3;++column)
                        sourcePositions=numeric(positions.value(QString("%1%2").arg(QChar('I'+(axis-1)*3+column-1)).arg(row)),found->worldTransform.Value(axis,column))&&sourcePositions;
                    ++positionRows;
                }
                sourcePositions=sourcePositions&&positionRows>0&&sourceTransforms(*page->document())==(cubeDocument?cubeSource:assemblySource);
                workbookCorrect=fourSheets&&sourceMatches&&sourcePositions&&(cubeDocument
                    ?cells.value("E2")=="1"&&cells.value("F2")==QStringLiteral("铜")&&numeric(cells.value("G2"),8960.)
                        &&cubeVolume&&numeric(cells.value("M2"),*cubeVolume*8960./1.e9)&&!cells.contains("A3")
                        &&positions.contains("A2")&&!positions.contains("A3")
                    :cells.value("E2")=="4"&&cells.value("F2")=="Aluminium"&&numeric(cells.value("G2"),2700.)
                        &&cells.value("E3")=="2"&&cells.value("F3")==QStringLiteral("钢")&&numeric(cells.value("G3"),7800.)
                        &&numeric(cells.value("M3"),.000975)&&!cells.contains("A4")
                        &&positions.contains("A7")&&!positions.contains("A8"));
                bomResult.insert("sourceMatches",sourceMatches);bomResult.insert("fourWorksheets",fourSheets);
                bomResult.insert("originalInstancePositionsVerified",sourcePositions);
                bomResult.insert("material",cells.value(cubeDocument?"F2":"F3"));
                bomResult.insert("density",cells.value(cubeDocument?"G2":"G3"));
                bomResult.insert("unitMass",cells.value(cubeDocument?"M2":"M3"));
            }catch(const std::exception& error){workbookError=QString::fromUtf8(error.what());}
            bomResult.insert("workbookValuesCorrect",workbookCorrect);bomResult.insert("error",workbookError);
            const QString partsPath=output+"/"+tag+"-current-parts";QDir().mkpath(partsPath);
            const auto priorDirectories=QDir(partsPath).entryList(QDir::Dirs|QDir::NoDotAndDotDot);
            auto partsResult=exportThroughUi(shellParts,partsPath,QStringLiteral("拆分导出完成"));
            QString resultDirectory;
            for(const auto& name:QDir(partsPath).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
                if(!priorDirectories.contains(name))resultDirectory=QDir(partsPath).filePath(name);
            bool manifestCorrect=false,stepReadbackCorrect=false;QString partsError;
            try {
                const auto manifest=xlsxSheetCells(QDir(resultDirectory).filePath("manifest.xlsx"));
                manifestCorrect=manifest.value("E1")==QStringLiteral("本次勾选数量")&&(cubeDocument
                    ?manifest.value("E2")=="1"&&!manifest.contains("A3")
                    :manifest.value("E2")=="4"&&manifest.value("E3")=="2"&&!manifest.contains("A4"));
                const auto files=QDir(resultDirectory).entryList({"*.step","*.stp"},QDir::Files);
                bool cubeReadBack=false,smallReadBack=false,largeReadBack=false;std::atomic_bool readCancel(false);
                for(const auto& filename:files){
                    const auto readback=Stp::loadStep(QDir(resultDirectory).filePath(filename),{},readCancel);
                    if(readback->parts.size()!=1||!readback->parts.front().volumeMm3)continue;
                    const auto& part=readback->parts.front();const double volume=*part.volumeMm3;
                    if(cubeDocument)cubeReadBack=cubeVolume&&std::abs(volume-*cubeVolume)<1.e-6
                        &&part.material.isEmpty()&&!part.densityKgM3;
                    else if(std::abs(volume-125.)<1.e-6)smallReadBack=part.material.isEmpty()&&!part.densityKgM3;
                    else if(std::abs(volume-6000.)<1.e-6)largeReadBack=part.material=="Aluminium"
                        &&part.densityKgM3&&std::abs(*part.densityKgM3-2700.)<1.e-8;
                }
                stepReadbackCorrect=cubeDocument?files.size()==1&&cubeReadBack:files.size()==2&&smallReadBack&&largeReadBack;
                partsResult.insert("stepFileCount",files.size());
            }catch(const std::exception& error){partsError=QString::fromUtf8(error.what());}
            partsResult.insert("resultDirectory",resultDirectory);partsResult.insert("manifestScopeCorrect",manifestCorrect);
            partsResult.insert("originalGeometryAndAttributesVerified",stepReadbackCorrect);partsResult.insert("error",partsError);
            const bool routed=currentActivated&&bomResult.value("passed").toBool()&&workbookCorrect
                &&partsResult.value("passed").toBool()&&manifestCorrect&&stepReadbackCorrect&&workspace.currentDocument()==page;
            exports[tag]=QJsonObject{{"passed",routed},{"bom",bomResult},{"parts",partsResult}};
            exportRouting=exportRouting&&routed;
        }
        exports.insert("passed",exportRouting);report["currentDocumentExportRouting"]=exports;passed=passed&&exportRouting;
    }

    QTemporaryDir sameNames;QDir().mkpath(sameNames.path()+"/a");QDir().mkpath(sameNames.path()+"/b");
    const QString sameA=sameNames.path()+"/a/同名.step",sameB=sameNames.path()+"/b/同名.step";
    const bool copiesCreated=QFile::copy(cube,sameA)&&QFile::copy(cube,sameB);
    const int countBeforeCopies=workspace.documentCount();workspace.openFiles({sameA,sameB});QTest::qWait(50);
    QPointer<Stp::MainWindow> copyA=documentAt(tabs,countBeforeCopies),copyB=documentAt(tabs,countBeforeCopies+1);
    const bool sameNameDistinct=copiesCreated&&workspace.documentCount()==countBeforeCopies+2&&copyA&&copyB
        &&activate(tabs,copyA)&&activate(tabs,copyB)&&copyA!=copyB
        &&QFileInfo(copyA->document()->sourcePath).canonicalFilePath()!=QFileInfo(copyB->document()->sourcePath).canonicalFilePath();
    report["sameBasenameDifferentDirectories"]=sameNameDistinct;passed=passed&&sameNameDistinct;
    if(copyA&&copyB){
        activate(tabs,copyA);const auto activeBefore=workspace.currentDocument();workspace.closeDocument(copyB);
        const bool inactiveClosed=waitFor([&]{return workspace.documentCount()==countBeforeCopies+1&&copyB.isNull();})
            &&workspace.currentDocument()==activeBefore;
        // Cocoa modal export dialogs can restore focus asynchronously. A fixed
        // 30 ms wait is insufficient for a window-context shortcut; wait for
        // actual activation and type the action's platform-native sequence.
        workspace.raise();workspace.activateWindow();
        const bool windowActivated=QTest::qWaitForWindowActive(&workspace,5000);workspace.setFocus();
        const bool closeEnabled=closeAction->isEnabled();const auto closeShortcut=closeAction->shortcut();
        QSignalSpy closeTriggered(closeAction,&QAction::triggered);
        QTest::keySequence(&workspace,closeShortcut);
        const bool currentClosed=waitFor([&]{return workspace.documentCount()==countBeforeCopies&&copyA.isNull();})
            &&windowActivated&&closeEnabled&&closeTriggered.count()==1;
        report["closeInactiveAndCurrent"]=QJsonObject{{"passed",inactiveClosed&&currentClosed},{"inactiveClosed",inactiveClosed},{"currentClosed",currentClosed},
            {"windowActivatedForShortcut",windowActivated},{"closeActionEnabled",closeEnabled},{"closeShortcut",closeShortcut.toString(QKeySequence::PortableText)},
            {"closeShortcutTriggeredCount",closeTriggered.count()},{"documentCountAfterShortcut",workspace.documentCount()},{"expectedDocumentCount",countBeforeCopies},
            {"currentPageDestroyed",copyA.isNull()}};
        passed=passed&&inactiveClosed&&currentClosed;
    }else passed=false;
    closeAllAction->trigger();const bool allClosed=waitFor([&]{return workspace.documentCount()==0&&workspace.jobQueue()->idle();})
        &&waitFor([&]{return cubePage.isNull()&&assemblyPage.isNull()&&pmiPage.isNull();})
        &&!workspace.currentDocument()&&!closeAction->isEnabled()&&!closeAllAction->isEnabled();
    workspace.openFiles({cube});QTest::qWait(30);auto reopened=workspace.currentDocument();
    const bool reopen=allClosed&&reopened&&activate(tabs,reopened)&&workspace.documentCount()==1
        &&!reopened->document()->parts.front().manualMaterial&&!reopened->document()->parts.front().manualDensityKgM3
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        &&!reopened->findChild<Stp::ViewerWidget*>()->grabMode()&&!reopened->findChild<QAction*>("togglePartGrab")->isChecked()
        &&displayTransforms(reopened->findChild<Stp::ViewerWidget*>())==cubeInitialDisplay
#endif
        ;
    report["closeAllAndReopen"]=QJsonObject{{"passed",allClosed&&reopen},{"allClosed",allClosed},{"reopenedWithoutOldManualOverrides",reopen}};
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    auto reopenedReport=report["closeAllAndReopen"].toObject();reopenedReport.insert("grabModeAndPlacementCleared",reopen);report["closeAllAndReopen"]=reopenedReport;
#endif
    passed=passed&&allClosed&&reopen;
    workspace.closeAll();waitFor([&]{return workspace.documentCount()==0&&workspace.jobQueue()->idle();});

    // Atomic gates make queued/active cancellation deterministic without
    // relying on STEP size, disk timing, or a particular CPU's mesh speed.
    auto jobs=workspace.jobQueue();
    auto gate=std::make_shared<std::atomic_bool>(false),entered=std::make_shared<std::atomic_bool>(false);
    constexpr quint64 blockerOwner=900001;
    jobs->enqueue(blockerOwner,[gate,entered](std::atomic_bool& cancel,const Stp::ProgressCallback&){
        entered->store(true);
        while(!gate->load()&&!cancel.load())std::this_thread::sleep_for(std::chrono::milliseconds(2));
    },{},[](const QString&){});
    const bool blockerRunning=waitFor([&]{return entered->load();});
    workspace.openFiles({cube,assembly});QTest::qWait(30);
    QPointer<Stp::MainWindow> queuedSurvivor=documentAt(tabs,0),queuedClosed=documentAt(tabs,1);
    const quint64 queuedOwner=queuedClosed?queuedClosed->sessionId():0;
    const bool queuedImportExists=queuedClosed&&queuedClosed->isBusy()&&jobs->hasJobs(queuedOwner)
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        &&!queuedClosed->findChild<QAction*>("togglePartGrab")->isEnabled()
#endif
        ;
    if(queuedClosed)tabs->setCurrentWidget(queuedClosed);
    QJsonObject queuedDebug{{"closedSession",double(queuedOwner)},
        {"survivorSession",double(queuedSurvivor?queuedSurvivor->sessionId():0)},
        {"closedTabIndexBefore",queuedClosed?tabs->indexOf(queuedClosed):-1},
        {"survivorTabIndexBefore",queuedSurvivor?tabs->indexOf(queuedSurvivor):-1},
        {"currentSessionBeforeClose",double(workspace.currentDocument()?workspace.currentDocument()->sessionId():0)},
        {"countBeforeClose",workspace.documentCount()}};
    workspace.closeCurrent();
    queuedDebug["countImmediatelyAfterClose"]=workspace.documentCount();
    queuedDebug["closedPointerAliveImmediatelyAfterClose"]=!queuedClosed.isNull();
    queuedDebug["survivorPointerAliveImmediatelyAfterClose"]=!queuedSurvivor.isNull();
    queuedDebug["closedOwnerJobsImmediatelyAfterClose"]=jobs->hasJobs(queuedOwner);
    const bool queuedImportClosed=waitFor([&]{return queuedClosed.isNull()&&workspace.documentCount()==1;})
        &&!jobs->hasJobs(queuedOwner);
    queuedDebug["countAfterClosedWait"]=workspace.documentCount();
    queuedDebug["closedPointerAliveAfterWait"]=!queuedClosed.isNull();
    queuedDebug["survivorPointerAliveAfterWait"]=!queuedSurvivor.isNull();
    queuedDebug["closedOwnerJobsAfterWait"]=jobs->hasJobs(queuedOwner);
    queuedDebug["survivorIndexAfterWait"]=queuedSurvivor?tabs->indexOf(queuedSurvivor):-1;
    gate->store(true);
    const bool survivingImportCompletes=queuedSurvivor&&activate(tabs,queuedSurvivor)&&waitFor([&]{return jobs->idle();});
    report["closeQueuedImport"]=QJsonObject{{"passed",blockerRunning&&queuedImportExists&&queuedImportClosed&&survivingImportCompletes},
        {"blockerRunning",blockerRunning},{"queuedImportExists",queuedImportExists},{"closedOwnerDrained",queuedImportClosed},
        {"survivingImportCompletes",survivingImportCompletes},{"debug",queuedDebug}};
    passed=passed&&blockerRunning&&queuedImportExists&&queuedImportClosed&&survivingImportCompletes;

    if(queuedSurvivor){
        auto activeEntered=std::make_shared<std::atomic_bool>(false);
        auto cancellationObserved=std::make_shared<std::atomic_bool>(false);
        const quint64 activeOwner=queuedSurvivor->sessionId();
        const auto activeCancel=jobs->enqueue(activeOwner,[activeEntered,cancellationObserved](std::atomic_bool& cancel,const Stp::ProgressCallback&){
            activeEntered->store(true);
            while(!cancel.load())std::this_thread::sleep_for(std::chrono::milliseconds(2));
            cancellationObserved->store(true);
        },{},[](const QString&){});
        const bool activeRunning=waitFor([&]{return activeEntered->load();});
        workspace.closeCurrent();
        const bool activeClosed=waitFor([&]{return queuedSurvivor.isNull()&&workspace.documentCount()==0&&jobs->idle();})
            &&activeCancel->load()&&cancellationObserved->load()&&!jobs->hasJobs(activeOwner);
        const bool tokenWasCancelled=activeCancel->load();
        if(!activeClosed){activeCancel->store(true);waitFor([&]{return jobs->idle();});}
        report["closeActiveOwnedCadJob"]=QJsonObject{{"passed",activeRunning&&activeClosed},
            {"workerWasRunning",activeRunning},{"cancelTokenSet",tokenWasCancelled},{"workerObservedCancellation",cancellationObserved->load()},
            {"documentDestroyedAfterDrain",activeClosed}};
        passed=passed&&activeRunning&&activeClosed;
    }else passed=false;

    workspace.openFiles({assembly});QTest::qWait(30);QPointer<Stp::MainWindow> meshPage=workspace.currentDocument();
    const bool meshLoaded=meshPage&&activate(tabs,meshPage);
    auto meshGate=std::make_shared<std::atomic_bool>(false),meshBlockerEntered=std::make_shared<std::atomic_bool>(false);
    jobs->enqueue(blockerOwner,[meshGate,meshBlockerEntered](std::atomic_bool& cancel,const Stp::ProgressCallback&){
        meshBlockerEntered->store(true);
        while(!meshGate->load()&&!cancel.load())std::this_thread::sleep_for(std::chrono::milliseconds(2));
    },{},[](const QString&){});
    const bool meshBlockerRunning=waitFor([&]{return meshBlockerEntered->load();});
    quint64 meshOwner=0;bool meshWasQueued=false;
    if(meshLoaded){
        meshOwner=meshPage->sessionId();auto precision=meshPage->findChild<QComboBox*>("meshQuality");
        if(precision)precision->setCurrentIndex(precision->currentIndex()==2?0:2);
        meshWasQueued=waitFor([&]{return meshPage->isBusy()&&jobs->hasJobs(meshOwner)
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
            &&!meshPage->findChild<QAction*>("togglePartGrab")->isEnabled()
#endif
            ;});
    }
    workspace.closeCurrent();const bool meshClosed=waitFor([&]{return meshPage.isNull()&&workspace.documentCount()==0;})&&!jobs->hasJobs(meshOwner);
    meshGate->store(true);const bool meshQueueDrained=waitFor([&]{return jobs->idle();});
    report["closeQueuedMesh"]=QJsonObject{{"passed",meshLoaded&&meshBlockerRunning&&meshWasQueued&&meshClosed&&meshQueueDrained},
        {"loaded",meshLoaded},{"blockerRunning",meshBlockerRunning},{"meshJobQueued",meshWasQueued},
        {"closedOwnerDrained",meshClosed},{"queueDrained",meshQueueDrained}};
    passed=passed&&meshLoaded&&meshBlockerRunning&&meshWasQueued&&meshClosed&&meshQueueDrained;
    workspace.closeAll();jobs->cancelOwner(blockerOwner);waitFor([&]{return jobs->idle()&&workspace.documentCount()==0;});

    workspace.openFiles({cube});QPointer<Stp::MainWindow> queuedExportPage=workspace.currentDocument();
    const bool exportDocumentLoaded=queuedExportPage&&activate(tabs,queuedExportPage);
    auto exportGate=std::make_shared<std::atomic_bool>(false),exportBlockerEntered=std::make_shared<std::atomic_bool>(false);
    jobs->enqueue(blockerOwner,[exportGate,exportBlockerEntered](std::atomic_bool& cancel,const Stp::ProgressCallback&){
        exportBlockerEntered->store(true);
        while(!exportGate->load()&&!cancel.load())std::this_thread::sleep_for(std::chrono::milliseconds(2));
    },{},[](const QString&){});
    const bool exportBlockerRunning=waitFor([&]{return exportBlockerEntered->load();});
    const QString cancelledExportPath=output+"/queued-export-closed.xlsx";QFile::remove(cancelledExportPath);
    bool unexpectedExportDialog=false;QTimer modalObserver;modalObserver.setInterval(20);
    QObject::connect(&modalObserver,&QTimer::timeout,[&]{
        for(auto widget:QApplication::topLevelWidgets())if(auto message=qobject_cast<QMessageBox*>(widget);message&&message->isVisible()){
            unexpectedExportDialog=true;QMetaObject::invokeMethod(message,"accept",Qt::QueuedConnection);
        }
    });
    modalObserver.start();QJsonObject queuedExportDialog;
    quint64 exportOwner=0;bool exportWasQueued=false,queuedExportClosed=false;
    if(exportDocumentLoaded){
        exportOwner=queuedExportPage->sessionId();
        queuedExportDialog=exportThroughUi(shellBom,cancelledExportPath,QStringLiteral("导出完成"),nullptr,false);
        exportWasQueued=queuedExportDialog.value("passed").toBool()&&queuedExportPage->isBusy()&&jobs->hasJobs(exportOwner);
        workspace.closeCurrent();
        queuedExportClosed=waitFor([&]{return queuedExportPage.isNull()&&workspace.documentCount()==0;})&&!jobs->hasJobs(exportOwner);
    }
    exportGate->store(true);const bool exportQueueDrained=waitFor([&]{return jobs->idle();});QTest::qWait(100);modalObserver.stop();
    const bool queuedExportCancelled=exportDocumentLoaded&&exportBlockerRunning&&exportWasQueued&&queuedExportClosed&&exportQueueDrained
        &&!QFileInfo::exists(cancelledExportPath)&&!unexpectedExportDialog&&!QApplication::activeModalWidget();
    report["closeQueuedExport"]=QJsonObject{{"passed",queuedExportCancelled},{"loaded",exportDocumentLoaded},
        {"blockerRunning",exportBlockerRunning},{"exportJobQueued",exportWasQueued},{"closedOwnerDrained",queuedExportClosed},
        {"queueDrained",exportQueueDrained},{"outputAbsent",!QFileInfo::exists(cancelledExportPath)},
        {"noCompletionDialog",!unexpectedExportDialog},{"saveDialog",queuedExportDialog}};
    passed=passed&&queuedExportCancelled;
    workspace.closeAll();waitFor([&]{return workspace.documentCount()==0&&jobs->idle();});

    const QString repeated=repository.absoluteFilePath("samples/repeated-2000.step");
    workspace.openFiles({repeated});QPointer<Stp::MainWindow> cancelledScenePage=workspace.currentDocument();
    bool cancelRequested=false,sceneCancelled=false;QMetaObject::Connection cancelProgressConnection,cancelledConnection;
    if(cancelledScenePage){
        auto view=cancelledScenePage->findChild<Stp::ViewerWidget*>();
        cancelledConnection=QObject::connect(view,&Stp::ViewerWidget::sceneBuildCancelled,cancelledScenePage.data(),[&]{sceneCancelled=true;});
        cancelProgressConnection=QObject::connect(view,&Stp::ViewerWidget::sceneProgress,cancelledScenePage.data(),[&,view](int percent){
            if(percent>0&&percent<90&&!cancelRequested){cancelRequested=true;view->cancelSceneBuild();}
        });
    }
    const bool sceneCancellationCompleted=waitFor([&]{return cancelledScenePage&&sceneCancelled&&!cancelledScenePage->isBusy();});
    QObject::disconnect(cancelProgressConnection);QObject::disconnect(cancelledConnection);
    const bool sceneFailureVisible=cancelledScenePage&&cancelledScenePage->findChild<QWidget*>("importFailure")->isVisible();
    const bool cancelledBomAvailable=cancelledScenePage&&cancelledScenePage->document()
        &&cancelledScenePage->findChild<QAction*>("exportBom")->isEnabled()
        &&cancelledScenePage->findChild<QTableWidget*>("bomTable")->rowCount()>0;
    const auto cancelledSession=cancelledScenePage?cancelledScenePage->sessionId():0;
    bool retryClicked=false,retried=false;QJsonObject retryFrame;
    if(sceneCancellationCompleted&&cancelledScenePage){
        auto retry=cancelledScenePage->findChild<QPushButton*>("retryImport");
        retryClicked=retry&&retry->isVisible();
        if(retryClicked)QTest::mouseClick(retry,Qt::LeftButton);
        retried=retryClicked&&activate(tabs,cancelledScenePage)&&workspace.documentCount()==1
            &&cancelledScenePage->sessionId()==cancelledSession&&!cancelledScenePage->findChild<QWidget*>("importFailure")->isVisible();
        if(retried)retryFrame=renderedModel(cancelledScenePage,output+"/scene-retried.png");
    }
    const bool cancelledSceneRetry=cancelRequested&&sceneCancellationCompleted&&sceneFailureVisible&&cancelledBomAvailable
        &&retried&&retryFrame.value("passed").toBool();
    report["cancelledSceneRetry"]=QJsonObject{{"passed",cancelledSceneRetry},{"cancelRequestedDuringSceneBuild",cancelRequested},
        {"sceneCancellationCompleted",sceneCancellationCompleted},{"retryOffered",sceneFailureVisible},
        {"bomAvailableAfterSceneCancellation",cancelledBomAvailable},{"retryClicked",retryClicked},
        {"sameSessionRecovered",retried},{"frame",retryFrame}};
    passed=passed&&cancelledSceneRetry;
    workspace.closeAll();waitFor([&]{return cancelledScenePage.isNull()&&workspace.documentCount()==0&&jobs->idle();});

    workspace.openFiles({cube});QPointer<Stp::MainWindow> quittingPage=workspace.currentDocument();
    const bool quittingDocumentLoaded=quittingPage&&activate(tabs,quittingPage);
    auto quitWorkerEntered=std::make_shared<std::atomic_bool>(false),quitCancellationObserved=std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> quitCancel;bool quitWorkerRunning=false,immediatePageAlive=false;
    bool closeAcceptedImmediately=false,quitCompleted=false;int countImmediatelyAfterQuit=-1;qint64 closeCallMs=-1;
    if(quittingDocumentLoaded){
        quitCancel=jobs->enqueue(quittingPage->sessionId(),[quitWorkerEntered,quitCancellationObserved](std::atomic_bool& cancel,const Stp::ProgressCallback&){
            quitWorkerEntered->store(true);
            while(!cancel.load())std::this_thread::sleep_for(std::chrono::milliseconds(2));
            quitCancellationObserved->store(true);
        },{},[](const QString&){});
        quitWorkerRunning=waitFor([&]{return quitWorkerEntered->load();});
        workspace.grab().save(output+"/workspace-before-quit.png");
        QElapsedTimer closeTimer;closeTimer.start();closeAcceptedImmediately=workspace.close();closeCallMs=closeTimer.elapsed();
        immediatePageAlive=!quittingPage.isNull();countImmediatelyAfterQuit=workspace.documentCount();
        quitCompleted=waitFor([&]{return workspace.isHidden()&&quittingPage.isNull()&&workspace.documentCount()==0&&jobs->idle();});
        if(!quitCompleted){quitCancel->store(true);waitFor([&]{return jobs->idle();});}
    }
    const bool asynchronousQuit=quittingDocumentLoaded&&quitWorkerRunning&&!closeAcceptedImmediately&&closeCallMs>=0&&closeCallMs<250
        &&immediatePageAlive&&countImmediatelyAfterQuit==0&&quitCompleted&&quitCancel&&quitCancel->load()&&quitCancellationObserved->load();
    report["applicationQuitWithActiveJob"]=QJsonObject{{"passed",asynchronousQuit},{"documentLoaded",quittingDocumentLoaded},
        {"workerWasRunning",quitWorkerRunning},{"closeAcceptedImmediately",closeAcceptedImmediately},{"closeCallMs",double(closeCallMs)},
        {"pageAliveUntilWorkerDrain",immediatePageAlive},{"tabsRemovedImmediately",countImmediatelyAfterQuit==0},
        {"workerObservedCancellation",quitCancellationObserved->load()},{"windowHiddenAfterDrain",quitCompleted}};
    passed=passed&&asynchronousQuit;
    report["passed"]=passed;
    QSaveFile file(output+"/workspace-report.json");
    if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(report).toJson())<0||!file.commit())return 2;
    std::cout<<QJsonDocument(report).toJson().toStdString();return passed?0:1;
}
