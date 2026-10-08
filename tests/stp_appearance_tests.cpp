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
#include <QMessageBox>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
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
#include <QXmlStreamReader>
#include <QtEndian>
#include <zlib.h>
#include <Graphic3d_Camera.hxx>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <thread>
#include <stdexcept>

#include <QMouseEvent>
#include <QSignalSpy>
#include <QStyleOptionSlider>
#include <QToolBar>
#include <QToolButton>
#include <AIS_DisplayMode.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <TopExp_Explorer.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFPrs_AISObject.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_ChildIterator.hxx>
#include <TDataStd_Name.hxx>
#include <set>

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
QRect visibleRect(QWidget* widget)
{
    QRect visible=widget->rect();
    for(auto parent=widget->parentWidget();parent;parent=parent->parentWidget())
        visible=visible.intersected(QRect(widget->mapFrom(parent,QPoint()),parent->size()));
    return visible;
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


bool sameTransform(const gp_Trsf& a,const gp_Trsf& b)
{
    for(int row=1;row<=3;++row)for(int column=1;column<=4;++column)
        if(std::abs(a.Value(row,column)-b.Value(row,column))>1.e-6)return false;
    return true;
}
int detectedOccurrence(Stp::ViewerWidget* view,const Stp::CadDocument& document)
{
    if(!view->context()->HasDetected())return -1;
    const auto object=Handle(XCAFPrs_AISObject)::DownCast(view->context()->DetectedInteractive());
    if(object.IsNull())return -1;
    const auto world=object->LocalTransformation()*XCAFDoc_ShapeTool::GetLocation(object->GetLabel()).Transformation();
    for(size_t index=0;index<document.occurrences.size();++index)
        if(document.occurrences[index].label==object->GetLabel()&&sameTransform(document.occurrences[index].worldTransform,world))return int(index);
    return -1;
}
QPoint pickPoint(Stp::ViewerWidget* view,const Stp::CadDocument& document,int occurrence)
{
    auto viewport=view->findChild<QWidget*>("cadViewport");
    if(!viewport||occurrence<0||size_t(occurrence)>=document.occurrences.size())return {-1,-1};
    const auto& item=document.occurrences[size_t(occurrence)];if(item.partIndex<0)return {-1,-1};
    Bnd_Box box;BRepBndLib::Add(document.parts[size_t(item.partIndex)].shape,box);
    if(box.IsVoid())return {-1,-1};
    double x0,y0,z0,x1,y1,z1;box.Get(x0,y0,z0,x1,y1,z1);
    const double low[]{x0,y0,z0},high[]{x1,y1,z1};const double ratio=viewport->devicePixelRatioF();
    for(int axis=0;axis<3;++axis)for(double side:{0.,1.})for(double a:{.25,.5,.75})for(double b:{.25,.5,.75}){
        double coordinates[3];for(int i=0;i<3;++i)coordinates[i]=(low[i]+high[i])*.5;
        coordinates[axis]=low[axis]+side*(high[axis]-low[axis]);
        coordinates[(axis+1)%3]=low[(axis+1)%3]+a*(high[(axis+1)%3]-low[(axis+1)%3]);
        coordinates[(axis+2)%3]=low[(axis+2)%3]+b*(high[(axis+2)%3]-low[(axis+2)%3]);
        const auto point=gp_Pnt(coordinates[0],coordinates[1],coordinates[2]).Transformed(item.worldTransform);
        int x,y;view->view()->Convert(point.X(),point.Y(),point.Z(),x,y);const QPoint logical(qRound(x/ratio),qRound(y/ratio));
        if(!viewport->rect().adjusted(4,4,-4,-4).contains(logical))continue;
        view->context()->MoveTo(x,y,view->view(),false);
        if(detectedOccurrence(view,document)==occurrence)return logical;
    }
    return {-1,-1};
}
QMenu* visiblePartMenu()
{
    for(auto widget:QApplication::topLevelWidgets())
        if(auto menu=qobject_cast<QMenu*>(widget);menu&&menu->isVisible()&&menu->objectName()=="partAppearanceMenu")return menu;
    return nullptr;
}
QAction* menuAction(QMenu* menu,const char* name)
{
    if(menu)for(auto action:menu->actions())if(action->objectName()==name)return action;
    return nullptr;
}
QJsonObject clickContextAction(Stp::MainWindow* page,int occurrence,const char* name)
{
    auto view=page->findChild<Stp::ViewerWidget*>();auto viewport=view->findChild<QWidget*>("cadViewport");
    const auto point=pickPoint(view,*page->document(),occurrence);QSignalSpy requested(view,&Stp::ViewerWidget::partContextMenuRequested);
    if(point.x()<0)return {{"passed",false},{"error","No visible projected face for occurrence"},{"occurrence",occurrence}};
    QTest::mouseClick(viewport,Qt::RightButton,Qt::NoModifier,point);
    const bool opened=waitFor([]{return visiblePartMenu()!=nullptr;},1000);auto menu=visiblePartMenu();
    auto action=menuAction(menu,name);const bool enabled=action&&action->isEnabled();
    const bool correctHit=requested.size()==1&&requested.at(0).at(0).toInt()==occurrence;
    if(enabled)QTest::mouseClick(menu,Qt::LeftButton,Qt::NoModifier,menu->actionGeometry(action).center());
    else if(menu)QTest::keyClick(menu,Qt::Key_Escape);
    QTest::qWait(60);
    return {{"passed",opened&&enabled&&correctHit},{"occurrence",occurrence},{"menuOpened",opened},
        {"actionEnabled",enabled},{"correctProjectedHit",correctHit},{"point",QJsonArray{point.x(),point.y()}}};
}
void rightDrag(QWidget* viewport,QPoint from,QPoint to)
{
    QTest::mousePress(viewport,Qt::RightButton,Qt::NoModifier,from);
    for(int step=1;step<=6;++step){
        const QPoint position=from+(to-from)*step/6;
        QMouseEvent move(QEvent::MouseMove,QPointF(position),QPointF(viewport->mapToGlobal(position)),Qt::NoButton,Qt::RightButton,Qt::NoModifier);
        QApplication::sendEvent(viewport,&move);
    }
    QTest::mouseRelease(viewport,Qt::RightButton,Qt::NoModifier,to);QTest::qWait(40);
}
QPoint sliderHandle(QSlider* slider)
{
    QStyleOptionSlider option;option.initFrom(slider);option.orientation=slider->orientation();option.minimum=slider->minimum();
    option.maximum=slider->maximum();option.sliderPosition=slider->sliderPosition();option.sliderValue=slider->value();option.upsideDown=false;
    return slider->style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderHandle,slider).center();
}
bool dragSlider(Stp::MainWindow* page,QSlider* slider,bool returnToStart)
{
    const int before=int(page->appearanceUndoCount()),initialValue=slider->value();const QPoint from=sliderHandle(slider);
    QTest::mousePress(slider,Qt::LeftButton,Qt::NoModifier,from);bool intermediateGrouped=true;
    for(int fraction:{4,6,8}){
        const QPoint position(slider->width()*fraction/10,slider->height()/2);
        QMouseEvent move(QEvent::MouseMove,QPointF(position),QPointF(slider->mapToGlobal(position)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(slider,&move);intermediateGrouped=intermediateGrouped&&int(page->appearanceUndoCount())==before;
    }
    const QPoint last=returnToStart?from:QPoint(slider->width()*8/10,slider->height()/2);
    if(returnToStart){QMouseEvent move(QEvent::MouseMove,QPointF(last),QPointF(slider->mapToGlobal(last)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(slider,&move);}
    QTest::mouseRelease(slider,Qt::LeftButton,Qt::NoModifier,last);QTest::qWait(30);
    return intermediateGrouped&&(returnToStart?slider->value()==initialValue&&int(page->appearanceUndoCount())==before
        :slider->value()!=initialValue&&int(page->appearanceUndoCount())==before+1);
}
QJsonArray bomTexts(Stp::MainWindow* page)
{
    QJsonArray rows;auto table=page->findChild<QTableWidget*>("bomTable");
    for(int row=0;row<table->rowCount();++row){QJsonArray cells;for(int column=0;column<table->columnCount();++column)
        cells.append(table->item(row,column)?table->item(row,column)->text():QString());rows.append(cells);}return rows;
}
QJsonArray checks(Stp::MainWindow* page)
{
    QJsonArray rows;for(QTreeWidgetItemIterator it(page->findChild<QTreeWidget*>("assemblyTree"));*it;++it)
        rows.append(QJsonArray{(*it)->data(0,Qt::UserRole).toInt(),int((*it)->checkState(0))});return rows;
}
void revealControl(Stp::MainWindow* page,QWidget* control)
{
    if(auto tabs=page->findChild<QTabWidget*>("viewControlTabs"))
        for(int index=0;index<tabs->count();++index)if(tabs->widget(index)->isAncestorOf(control))tabs->setCurrentIndex(index);
    for(auto parent=control->parentWidget();parent;parent=parent->parentWidget())
        if(auto scroll=qobject_cast<QScrollArea*>(parent)){scroll->ensureWidgetVisible(control,8,8);break;}
    QTest::qWait(40);
}
bool nonblankScreenshot(Stp::ViewerWidget* view,const QString& path)
{
    if(!view->saveScreenshot(path))return false;
    QImage image(path);if(image.isNull())return false;
    std::set<QRgb> colors;
    for(int y=0;y<image.height();y+=3)for(int x=0;x<image.width();x+=3)colors.insert(image.pixel(x,y));
    return colors.size()>64;
}
QJsonObject panAndBlankMenu(Stp::MainWindow* page,int target)
{
    auto view=page->findChild<Stp::ViewerWidget*>();auto viewport=view->findChild<QWidget*>("cadViewport");
    view->selectOccurrences({target});const auto selected=view->selectedOccurrences();
    const Handle(Graphic3d_Camera) camera=new Graphic3d_Camera(view->view()->Camera());
    const auto history=page->appearanceUndoCount();QSignalSpy menuRequests(view,&Stp::ViewerWidget::partContextMenuRequested);
    const QPoint from=viewport->rect().center(),to=from+QPoint(72,36);rightDrag(viewport,from,to);
    const auto after=view->view()->Camera();
    const bool panned=after->Center().Distance(camera->Center())>1.e-6
        &&(gp_Vec(camera->Eye(),after->Eye())-gp_Vec(camera->Center(),after->Center())).Magnitude()<1.e-6
        &&after->Direction().IsEqual(camera->Direction(),1.e-8)&&std::abs(after->Scale()-camera->Scale())<1.e-6;
    const bool noMenu=menuRequests.isEmpty()&&!visiblePartMenu()&&page->appearanceUndoCount()==history;
    rightDrag(viewport,to,from);const bool reversed=cameraMatches(view->view()->Camera(),camera);
    QPoint blank(-1,-1);
    for(const QPoint point:{QPoint(8,8),QPoint(viewport->width()-8,8),QPoint(8,viewport->height()-8),QPoint(viewport->width()-8,viewport->height()-8)}){
        const auto ratio=viewport->devicePixelRatioF();view->context()->MoveTo(qRound(point.x()*ratio),qRound(point.y()*ratio),view->view(),false);
        if(!view->context()->HasDetected()){blank=point;break;}
    }
    bool disabled=false,blankSignal=false;
    if(blank.x()>=0){
        QTest::mouseClick(viewport,Qt::RightButton,Qt::NoModifier,blank);waitFor([]{return visiblePartMenu();},1000);
        auto menu=visiblePartMenu();auto transparent=menuAction(menu,"makePartTransparent70"),hide=menuAction(menu,"hidePart");
        disabled=transparent&&hide&&!transparent->isEnabled()&&!hide->isEnabled();
        blankSignal=menuRequests.size()==1&&menuRequests.at(0).at(0).toInt()==-1;
        if(menu)QTest::keyClick(menu,Qt::Key_Escape);QTest::qWait(40);
    }
    const bool kept=selected==view->selectedOccurrences()&&history==page->appearanceUndoCount();
    return {{"passed",panned&&noMenu&&reversed&&disabled&&blankSignal&&kept},{"rightDragPans",panned},
        {"dragNeverOpensPopup",noMenu},{"reverseReturnsCamera",reversed},{"blankPartActionsDisabled",disabled},
        {"blankHitReported",blankSignal},{"blankKeepsSelectionAndHistory",kept}};
}
Quantity_ColorRGBA drawerColor(const Handle(Prs3d_Drawer)& drawer)
{
    const auto aspect=drawer->ShadingAspect()->Aspect();
    return Quantity_ColorRGBA(aspect->InteriorColor(),aspect->FrontMaterial().Alpha());
}
bool colorNear(const Quantity_ColorRGBA& first,const Quantity_ColorRGBA& second)
{
    return first.GetRGB().Distance(second.GetRGB())<1.e-6&&std::abs(first.Alpha()-second.Alpha())<1.e-6;
}
QJsonObject sourceAlphaTest(const QString& output)
{
    Stp::ViewerWidget view;view.resize(960,680);view.show();QTest::qWait(80);
    auto document=std::make_shared<Stp::CadDocument>();document->xde=new TDocStd_Document("BinXCAF");
    auto shapes=XCAFDoc_DocumentTool::ShapeTool(document->xde->Main());auto colors=XCAFDoc_DocumentTool::ColorTool(document->xde->Main());
    const auto part=shapes->AddShape(BRepPrimAPI_MakeBox(10,10,10).Shape(),false);
    TopoDS_Compound compound;BRep_Builder().MakeCompound(compound);const auto root=shapes->AddShape(compound,true);
    shapes->AddComponent(root,part,TopLoc_Location());gp_Trsf translation;translation.SetTranslation(gp_Vec(18,0,0));
    const auto second=shapes->AddComponent(root,part,TopLoc_Location(translation));
    const Quantity_ColorRGBA sourceFace(Quantity_NOC_CYAN1,.4f),sourcePart(Quantity_NOC_YELLOW,.8f),sourceInstance(Quantity_NOC_PURPLE,.9f);
    colors->SetColor(part,sourcePart,XCAFDoc_ColorSurf);colors->SetColor(second,sourceInstance,XCAFDoc_ColorSurf);
    TopExp_Explorer faces(XCAFDoc_ShapeTool::GetShape(part),TopAbs_FACE);
    colors->SetColor(shapes->AddSubShape(part,faces.Current()),sourceFace,XCAFDoc_ColorSurf);
    shapes->UpdateAssemblies();Stp::rebuildIndex(*document);std::atomic_bool cancel(false);Stp::prepareDisplayMeshes(*document,1,cancel);
    bool ready=false;auto connection=QObject::connect(&view,&Stp::ViewerWidget::sceneReady,[&]{ready=true;});
    view.setDocument(document,{},true);const bool loaded=waitFor([&]{return ready;});QObject::disconnect(connection);
    struct Snapshot{int occurrence=-1;Handle(XCAFPrs_AISObject) object;Quantity_ColorRGBA color;std::vector<std::pair<TopoDS_Shape,Quantity_ColorRGBA>> custom;};
    std::vector<Snapshot> snapshots;AIS_ListOfInteractive presentations;if(loaded)view.context()->DisplayedObjects(presentations);
    bool sourceFacePresent=false;
    for(const auto& presentation:presentations){
        auto object=Handle(XCAFPrs_AISObject)::DownCast(presentation);if(object.IsNull())continue;
        Snapshot snapshot;snapshot.object=object;snapshot.color=drawerColor(object->Attributes());
        const auto world=object->LocalTransformation()*XCAFDoc_ShapeTool::GetLocation(object->GetLabel()).Transformation();
        for(size_t index=0;index<document->occurrences.size();++index)
            if(document->occurrences[index].label==object->GetLabel()&&sameTransform(document->occurrences[index].worldTransform,world))snapshot.occurrence=int(index);
        for(AIS_DataMapOfShapeDrawer::Iterator style(object->CustomAspectsMap());style.More();style.Next()){
            const auto color=drawerColor(style.Value());snapshot.custom.emplace_back(style.Key(),color);sourceFacePresent|=colorNear(color,sourceFace);
        }
        snapshots.push_back(snapshot);
    }
    bool temporary=false,persistent=false,restored=false,selectedScreenshot=false,restoredScreenshot=false;
    const bool wireframe=loaded&&view.context()->SelectionStyle()->DisplayMode()==AIS_WireFrame&&view.context()->HighlightStyle()->DisplayMode()==AIS_WireFrame;
    if(snapshots.size()==2&&snapshots[0].occurrence>=0&&snapshots[1].occurrence>=0){
        const auto& target=snapshots[0];view.setOccurrenceTemporaryTransparency(target.occurrence,70);view.selectOccurrences({target.occurrence});
        const auto temporaryStyles=[&]{
            const auto current=drawerColor(target.object->Attributes());bool ok=std::abs(current.Alpha()-.3)<1.e-6&&current.GetRGB().Distance(target.color.GetRGB())<1.e-6;
            for(const auto& entry:target.custom){const auto* drawer=target.object->CustomAspectsMap().Seek(entry.first);
                ok=ok&&drawer&&std::abs(drawerColor(*drawer).Alpha()-.3)<1.e-6&&drawerColor(*drawer).GetRGB().Distance(entry.second.GetRGB())<1.e-6;}
            return ok;
        };
        temporary=temporaryStyles();selectedScreenshot=nonblankScreenshot(&view,output+"/appearance-alpha-selected.png");
        view.selectOccurrences({snapshots[1].occurrence});persistent=temporaryStyles()&&view.effectiveOccurrenceTransparency(target.occurrence)==70;
        view.applyAppearanceState({});restored=true;
        for(const auto& snapshot:snapshots){
            restored=restored&&colorNear(drawerColor(snapshot.object->Attributes()),snapshot.color)&&snapshot.object->CustomAspectsMap().Size()==int(snapshot.custom.size());
            for(const auto& entry:snapshot.custom){const auto* drawer=snapshot.object->CustomAspectsMap().Seek(entry.first);restored=restored&&drawer&&colorNear(drawerColor(*drawer),entry.second);}
        }
        restoredScreenshot=nonblankScreenshot(&view,output+"/appearance-alpha-restored.png");
    }
    return {{"passed",loaded&&sourceFacePresent&&temporary&&persistent&&restored&&wireframe&&selectedScreenshot&&restoredScreenshot},
        {"loaded",loaded},{"importedFaceAlpha04",sourceFacePresent},{"temporaryAlpha03AcrossFaceStyles",temporary},
        {"selectionTransferPreservesOverride",persistent},{"sourceColorsAndAlphaRestored",restored},
        {"selectionAndHoverUseWireframe",wireframe},{"selectedScreenshotNonblank",selectedScreenshot},{"restoredScreenshotNonblank",restoredScreenshot}};
}
QJsonObject readbackSignature(const QString& path)
{
    std::atomic_bool cancel(false);const auto document=Stp::loadStep(path,{},cancel);QJsonArray parts,colorValues,placements;
    for(const auto& part:document->parts){QJsonArray bbox;for(double value:part.bboxMm)bbox.append(value);
        int solids=0;for(TopExp_Explorer explorer(part.shape,TopAbs_SOLID);explorer.More();explorer.Next())++solids;
        parts.append(QJsonObject{{"name",part.name},{"material",part.material},{"density",part.densityKgM3?QJsonValue(*part.densityKgM3):QJsonValue()},
            {"volume",part.volumeMm3?QJsonValue(*part.volumeMm3):QJsonValue()},{"area",part.areaMm2?QJsonValue(*part.areaMm2):QJsonValue()},
            {"bbox",bbox},{"solids",solids}});
    }
    const auto colors=XCAFDoc_DocumentTool::ColorTool(document->xde->Main());QStringList values;
    for(TDF_ChildIterator labels(document->xde->Main(),true);labels.More();labels.Next())
        for(auto type:{XCAFDoc_ColorGen,XCAFDoc_ColorSurf,XCAFDoc_ColorCurv}){
            Quantity_ColorRGBA color;if(colors->GetColor(labels.Value(),type,color))
                values.append(QString::number(int(type))+":"+QString::number(color.GetRGB().Red(),'g',12)+":"+QString::number(color.GetRGB().Green(),'g',12)+":"+QString::number(color.GetRGB().Blue(),'g',12)+":"+QString::number(color.Alpha(),'g',12));
        }
    values.sort();for(const auto& color:values)colorValues.append(color);
    for(const auto& occurrence:document->occurrences){QJsonArray transform;for(int row=1;row<=3;++row)for(int column=1;column<=4;++column)transform.append(occurrence.localTransform.Value(row,column));placements.append(transform);}
    return {{"parts",parts},{"colors",colorValues},{"localTransforms",placements}};
}
struct ExportSnapshot{QJsonObject report;std::vector<QMap<QString,QString>> sheets;QMap<QString,QJsonObject> parts;QMap<QString,QString> manifest;};
ExportSnapshot exportSnapshot(Stp::DocumentWorkspace& workspace,Stp::MainWindow* page,const QString& output,const QString& tag)
{
    // Each run owns fresh export paths; old workbooks and split directories
    // must not turn a regression run into an overwrite-dialog stress loop.
    QTemporaryDir exportRoot(output+"/"+tag+"-XXXXXX");exportRoot.setAutoRemove(false);
    require(exportRoot.isValid(),"cannot create isolated export directory");
    ExportSnapshot snapshot;const QString workbook=exportRoot.path()+"/BOM.xlsx",directory=exportRoot.path()+"/parts";QDir().mkpath(directory);
    auto bomAction=workspace.findChild<QAction*>("workspaceExportBom"),partsAction=workspace.findChild<QAction*>("workspaceExportParts");
    if(!bomAction||!partsAction){snapshot.report={{"passed",false},{"error","Missing workspace export actions"}};return snapshot;}
    auto bom=exportThroughUi(bomAction,workbook,QStringLiteral("导出完成"));waitFor([&]{return !page->isBusy()&&workspace.jobQueue()->idle();});
    auto split=exportThroughUi(partsAction,directory,QStringLiteral("拆分导出完成"));waitFor([&]{return !page->isBusy()&&workspace.jobQueue()->idle();});
    try{
        for(int sheet=1;sheet<=3;++sheet)snapshot.sheets.push_back(xlsxSheetCells(workbook,"xl/worksheets/sheet"+QByteArray::number(sheet)+".xml"));
        const auto results=QDir(directory).entryList({"STP_parts_*"},QDir::Dirs|QDir::NoDotAndDotDot);require(results.size()==1,"exactly one split directory expected");
        const QDir parts(QDir(directory).filePath(results.front()));snapshot.manifest=xlsxSheetCells(parts.filePath("manifest.xlsx"));
        for(const auto& file:parts.entryList({"*.stp","*.step"},QDir::Files))snapshot.parts.insert(file,readbackSignature(parts.filePath(file)));
        require(snapshot.parts.size()==2,"two unique STEP definitions expected");
        snapshot.report={{"passed",bom.value("passed").toBool()&&split.value("passed").toBool()},{"bom",bom},{"step",split},
            {"directory",parts.absolutePath()},{"qtyPart0",snapshot.sheets[0].value("E2")},{"qtyPart1",snapshot.sheets[0].value("E3")}};
    }catch(const std::exception& error){snapshot.report={{"passed",false},{"bom",bom},{"step",split},{"error",QString::fromUtf8(error.what())}};}
    return snapshot;
}
} // namespace

int main(int argc,char** argv)
{
    Mayo::QtOpenGlUtils::platformSetup(argc,argv);
#ifdef Q_OS_MACOS
    auto format=QSurfaceFormat::defaultFormat();format.setVersion(4,1);format.setProfile(QSurfaceFormat::CoreProfile);QSurfaceFormat::setDefaultFormat(format);
#endif
    qputenv("CSF_ShadersDirectory",QByteArray(STP_OCCT_RESOURCE_DIR)+"/Shaders");QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);app.setStyle(QStyleFactory::create("Fusion"));
    auto palette=app.palette();for(const auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText})palette.setColor(role,QColor("#233a56"));
    palette.setColor(QPalette::Window,QColor("#eef2f7"));palette.setColor(QPalette::Base,Qt::white);palette.setColor(QPalette::Button,QColor("#f0f4f9"));app.setPalette(palette);
    if(argc<2||argc>3){std::cerr<<"Usage: stp_appearance_tests output-directory [source-root]\n";return 2;}
    const QString output=QDir(QString::fromLocal8Bit(argv[1])).absolutePath();QDir().mkpath(output);
    const QDir repository(argc==3?QString::fromLocal8Bit(argv[2]):QDir::currentPath());
    const QString assembly=repository.absoluteFilePath("samples/colored-assembly.step"),cube=repository.absoluteFilePath("tests/inputs/cube.step");
    Stp::DocumentWorkspace workspace;workspace.resize(1440,831);workspace.show();QTest::qWait(150);
    workspace.openFiles({assembly,cube});auto tabs=workspace.findChild<QTabWidget*>("documentTabs");
    QPointer<Stp::MainWindow> page=documentAt(tabs,0),otherPage=documentAt(tabs,1);
    const bool loaded=page&&otherPage&&activate(tabs,page)&&activate(tabs,otherPage)&&activate(tabs,page);
    QJsonObject report{{"loaded",loaded}};bool passed=loaded;
    if(loaded){
        auto view=page->findChild<Stp::ViewerWidget*>();view->standardView(0);view->fitAll();QTest::qWait(100);
        int target=-1,otherInstance=-1;
        for(size_t index=0;index<page->document()->occurrences.size();++index)
            if(page->document()->occurrences[index].partIndex==0&&pickPoint(view,*page->document(),int(index)).x()>=0){
                if(target<0)target=int(index);else{otherInstance=int(index);break;}}
        const bool picks=target>=0&&otherInstance>=0;report["twoRepeatedInstancesPickable"]=picks;passed=passed&&picks;
        if(picks){
            const auto gestures=panAndBlankMenu(page,target);report["rightDragAndBlankMenu"]=gestures;passed=passed&&gestures.value("passed").toBool();
            const auto beforeBom=bomTexts(page),beforeChecks=checks(page);
            auto makeTransparent=clickContextAction(page,target,"makePartTransparent70");
            const bool applied=makeTransparent.value("passed").toBool()&&view->effectiveOccurrenceTransparency(target)==70
                &&view->appearanceState().instanceTransparency.count(target)==1&&view->effectiveOccurrenceTransparency(otherInstance)==0;
            const auto otherPoint=pickPoint(view,*page->document(),otherInstance);auto viewport=view->findChild<QWidget*>("cadViewport");
            QTest::mouseClick(viewport,Qt::LeftButton,Qt::NoModifier,otherPoint);QTest::qWait(40);
            const bool persistent=view->selectedOccurrences()==std::vector<int>{otherInstance}&&view->effectiveOccurrenceTransparency(target)==70
                &&view->effectiveOccurrenceTransparency(otherInstance)==0;
            const int historyBeforeNoop=int(page->appearanceUndoCount());auto repeat=clickContextAction(page,target,"makePartTransparent70");
            const bool noOp=repeat.value("passed").toBool()&&int(page->appearanceUndoCount())==historyBeforeNoop;
            auto hide=clickContextAction(page,target,"hidePart");
            const bool hidden=hide.value("passed").toBool()&&view->isOccurrenceTemporarilyHidden(target)&&!view->isOccurrenceDisplayed(target)
                &&view->selectedOccurrences()==std::vector<int>{target}&&bomTexts(page)==beforeBom&&checks(page)==beforeChecks;
            auto undo=page->findChild<QAction*>("undoAppearance");auto reset=page->findChild<QAction*>("resetAllAppearance");
            const bool actionsPresent=undo&&reset&&page->findChild<QToolButton*>("appearanceRestoreButton");
            const auto undoThroughMenu=clickContextAction(page,otherInstance,"undoAppearance");
            const bool hiddenUndone=!view->isOccurrenceTemporarilyHidden(target)&&view->isOccurrenceDisplayed(target)
                &&view->effectiveOccurrenceTransparency(target)==70;
            const auto resetThroughMenu=clickContextAction(page,otherInstance,"resetAllAppearance");
            const bool resetClean=view->appearanceState()==Stp::TemporaryAppearance{};
            report["contextCommands"]=QJsonObject{{"passed",applied&&persistent&&noOp&&hidden&&hiddenUndone&&resetClean&&actionsPresent},
                {"transparent70",makeTransparent},{"perInstancePersistence",persistent},{"repeatedCommandNoOp",noOp},
                {"hide",hide},{"hiddenOnlyInView",hidden},{"undoHide",hiddenUndone},{"resetClean",resetClean},
                {"undoThroughMenu",undoThroughMenu},{"resetThroughMenu",resetThroughMenu}};
            passed=passed&&applied&&persistent&&noOp&&hidden&&hiddenUndone&&resetClean&&actionsPresent
                &&undoThroughMenu.value("passed").toBool()&&resetThroughMenu.value("passed").toBool();
            view->saveScreenshot(output+"/appearance-context-reset.png");

            // Scope and manual engineering data are real user edits. Temporary
            // appearance must remain independent of both, including exports.
            selectPart(page,1);auto properties=page->findChild<QTableWidget*>("engineeringProperties");
            const bool manualMaterial=edit(properties,propertyValue(properties,QStringLiteral("材料")),QStringLiteral("钢"));
            const bool manualDensity=edit(properties,propertyValue(properties,QStringLiteral("密度 kg/m³")),QStringLiteral("7800"));
            auto tree=page->findChild<QTreeWidget*>("assemblyTree");const bool scoped=uncheck(tree,occurrenceItem(tree,otherInstance));
            const auto scopedBom=bomTexts(page),scopedChecks=checks(page);
            const bool supplementalData=manualMaterial&&manualDensity&&Stp::effectiveMaterial(page->document()->parts[1])==QStringLiteral("钢")
                &&Stp::effectiveDensity(page->document()->parts[1])&&std::abs(*Stp::effectiveDensity(page->document()->parts[1])-7800.)<1.e-8;
            auto originalExport=exportSnapshot(workspace,page,output,"appearance-original");
            const auto makeForExport=clickContextAction(page,target,"makePartTransparent70");
            const auto hideForExport=clickContextAction(page,target,"hidePart");
            auto temporaryExport=exportSnapshot(workspace,page,output,"appearance-temporary");
            const bool unchangedExports=originalExport.report.value("passed").toBool()&&temporaryExport.report.value("passed").toBool()
                &&originalExport.sheets==temporaryExport.sheets&&originalExport.parts==temporaryExport.parts&&originalExport.manifest==temporaryExport.manifest
                &&originalExport.report.value("qtyPart0").toString()=="4"&&originalExport.report.value("qtyPart1").toString()=="2"
                &&originalExport.sheets[0].value("F3")==QStringLiteral("钢")&&originalExport.sheets[0].value("G3")=="7800";
            report["exportIndependence"]=QJsonObject{{"passed",supplementalData&&scoped&&makeForExport.value("passed").toBool()&&hideForExport.value("passed").toBool()&&unchangedExports},
                {"manualEngineeringFieldsCommitted",supplementalData},{"treeScopeQty4And2",scoped},{"original",originalExport.report},
                {"temporary",temporaryExport.report},{"allThreeDataSheetsManifestGeometryPlacementsColorsUnchanged",unchangedExports}};
            passed=passed&&report["exportIndependence"].toObject().value("passed").toBool();

            const auto remeshState=view->appearanceState();const int remeshHistory=page->appearanceUndoCount();
            auto quality=page->findChild<QComboBox*>("meshQuality");QSignalSpy meshDone(view,&Stp::ViewerWidget::meshQualityFinished);
            quality->setCurrentIndex(2);const bool remeshed=waitFor([&]{return !meshDone.isEmpty()&&!page->isBusy();});
            const bool meshPreserved=remeshed&&view->appearanceState()==remeshState&&page->appearanceUndoCount()==remeshHistory
                &&view->isOccurrenceTemporarilyHidden(target)&&!view->isOccurrenceDisplayed(target)&&bomTexts(page)==scopedBom&&checks(page)==scopedChecks;
            report["remeshPreservesAppearance"]=QJsonObject{{"passed",meshPreserved},{"finished",remeshed},{"historyUnchanged",page->appearanceUndoCount()==remeshHistory}};
            passed=passed&&meshPreserved;

            // ShowAll is an appearance command and also checks every tree
            // item. Undo restores its temporary mask, while keeping the newly
            // chosen processing scope and the resulting BOM quantities.
            QAction* showAll=nullptr;for(auto action:page->findChild<QToolBar*>("mainToolbar")->actions())
                if(action->text()==QStringLiteral("显示全部"))showAll=action;
            if(showAll)showAll->trigger();QTest::qWait(70);
            auto bomTable=page->findChild<QTableWidget*>("bomTable");
            const bool shownAll=showAll&&view->appearanceState().hidden.empty()&&view->isOccurrenceDisplayed(target)
                &&occurrenceItem(tree,otherInstance)->checkState(0)==Qt::Checked&&number(valueForPart(bomTable,0,2),5.)
                &&number(valueForPart(bomTable,1,2),2.)&&page->appearanceUndoCount()==remeshHistory+1;
            const auto showAllBom=bomTexts(page),showAllChecks=checks(page);
            if(undo)undo->trigger();QTest::qWait(50);
            const bool maskRestoredScopeKept=view->appearanceState()==remeshState&&page->appearanceUndoCount()==remeshHistory
                &&view->isOccurrenceTemporarilyHidden(target)&&!view->isOccurrenceDisplayed(target)
                &&occurrenceItem(tree,otherInstance)->checkState(0)==Qt::Checked&&view->isOccurrenceDisplayed(otherInstance)
                &&bomTexts(page)==showAllBom&&checks(page)==showAllChecks&&number(valueForPart(bomTable,0,2),5.);
            report["showAllUndoKeepsProcessingScope"]=QJsonObject{{"passed",shownAll&&maskRestoredScopeKept},
                {"clearsMaskAndRecordsOneStep",shownAll},{"undoRestoresMaskOnly",maskRestoredScopeKept},
                {"previouslyUncheckedLeafStillChecked",occurrenceItem(tree,otherInstance)->checkState(0)==Qt::Checked},
                {"bomKeepsAllFiveInstances",number(valueForPart(bomTable,0,2),5.)}};
            passed=passed&&shownAll&&maskRestoredScopeKept;

            // Explicit selected/global slider commands replace overrides only
            // in their own scope. Verify all intermediate snapshots by Undo.
            if(reset)reset->trigger();QTest::qWait(30);
            auto slider=page->findChild<QSlider*>("transparencySlider");auto selectedOnly=page->findChild<QCheckBox*>("selectedTransparency");
            std::vector<Stp::TemporaryAppearance> overrideStates{view->appearanceState()};
            const auto contextA=clickContextAction(page,target,"makePartTransparent70");overrideStates.push_back(view->appearanceState());
            const auto contextB=clickContextAction(page,otherInstance,"makePartTransparent70");overrideStates.push_back(view->appearanceState());
            const bool bothOverrides=contextA.value("passed").toBool()&&contextB.value("passed").toBool()
                &&view->effectiveOccurrenceTransparency(target)==70&&view->effectiveOccurrenceTransparency(otherInstance)==70
                &&view->appearanceState().instanceTransparency.size()==2&&view->selectedOccurrences()==std::vector<int>{otherInstance};
            revealControl(page,selectedOnly);QTest::mouseClick(selectedOnly,Qt::LeftButton);QTest::qWait(20);overrideStates.push_back(view->appearanceState());
            slider->setValue(50);QTest::qWait(20);overrideStates.push_back(view->appearanceState());
            const bool selectedScope=view->appearanceState().selectionOnly&&view->appearanceState().transparency==50
                &&view->appearanceState().instanceTransparency.size()==1&&view->appearanceState().instanceTransparency.count(target)==1
                &&view->effectiveOccurrenceTransparency(target)==70&&view->effectiveOccurrenceTransparency(otherInstance)==50;
            QTest::mouseClick(selectedOnly,Qt::LeftButton);QTest::qWait(20);overrideStates.push_back(view->appearanceState());
            slider->setValue(30);QTest::qWait(20);overrideStates.push_back(view->appearanceState());
            const bool globalScope=!view->appearanceState().selectionOnly&&view->appearanceState().transparency==30
                &&view->appearanceState().instanceTransparency.empty()&&view->effectiveOccurrenceTransparency(target)==30
                &&view->effectiveOccurrenceTransparency(otherInstance)==30;
            bool restoredEachSnapshot=page->appearanceUndoCount()==int(overrideStates.size())-1;QJsonArray undoStages;
            for(int index=int(overrideStates.size())-2;index>=0;--index){
                if(undo)undo->trigger();QTest::qWait(15);const auto state=view->appearanceState();
                const bool match=state==overrideStates[size_t(index)]&&page->appearanceUndoCount()==index
                    &&slider->value()==state.transparency&&selectedOnly->isChecked()==state.selectionOnly;
                restoredEachSnapshot=restoredEachSnapshot&&match;undoStages.append(QJsonObject{{"expectedStage",index},{"passed",match},
                    {"transparency",state.transparency},{"selectionOnly",state.selectionOnly},{"overrideCount",int(state.instanceTransparency.size())}});
            }
            const bool scopeStillAll=bomTexts(page)==showAllBom&&checks(page)==showAllChecks;
            report["explicitSliderOverrideScopes"]=QJsonObject{{"passed",bothOverrides&&selectedScope&&globalScope&&restoredEachSnapshot&&scopeStillAll},
                {"twoInstance70Overrides",bothOverrides},{"selected50ReplacesOnlyB",selectedScope},{"global30ClearsAllOverrides",globalScope},
                {"undoRestoresEveryMapAndSliderScope",restoredEachSnapshot},{"bomAndCheckboxesUnaffected",scopeStillAll},{"undoStages",undoStages}};
            passed=passed&&bothOverrides&&selectedScope&&globalScope&&restoredEachSnapshot&&scopeStillAll;
            const bool restoredBaseScope=uncheck(tree,occurrenceItem(tree,otherInstance))&&checks(page)==scopedChecks&&bomTexts(page)==scopedBom;
            report["scopeFixtureRestored"]=restoredBaseScope;passed=passed&&restoredBaseScope;

            revealControl(page,slider);const bool groupedDrag=dragSlider(page,slider,false),roundTripNoop=dragSlider(page,slider,true);
            const int beforeKeys=page->appearanceUndoCount();slider->setFocus();
            for(int step=0;step<3;++step)QTest::keyClick(slider,Qt::Key_Right);QTest::qWait(40);
            const bool keyboardSteps=page->appearanceUndoCount()==beforeKeys+3;
            auto restoreButton=page->findChild<QToolButton*>("appearanceRestoreButton");const int beforeButton=page->appearanceUndoCount();
            if(restoreButton)QTest::mouseClick(restoreButton,Qt::LeftButton,Qt::NoModifier,QPoint(8,restoreButton->height()/2));QTest::qWait(30);
            const bool buttonUndo=restoreButton&&restoreButton->text()==QStringLiteral("还原")&&page->appearanceUndoCount()==beforeButton-1;
            if(reset)reset->trigger();QTest::qWait(30);slider->setFocus();bool eachKeyStep=true;
            for(int step=0;step<110;++step){QTest::keyClick(slider,step%2?Qt::Key_Left:Qt::Key_Right);eachKeyStep=eachKeyStep&&page->appearanceUndoCount()==std::min(step+1,100);}
            const bool capped=page->appearanceUndoCount()==100;
            for(int step=0;step<100&&undo;++step)undo->trigger();QTest::qWait(30);
            const bool drained=page->appearanceUndoCount()==0&&undo&&!undo->isEnabled();
            const int beforeDefaultReset=page->appearanceUndoCount();if(reset)reset->trigger();
            const bool emptyResetNoop=page->appearanceUndoCount()==beforeDefaultReset&&view->appearanceState()==Stp::TemporaryAppearance{};
            report["appearanceHistory"]=QJsonObject{{"passed",groupedDrag&&roundTripNoop&&keyboardSteps&&buttonUndo&&eachKeyStep&&capped&&drained&&emptyResetNoop},
                {"wholeDragOneStep",groupedDrag},{"roundTripDragNoOp",roundTripNoop},{"eachKeyboardChangeOneStep",keyboardSteps},
                {"toolbarClickUndo",buttonUndo},{"eachKeyCountMatchesCap",eachKeyStep},{"historyCap100",capped},{"undoDrainsAndDisables",drained},{"resetDefaultNoOp",emptyResetNoop}};
            passed=passed&&report["appearanceHistory"].toObject().value("passed").toBool();

            // Exercise reset on all state channels at once, then compare the
            // unrelated document and camera state, not merely the UI labels.
            view->selectOccurrences({target});QTest::mouseClick(selectedOnly,Qt::LeftButton);slider->setValue(55);QTest::qWait(30);
            const auto transparentForReset=clickContextAction(page,target,"makePartTransparent70");
            const auto hideForReset=clickContextAction(page,target,"hidePart");
            auto explosion=page->findChild<QSlider*>("explosionSlider"),section=page->findChild<QSlider*>("sectionSlider");
            auto axis=page->findChild<QComboBox*>("sectionAxis");explosion->setValue(54);axis->setCurrentIndex(1);section->setValue(77);QTest::qWait(30);
            const Handle(Graphic3d_Camera) resetCamera=new Graphic3d_Camera(view->view()->Camera());
            if(reset)reset->trigger();QTest::qWait(30);
            const bool resetAll=view->appearanceState()==Stp::TemporaryAppearance{}&&page->appearanceUndoCount()==0
                &&slider->value()==0&&!selectedOnly->isChecked()&&!view->isOccurrenceTemporarilyHidden(target)&&view->isOccurrenceDisplayed(target)
                &&cameraMatches(view->view()->Camera(),resetCamera)&&explosion->value()==54&&axis->currentIndex()==1&&section->value()==77
                &&checks(page)==scopedChecks&&bomTexts(page)==scopedBom&&Stp::effectiveMaterial(page->document()->parts[1])==QStringLiteral("钢")
                &&Stp::effectiveDensity(page->document()->parts[1])&&*Stp::effectiveDensity(page->document()->parts[1])==7800.;
            report["resetPreservesOtherState"]=QJsonObject{{"passed",transparentForReset.value("passed").toBool()&&hideForReset.value("passed").toBool()&&resetAll},
                {"allFlagsMapsAndHiddenCleared",view->appearanceState()==Stp::TemporaryAppearance{}},{"cameraExplosionSectionManualBomScopePreserved",resetAll}};
            passed=passed&&report["resetPreservesOtherState"].toObject().value("passed").toBool();
            axis->setCurrentIndex(0);explosion->setValue(0);view->fitAll();QTest::qWait(60);

            clickContextAction(page,target,"makePartTransparent70");const auto tabState=view->appearanceState();const auto tabHistory=page->appearanceUndoCount();
            const auto popupPoint=pickPoint(view,*page->document(),target);QTest::mouseClick(viewport,Qt::RightButton,Qt::NoModifier,popupPoint);
            waitFor([]{return visiblePartMenu();},1000);auto staleMenu=visiblePartMenu();QPointer<QAction> staleUndo=menuAction(staleMenu,"undoAppearance"),staleReset=menuAction(staleMenu,"resetAllAppearance");
            const bool localProxies=staleUndo&&staleReset&&staleUndo!=undo&&staleReset!=reset;
            for(int index=0;index<tabs->count();++index)if(documentAt(tabs,index)==otherPage)tabs->setCurrentIndex(index);
            const bool switched=workspace.currentDocument()==otherPage;auto otherView=otherPage->findChild<Stp::ViewerWidget*>();
            const auto otherState=otherView->appearanceState();const int otherHistory=otherPage->appearanceUndoCount();
            const bool proxiesAliveBeforeLateTrigger=staleUndo&&staleReset;
            if(staleUndo)staleUndo->trigger();if(staleReset)staleReset->trigger();QTest::qWait(40);
            const bool lateGuard=view->appearanceState()==tabState&&page->appearanceUndoCount()==tabHistory
                &&otherView->appearanceState()==otherState&&otherPage->appearanceUndoCount()==otherHistory;
            auto otherSlider=otherPage->findChild<QSlider*>("transparencySlider");revealControl(otherPage,otherSlider);otherSlider->setFocus();QTest::keyClick(otherSlider,Qt::Key_Right);QTest::qWait(30);
            const bool independent=otherView->appearanceState().transparency==1&&otherPage->appearanceUndoCount()==otherHistory+1
                &&view->appearanceState()==tabState&&page->appearanceUndoCount()==tabHistory;
            const int beforeDuplicate=workspace.documentCount();workspace.openFiles({QFileInfo(assembly).canonicalFilePath()});QTest::qWait(70);
            const bool duplicatePreserved=workspace.documentCount()==beforeDuplicate&&workspace.currentDocument()==page&&view->appearanceState()==tabState&&page->appearanceUndoCount()==tabHistory;
            report["sessionAndStaleMenuIsolation"]=QJsonObject{{"passed",localProxies&&switched&&lateGuard&&independent&&duplicatePreserved},
                {"popupUsesLocalProxies",localProxies},{"proxiesAliveBeforeLateTrigger",proxiesAliveBeforeLateTrigger},
                {"closedPopupLateActionsHarmless",lateGuard},{"historiesAndAppearanceIndependent",independent},{"canonicalDuplicatePreservesState",duplicatePreserved}};
            passed=passed&&report["sessionAndStaleMenuIsolation"].toObject().value("passed").toBool();

            workspace.resize(1000,700);QTest::qWait(100);
            restoreButton=page->findChild<QToolButton*>("appearanceRestoreButton");bool restoreAccessible=restoreButton&&restoreButton->isVisible()&&visibleRect(restoreButton).contains(restoreButton->rect().center());
            bool overflowAccessible=false;
            if(!restoreAccessible){
                auto toolbar=page->findChild<QToolBar*>("mainToolbar");auto overflow=toolbar?toolbar->findChild<QToolButton*>("qt_toolbar_ext_button"):nullptr;
                if(overflow&&overflow->isVisible()){
                    QTest::mouseClick(overflow,Qt::LeftButton);QTest::qWait(40);
                    for(auto widget:QApplication::topLevelWidgets())if(auto menu=qobject_cast<QMenu*>(widget);menu&&menu->isVisible()){
                        for(auto action:menu->actions())if(action->text()==QStringLiteral("还原")||action->objectName()==QStringLiteral("undoAppearance")||action->objectName()==QStringLiteral("resetAllAppearance"))overflowAccessible=true;
                        QTest::keyClick(menu,Qt::Key_Escape);
                    }
                }
            }
            const bool narrowShot=nonblankScreenshot(view,output+"/appearance-workspace-1000x700.png");
            const bool fullNarrowShot=workspace.grab().save(output+"/appearance-workspace-1000x700-full.png");
            report["narrowWindow"]=QJsonObject{{"passed",(restoreAccessible||overflowAccessible)&&narrowShot&&fullNarrowShot&&view->appearanceState()==tabState&&page->appearanceUndoCount()==tabHistory},
                {"restoreButtonVisible",restoreAccessible},{"toolbarOverflowOffersRestore",overflowAccessible},{"nonblankModel",narrowShot},{"fullWorkspaceScreenshotSaved",fullNarrowShot}};
            passed=passed&&report["narrowWindow"].toObject().value("passed").toBool();workspace.resize(1440,831);QTest::qWait(70);

            workspace.closeDocument(page);const bool closed=waitFor([&]{return page.isNull()&&workspace.documentCount()==1;});
            workspace.openFiles({assembly});QPointer<Stp::MainWindow> reopened=workspace.currentDocument();const bool reopenedLoaded=reopened&&activate(tabs,reopened);
            const bool reopenedClean=reopenedLoaded&&reopened->appearanceUndoCount()==0&&reopened->findChild<Stp::ViewerWidget*>()->appearanceState()==Stp::TemporaryAppearance{}
                &&!reopened->document()->parts[1].manualMaterial&&!reopened->document()->parts[1].manualDensityKgM3;
            report["closeAndReopenClean"]=QJsonObject{{"passed",closed&&reopenedClean},{"oldSessionDeleted",closed},{"newAppearanceHistoryAndManualStateClean",reopenedClean}};
            passed=passed&&closed&&reopenedClean;
        }
    }
    workspace.closeAll();waitFor([&]{return workspace.documentCount()==0&&workspace.jobQueue()->idle();});
    try{const auto alpha=sourceAlphaTest(output);report["sourceAlphaAndHighlight"]=alpha;passed=passed&&alpha.value("passed").toBool();}
    catch(const std::exception& error){report["sourceAlphaAndHighlight"]=QJsonObject{{"passed",false},{"error",QString::fromUtf8(error.what())}};passed=false;}
    report["passed"]=passed;QSaveFile file(output+"/appearance-report.json");
    if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(report).toJson())<0||!file.commit())return 2;
    std::cout<<QJsonDocument(report).toJson().toStdString();return passed?0:1;
}
