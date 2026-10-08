#include "measurement_panel.h"
#include "main_window.h"
#include "viewer_widget.h"
#include "../src/app/qtopengl_utils.h"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QDockWidget>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QMouseEvent>
#include <QPushButton>
#include <QPointer>
#include <QTableWidget>
#include <QStyleFactory>
#include <QStyleOptionSlider>
#include <QStyleOptionViewItem>
#include <QSurfaceFormat>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTimer>
#include <Graphic3d_Camera.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <BRepBndLib.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFPrs_AISObject.hxx>
#include <iostream>
#include <cmath>
#include <algorithm>

namespace {
QJsonArray rectangle(const QRect& r){return {r.x(),r.y(),r.width(),r.height()};}
QRect visibleRect(QWidget* widget)
{
    QRect visible=widget->rect();
    for(auto parent=widget->parentWidget();parent;parent=parent->parentWidget())
        visible=visible.intersected(QRect(widget->mapFrom(parent,QPoint()),parent->size()));
    return visible;
}
QJsonObject control(QWidget* widget)
{
    return {{"name",widget->objectName()},{"geometry",rectangle(widget->geometry())},
        {"sizeHint",QJsonArray{widget->sizeHint().width(),widget->sizeHint().height()}},
        {"minimum",QJsonArray{widget->minimumWidth(),widget->minimumHeight()}},
        {"visibleRect",rectangle(visibleRect(widget))},
        {"fullyVisible",widget->isVisible()&&visibleRect(widget)==widget->rect()}};
}
void revealControl(Stp::MainWindow& window,QWidget* widget)
{
    // Compact desktops put groups in separate pages; inspect each visible page
    // instead of treating deliberately inactive tabs as clipped controls.
    if(auto tabs=window.findChild<QTabWidget*>("viewControlTabs"))
        for(int index=0;index<tabs->count();++index)
            if(tabs->widget(index)->isAncestorOf(widget)){tabs->setCurrentIndex(index);QTest::qWait(20);break;}
    for(auto parent=widget->parentWidget();parent;parent=parent->parentWidget())
        if(auto scroll=qobject_cast<QScrollArea*>(parent)){scroll->ensureWidgetVisible(widget,0,0);QTest::qWait(20);break;}
}
QTableWidgetItem* propertyValue(QTableWidget* properties,const QString& name)
{
    for(int row=0;row<properties->rowCount();++row)
        if(properties->item(row,0)&&properties->item(row,0)->text()==name)return properties->item(row,1);
    return nullptr;
}
int bomRowForPart(QTableWidget* bom,int partIndex)
{
    for(int row=0;row<bom->rowCount();++row)
        if(bom->item(row,0)&&bom->item(row,0)->data(Qt::UserRole).isValid()
            &&bom->item(row,0)->data(Qt::UserRole).toInt()==partIndex)return row;
    return -1;
}
bool editable(const QTableWidgetItem* item)
{
    return item&&(item->flags()&Qt::ItemIsEditable);
}
bool numericCell(const QTableWidgetItem* item,double expected)
{
    bool ok=false;const double value=item?item->text().toDouble(&ok):0.;
    return ok&&std::abs(value-expected)<std::max(1.e-12,std::abs(expected)*1.e-8);
}
bool missingNumber(const QTableWidgetItem* item)
{
    bool ok=false;if(item)item->text().toDouble(&ok);return item&&!ok;
}
QLineEdit* visibleEditor(QTableWidget* table)
{
    for(auto editor:table->findChildren<QLineEdit*>())if(editor->isVisible())return editor;
    return nullptr;
}
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
bool sameTransform(const gp_Trsf& a,const gp_Trsf& b)
{
    for(int row=1;row<=3;++row)for(int column=1;column<=4;++column)
        if(std::abs(a.Value(row,column)-b.Value(row,column))>1.e-6)return false;
    return true;
}
bool cameraMatches(const Handle(Graphic3d_Camera)& actual,const Handle(Graphic3d_Camera)& expected)
{
    return actual->Center().Distance(expected->Center())<1.e-6&&actual->Eye().Distance(expected->Eye())<1.e-6
        &&actual->Direction().IsEqual(expected->Direction(),1.e-6)&&actual->Up().IsEqual(expected->Up(),1.e-6)
        &&std::abs(actual->Scale()-expected->Scale())<1.e-6;
}
Handle(XCAFPrs_AISObject) occurrenceObject(Stp::ViewerWidget* view,int occurrence)
{
    view->selectOccurrences({occurrence});view->context()->InitSelected();
    return view->context()->MoreSelected()
        ?Handle(XCAFPrs_AISObject)::DownCast(view->context()->SelectedInteractive()):Handle(XCAFPrs_AISObject)();
}
QPoint pickObject(Stp::ViewerWidget* view,const Stp::CadDocument& document,int occurrence,const Handle(XCAFPrs_AISObject)& object)
{
    auto viewport=view->findChild<QWidget*>("cadViewport");
    if(!viewport||object.IsNull()||occurrence<0||size_t(occurrence)>=document.occurrences.size())return {-1,-1};
    const int part=document.occurrences[size_t(occurrence)].partIndex;if(part<0)return {-1,-1};
    Bnd_Box box;BRepBndLib::Add(document.parts[size_t(part)].shape,box);if(box.IsVoid())return {-1,-1};
    double x0,y0,z0,x1,y1,z1;box.Get(x0,y0,z0,x1,y1,z1);
    const double low[]{x0,y0,z0},high[]{x1,y1,z1};const double ratio=viewport->devicePixelRatioF();
    // Keep the selected handle: a placed instance no longer matches its source
    // matrix. Project its current display transform when grabbing it again.
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
void leftDrag(QWidget* viewport,const QPoint& from,const QPoint& to)
{
    QMouseEvent press(QEvent::MouseButtonPress,from,from,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(viewport,&press);
    for(int step=1;step<=4;++step){
        const QPoint point=from+(to-from)*step/4;
        QMouseEvent move(QEvent::MouseMove,point,point,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(viewport,&move);
    }
    QMouseEvent release(QEvent::MouseButtonRelease,to,to,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(viewport,&release);QTest::qWait(40);
}
QJsonObject testPartGrab(Stp::MainWindow& window,const Stp::CadDocument& document,const QString& output)
{
    auto view=window.findChild<Stp::ViewerWidget*>();auto viewport=view->findChild<QWidget*>("cadViewport");
    auto action=window.findChild<QAction*>("togglePartGrab");auto button=window.findChild<QToolButton*>("partGrabButton");
    auto reset=window.findChild<QPushButton*>("resetPartPositions");auto explosion=window.findChild<QSlider*>("explosionSlider");
    if(!action||!button||!reset||!viewport)return {{"passed",false},{"error","Grab controls missing"}};
    auto transparency=window.findChild<QSlider*>("transparencySlider");auto section=window.findChild<QComboBox*>("sectionAxis");
    const int oldTransparency=transparency->value(),oldSection=section->currentIndex();
    const auto oldSelection=view->selectedOccurrences();
    const Handle(Graphic3d_Camera) camera=new Graphic3d_Camera(view->view()->Camera());
    transparency->setValue(0);section->setCurrentIndex(0);QTest::qWait(30);
    int occurrence=-1;QPoint point;Handle(XCAFPrs_AISObject) object;
    for(size_t index=0;index<document.occurrences.size();++index)if(document.occurrences[index].partIndex>=0){
        object=occurrenceObject(view,int(index));point=pickObject(view,document,int(index),object);
        if(point.x()>=0){occurrence=int(index);break;}
    }
    if(occurrence<0)return {{"passed",false},{"error","No visible part face"}};
    const gp_Trsf base=object->LocalTransformation();
    AIS_ListOfInteractive items;view->context()->ObjectsInside(items);
    std::vector<std::pair<Handle(XCAFPrs_AISObject),gp_Trsf>> originals;
    for(const auto& item:items){auto shape=Handle(XCAFPrs_AISObject)::DownCast(item);if(!shape.IsNull())originals.emplace_back(shape,shape->LocalTransformation());}
    const bool defaultOff=action->isCheckable()&&!action->isChecked()&&!view->grabMode()&&button->isEnabled();
    revealControl(window,button);
    const auto openIcon=button->icon().pixmap(QSize(128,128),QIcon::Normal,QIcon::Off);
    const auto closedIcon=button->icon().pixmap(QSize(128,128),QIcon::Normal,QIcon::On);
    const bool iconStates=button->toolButtonStyle()==Qt::ToolButtonIconOnly
        &&button->accessibleName()==QStringLiteral("抓手")&&!button->toolTip().isEmpty()
        &&!openIcon.isNull()&&!closedIcon.isNull()&&openIcon.toImage()!=closedIcon.toImage();
    const bool openScreenshot=button->grab().save(output+"/grab-button-open.png")
        &&openIcon.save(output+"/grab-icon-open.png");
    QTest::mouseClick(button,Qt::LeftButton);
    const bool enabled=action->isChecked()&&view->grabMode();
    const bool closedScreenshot=button->grab().save(output+"/grab-button-closed.png")
        &&closedIcon.save(output+"/grab-icon-closed.png")&&window.grab().save(output+"/grab-window-enabled.png");
    QTest::mouseClick(viewport,Qt::LeftButton,Qt::NoModifier,point);
    const bool picked=view->selectedOccurrences()==std::vector<int>{occurrence};
    leftDrag(viewport,point,point+QPoint(65,-35));const gp_Trsf placed=object->LocalTransformation();
    bool onlyOneMoved=!sameTransform(base,placed);
    for(const auto& state:originals)if(state.first!=object)onlyOneMoved=onlyOneMoved&&sameTransform(state.first->LocalTransformation(),state.second);
    const bool grabbed=onlyOneMoved&&cameraMatches(view->view()->Camera(),camera);
    const bool placedScreenshot=view->saveScreenshot(output+"/grab-placed.png");
    QTest::mouseClick(button,Qt::LeftButton);
    const bool closed=!action->isChecked()&&!view->grabMode()&&sameTransform(placed,object->LocalTransformation());
    leftDrag(viewport,QPoint(12,12),QPoint(95,70));
    const bool rotationRestored=!cameraMatches(view->view()->Camera(),camera)&&sameTransform(placed,object->LocalTransformation());
    view->view()->Camera()->Copy(camera);view->view()->Redraw();QTest::mouseClick(button,Qt::LeftButton);
    point=pickObject(view,document,occurrence,object);
    if(point.x()>=0)leftDrag(viewport,point,point+QPoint(-30,25));
    const bool regrabbed=point.x()>=0&&!sameTransform(placed,object->LocalTransformation())&&cameraMatches(view->view()->Camera(),camera);
    const auto beforeBlank=object->LocalTransformation();leftDrag(viewport,QPoint(12,12),QPoint(80,55));
    const bool blankDidNotRotate=cameraMatches(view->view()->Camera(),camera)&&sameTransform(beforeBlank,object->LocalTransformation());
    const auto resetSelection=view->selectedOccurrences();const bool zeroBeforeReset=explosion->value()==0;
    revealControl(window,reset);QTest::mouseClick(reset,Qt::LeftButton);QTest::qWait(40);
    bool restored=zeroBeforeReset&&explosion->value()==0&&view->grabMode()&&action->isChecked()
        &&cameraMatches(view->view()->Camera(),camera)&&view->selectedOccurrences()==resetSelection;
    for(const auto& state:originals)restored=restored&&sameTransform(state.first->LocalTransformation(),state.second);
    const bool resetScreenshot=view->saveScreenshot(output+"/grab-reset.png");
    QTest::mouseClick(button,Qt::LeftButton);transparency->setValue(oldTransparency);section->setCurrentIndex(oldSection);
    view->selectOccurrences(oldSelection);view->selectionChanged(oldSelection);QTest::qWait(30);
    return {{"passed",defaultOff&&iconStates&&openScreenshot&&closedScreenshot&&enabled&&picked&&grabbed&&closed&&rotationRestored&&regrabbed&&blankDidNotRotate&&restored&&placedScreenshot&&resetScreenshot},
        {"iconOnlyAccessibleButton",iconStates},{"openButtonScreenshot",openScreenshot},{"closedButtonScreenshot",closedScreenshot},
        {"defaultOff",defaultOff},{"enabledThroughButton",enabled},{"clickSelectsInstance",picked},{"onlyOneInstanceMovedWithoutRotation",grabbed},
        {"closedRetainsPlacement",closed},{"leftRotationRestored",rotationRestored},{"placedInstanceCanBeGrabbedAgain",regrabbed},
        {"blankDragDoesNotRotate",blankDidNotRotate},{"zeroSliderResetRestoresAll",restored},{"placedScreenshot",placedScreenshot},{"resetScreenshot",resetScreenshot}};
}
#endif
// Open the delegate by double-clicking the visible cell, then commit through its
// normal editor signal path. setText also permits testing Chinese material input.
bool beginFieldEdit(QTableWidget* table,QTableWidgetItem* item,const QString& input,const QString& expectedEditor)
{
    if(!editable(item))return false;
    table->scrollToItem(item,QAbstractItemView::PositionAtCenter);
    const QPoint position=table->visualItemRect(item).center();table->setFocus();
    QTest::mouseClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,position);
    QTest::mouseDClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,position);QTest::qWait(30);
    auto editor=visibleEditor(table);if(!editor)return false;
    const bool initialValueCorrect=editor->text()==expectedEditor;
    editor->setText(input);return initialValueCorrect;
}
bool editField(QTableWidget* table,QTableWidgetItem* item,const QString& input,const QString& expectedEditor)
{
    const bool began=beginFieldEdit(table,item,input,expectedEditor);
    auto editor=visibleEditor(table);if(!editor)return false;
    QTest::keyClick(editor,Qt::Key_Return);QTest::qWait(100);
    return began&&!visibleEditor(table)&&!QApplication::activeModalWidget();
}
bool rejectsEditor(QTableWidget* table,QTableWidgetItem* item)
{
    if(!item||editable(item))return false;
    table->scrollToItem(item,QAbstractItemView::PositionAtCenter);
    const QPoint position=table->visualItemRect(item).center();table->setFocus();
    QTest::mouseClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,position);
    QTest::mouseDClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,position);QTest::qWait(30);
    auto editor=visibleEditor(table);
    if(editor)QTest::keyClick(editor,Qt::Key_Escape);
    return !editor;
}
bool clickCheckState(QTreeWidget* tree,QTreeWidgetItem* item,Qt::CheckState desired)
{
    if(!item)return false;
    for(int attempt=0;attempt<3&&item->checkState(0)!=desired;++attempt){
        tree->scrollToItem(item,QAbstractItemView::PositionAtCenter);tree->horizontalScrollBar()->setValue(0);QTest::qWait(20);
        QStyleOptionViewItem option;option.initFrom(tree);option.rect=tree->visualItemRect(item);
        option.features=QStyleOptionViewItem::HasCheckIndicator|QStyleOptionViewItem::HasDisplay;
        option.checkState=item->checkState(0);
        const auto indicator=tree->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,&option,tree);
        if(indicator.isEmpty())return false;
        QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,indicator.center());QTest::qWait(100);
    }
    if(item->checkState(0)!=desired){
        // Root labels can auto-scroll horizontally on narrow Windows trees;
        // Space is the native, accessible alternative for the same checkbox.
        tree->setCurrentItem(item);tree->setFocus();
        for(int attempt=0;attempt<2&&item->checkState(0)!=desired;++attempt){QTest::keyClick(tree,Qt::Key_Space);QTest::qWait(100);}
    }
    return item->checkState(0)==desired;
}
QJsonObject exportThroughUi(QAction* action,const QString& target,const QString& successTitle,QWidget* toolbarButton=nullptr)
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
    while(!completionSeen&&elapsed.elapsed()<30000)QTest::qWait(20);
    QTest::qWait(50);responder.stop();
    const bool completionMatches=successTitle==QStringLiteral("拆分导出完成")
        ?completionText.contains(QStringLiteral("已导出"))&&completionText.contains(targetPath+"/STP_parts_")
        :absolutePath(completionText)==targetPath;
    return {{"passed",fileDialogAccepted&&completionSeen&&completionIcon==int(QMessageBox::Information)
            &&completionMatches&&!QApplication::activeModalWidget()},
        {"dialogAccepted",fileDialogAccepted},{"completionTitle",completionTitle},{"completionText",completionText},
        {"completionIcon",completionIcon},{"completionMatches",completionMatches},{"acceptedPath",acceptedPath},
        {"fileNameEditorPresent",fileNameEditorPresent},{"target",target}};
}
bool cancelExportThroughUi(QWidget* toolbarButton)
{
    bool dialogSeen=false;QTimer responder;responder.setInterval(25);
    QObject::connect(&responder,&QTimer::timeout,[&]{
        for(auto widget:QApplication::topLevelWidgets())
            if(auto dialog=qobject_cast<QFileDialog*>(widget);dialog&&dialog->isVisible()){
                dialogSeen=true;dialog->reject();
            }
    });
    responder.start();QTest::mouseClick(toolbarButton,Qt::LeftButton);QTest::qWait(50);responder.stop();
    return dialogSeen&&!QApplication::activeModalWidget();
}
}

int main(int argc,char** argv)
{
    Mayo::QtOpenGlUtils::platformSetup(argc,argv);
#ifdef Q_OS_MACOS
    auto format=QSurfaceFormat::defaultFormat();format.setVersion(4,1);format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
#endif
    qputenv("CSF_ShadersDirectory",QByteArray(STP_OCCT_RESOURCE_DIR)+"/Shaders");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv);app.setStyle(QStyleFactory::create("Fusion"));
    auto palette=app.palette();
    for(const auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText})palette.setColor(role,QColor("#233a56"));
    palette.setColor(QPalette::Window,QColor("#eef2f7"));palette.setColor(QPalette::Base,Qt::white);
    palette.setColor(QPalette::Button,QColor("#f0f4f9"));app.setPalette(palette);
    if(argc<2||argc>3){std::cerr<<"Usage: stp_mac_layout_tests output-directory [sample.step]\n";return 2;}
    const QString output=QString::fromLocal8Bit(argv[1]);QDir().mkpath(output);
    Stp::MainWindow window;window.show();QTest::qWait(250);
    if(argc==3){
        window.openStep(QString::fromLocal8Bit(argv[2]));
        QElapsedTimer timer;timer.start();
        while(!window.findChild<QSlider*>("explosionSlider")->isEnabled()&&timer.elapsed()<30000)QTest::qWait(20);
        if(!window.findChild<QSlider*>("explosionSlider")->isEnabled()){std::cerr<<"Sample load timed out\n";return 2;}
    }
    QJsonObject report;QJsonArray scenarios;bool passed=true;
    auto pages=window.findChild<QStackedWidget*>("workspacePages");
    auto bomTable=window.findChild<QTableWidget*>("bomTable");
    auto diagnostics=window.findChild<QTextEdit*>("diagnostics");
    auto details=window.findChild<QDockWidget*>("detailsDock");
    auto assembly=window.findChild<QDockWidget*>("assemblyDock");
    auto hint=window.findChild<QLabel*>("interactionHint");
    auto pageAction=window.findChild<QAction*>("showBomPage");
    auto toolbar=window.findChild<QToolBar*>("mainToolbar");
    auto back=window.findChild<QPushButton*>("returnToModel");
    if(!pages||!pageAction||!back||!hint){std::cerr<<"Missing page navigation\n";return 1;}
    const bool defaultPage=pages->currentIndex()==0&&!bomTable->isVisible()&&!diagnostics->isVisible()
        &&!window.findChild<QDockWidget*>("bomDock")&&details->isAncestorOf(hint)
        &&window.dockWidgetArea(details)==Qt::RightDockWidgetArea;
    report["defaultModelPageAndRightHint"]=defaultPage;passed=passed&&defaultPage;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    auto grabAction=window.findChild<QAction*>("togglePartGrab");auto grabButton=window.findChild<QToolButton*>("partGrabButton");
    auto scope=window.findChild<QComboBox*>("explosionScope");
    const bool grabControl=grabAction&&grabButton&&grabAction->isCheckable()&&!grabAction->isChecked()
        &&grabButton->defaultAction()==grabAction&&scope&&scope->parentWidget()==grabButton->parentWidget()
        &&grabAction->isEnabled()==(argc==3)&&grabButton->toolButtonStyle()==Qt::ToolButtonIconOnly
        &&grabButton->accessibleName()==QStringLiteral("抓手")&&!grabButton->icon().isNull();
    report["grabControlDefault"]=grabControl;passed=passed&&grabControl;
#endif
    for(const auto size:{QSize(1440,812),QSize(1440,831),QSize(1280,760)}){
        window.resize(size);QTest::qWait(150);
        QJsonObject scenario{{"requestedSize",QJsonArray{size.width(),size.height()}},
            {"actualSize",QJsonArray{window.width(),window.height()}}};
        QJsonArray items;
        for(auto group:window.findChildren<QGroupBox*>())for(auto widget:group->findChildren<QWidget*>()){
            if(!qobject_cast<QComboBox*>(widget)&&!qobject_cast<QSlider*>(widget)&&
                !qobject_cast<QCheckBox*>(widget)&&!qobject_cast<QPushButton*>(widget)&&!qobject_cast<QToolButton*>(widget))continue;
            revealControl(window,widget);
            // revealControl activates the compact view-control pages. Objects
            // on other inactive tabs (for example measurement A/B) are not
            // currently displayed and must not be classified as clipped.
            if(!widget->isVisible())continue;
            auto result=control(widget);result["group"]=group->title();items.append(result);
            passed=passed&&result["fullyVisible"].toBool();
        }
        if(auto scroll=window.findChild<QScrollArea*>("viewControlsScroll")){
            scenario["scroll"]=control(scroll);scenario["scrollViewport"]=control(scroll->viewport());
            scenario["content"]=control(scroll->widget());
        }
        scenario["controls"]=items;
        const bool hintVisible=hint->isVisible()&&visibleRect(hint)==hint->rect();
        scenario["rightSidebarHintFullyVisible"]=hintVisible;passed=passed&&hintVisible;
        window.grab().save(output+QString("/window-%1x%2.png").arg(size.width()).arg(size.height()));
        scenarios.append(scenario);
    }
    window.resize(1000,700);QTest::qWait(150);
    QJsonArray narrow;
    auto scroll=window.findChild<QScrollArea*>("viewControlsScroll");
    for(auto group:window.findChildren<QGroupBox*>())for(auto widget:group->findChildren<QWidget*>()){
        if(!qobject_cast<QComboBox*>(widget)&&!qobject_cast<QSlider*>(widget)&&
            !qobject_cast<QCheckBox*>(widget)&&!qobject_cast<QPushButton*>(widget)&&!qobject_cast<QToolButton*>(widget))continue;
        revealControl(window,widget);
        if(!widget->isVisible())continue;
        auto result=control(widget);result["group"]=group->title();narrow.append(result);
        passed=passed&&result["fullyVisible"].toBool();
    }
    report["narrowWindowReachableControls"]=narrow;
    if(scroll)scroll->horizontalScrollBar()->setValue(0);
    window.grab().save(output+"/window-1000x700.png");
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    auto measurement=window.findChild<Stp::MeasurementPanel*>("measurementPanel");
    auto measurementTabs=details->findChild<QTabWidget*>("detailsTabs");
    auto operation=measurement?measurement->findChild<QComboBox*>("measurementOperation"):nullptr;
    auto kind=measurement?measurement->findChild<QComboBox*>("measurementKind"):nullptr;
    auto history=measurement?measurement->recordsPage()->findChild<QTableWidget*>("measurementRecords"):nullptr;
    auto clearMeasurements=measurement?measurement->findChild<QPushButton*>("measurementClearAll"):nullptr;
    bool measurementLayoutPassed=measurement&&measurementTabs&&operation&&kind&&history&&clearMeasurements;
    QJsonArray layouts;
    if(measurementLayoutPassed){
        const int oldTab=measurementTabs->currentIndex(),oldOperation=operation->currentIndex();
        const int oldKind=kind->currentData().toInt();
        measurementLayoutPassed=measurementTabs->count()==4&&measurementTabs->tabText(3)==QStringLiteral("测量记录");
        for(const QSize size : {QSize(900,640),QSize(1000,700),QSize(1440,820)}){
            window.resize(size);measurementTabs->setCurrentWidget(measurement);
            operation->setCurrentIndex(1);kind->setCurrentIndex(kind->findData(int(Stp::MeasurementKind::Angle)));
            for (const auto* name : {"measurementSelectionA","measurementSelectionB"}) {
                auto selection=measurement->findChild<QComboBox*>(name);
                selection->setCurrentIndex(selection->findData(int(Stp::MeasurementSelection::Face)));
            }
            QTest::qWait(100);
            bool complete=measurement->findChildren<QScrollArea*>().empty()&&!hint->isVisible();
            QJsonArray controls;
            for(auto widget:measurement->findChildren<QWidget*>()){
                if(!widget->objectName().startsWith(QStringLiteral("measurement"))||!widget->isVisible())continue;
                if(!qobject_cast<QComboBox*>(widget)&&!qobject_cast<QPushButton*>(widget)&&!qobject_cast<QCheckBox*>(widget))continue;
                auto result=control(widget);controls.append(result);complete=complete&&result["fullyVisible"].toBool();
            }
            complete=complete&&measurement->findChild<QCheckBox*>("measurementExtentA")->isVisible()
                &&measurement->findChild<QCheckBox*>("measurementExtentB")->isVisible();
            complete=complete&&clearMeasurements->isVisible()&&visibleRect(clearMeasurements)==clearMeasurements->rect();
            window.grab().save(output+QStringLiteral("/measurement-window-%1x%2.png").arg(size.width()).arg(size.height()));
            measurement->findChild<QPushButton*>("measurementShowRecords")->click();QTest::qWait(50);
            const int rows=history->rowCount();history->setRowCount(80);QTest::qWait(50);
            const bool separateHistory=measurementTabs->currentWidget()==measurement->recordsPage()&&!measurement->isVisible()
                &&history->isVisible()&&visibleRect(history)==history->rect()&&history->verticalScrollBar()->maximum()>0
                &&!hint->isVisible();
            window.grab().save(output+QStringLiteral("/measurement-records-%1x%2.png").arg(size.width()).arg(size.height()));
            history->setRowCount(rows);
            layouts.append(QJsonObject{{"size",QStringLiteral("%1x%2").arg(size.width()).arg(size.height())},
                {"operationCompleteWithoutScroll",complete},{"separateScrollableHistory",separateHistory},{"controls",controls}});
            measurementLayoutPassed=measurementLayoutPassed&&complete&&separateHistory;
        }
        measurementTabs->setCurrentWidget(measurement);operation->setCurrentIndex(0);
        for(auto value : {Stp::MeasurementKind::Mass,Stp::MeasurementKind::CenterOfMass,Stp::MeasurementKind::VolumeCenter})
            measurementLayoutPassed=measurementLayoutPassed&&kind->findData(int(value))>=0;
        operation->setCurrentIndex(oldOperation);kind->setCurrentIndex(kind->findData(oldKind));measurementTabs->setCurrentIndex(oldTab);
    }
    report["narrowMeasurementPage"]=QJsonObject{{"passed",measurementLayoutPassed},{"layouts",layouts}};
    passed=passed&&measurementLayoutPassed;
#endif
    window.resize(1440,812);QTest::qWait(100);
    // Compact mode intentionally starts with the assembly sidebar hidden.
    // Open it through its public view action before testing checkbox workflows.
    if(assembly->isHidden())assembly->toggleViewAction()->trigger();QTest::qWait(50);
    auto bomTabs=window.findChild<QTabWidget*>("bomPageTabs");
    auto detailTabs=details->findChild<QTabWidget*>();
    bool hintWithBothTabs=true;
    for(int tab:{0,1}){detailTabs->setCurrentIndex(tab);QTest::qWait(30);hintWithBothTabs=hintWithBothTabs&&hint->isVisible()&&visibleRect(hint)==hint->rect();}
    detailTabs->setCurrentIndex(0);report["hintWithPropertiesAndPmi"]=hintWithBothTabs;passed=passed&&hintWithBothTabs;
    const auto openPage=[&]{QTest::mouseClick(toolbar->widgetForAction(pageAction),Qt::LeftButton);QTest::qWait(100);};
    openPage();
    bool pageNavigation=pages->currentIndex()==1&&bomTable->isVisible()&&!details->isVisible()&&!assembly->isVisible();
    window.grab().save(output+"/bom-page.png");
    QTest::mouseClick(bomTabs->tabBar(),Qt::LeftButton,Qt::NoModifier,bomTabs->tabBar()->tabRect(1).center());QTest::qWait(50);
    pageNavigation=pageNavigation&&diagnostics->isVisible()&&!bomTable->isVisible();
    window.grab().save(output+"/diagnostics-page.png");
    QTest::mouseClick(back,Qt::LeftButton);QTest::qWait(100);
    pageNavigation=pageNavigation&&pages->currentIndex()==0&&details->isVisible()&&assembly->isVisible()&&!diagnostics->isVisible();
    details->hide();openPage();QTest::mouseClick(back,Qt::LeftButton);QTest::qWait(50);
    const bool dockStatePreserved=details->isHidden()&&!assembly->isHidden();details->show();QTest::qWait(50);
    report["pageNavigation"]=QJsonObject{{"passed",pageNavigation},{"userHiddenSidebarPreserved",dockStatePreserved}};
    passed=passed&&pageNavigation&&dockStatePreserved;
    if(argc==3){
        QJsonArray before;for(int row=0;row<bomTable->rowCount();++row)before.append(bomTable->item(row,2)->text());
        const auto moveSlider=[&window](QSlider* item){
            revealControl(window,item);
            QStyleOptionSlider option;option.initFrom(item);option.orientation=item->orientation();
            option.minimum=item->minimum();option.maximum=item->maximum();
            option.sliderPosition=item->sliderPosition();option.sliderValue=item->value();option.upsideDown=false;
            const QPoint from=item->style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderHandle,item).center();
            const QPoint to(item->width()*3/4,item->height()/2);
            QTest::mousePress(item,Qt::LeftButton,Qt::NoModifier,from);
            QTest::mouseMove(item,to,50);QTest::mouseRelease(item,Qt::LeftButton,Qt::NoModifier,to);QTest::qWait(50);
        };
        auto explosion=window.findChild<QSlider*>("explosionSlider");moveSlider(explosion);
        const int exploded=explosion->value();
        auto view=window.findChild<Stp::ViewerWidget*>();view->saveScreenshot(output+"/drag-exploded.png");
        for(auto button:window.findChildren<QPushButton*>())if(button->text()==QStringLiteral("复位"))QTest::mouseClick(button,Qt::LeftButton);
        const bool reset=explosion->value()==0;
        auto transparency=window.findChild<QSlider*>("transparencySlider");moveSlider(transparency);
        auto axis=window.findChild<QComboBox*>("sectionAxis");revealControl(window,axis);QTest::mouseClick(axis,Qt::LeftButton);
        QTest::keyClick(axis,Qt::Key_Down);QTest::keyClick(axis,Qt::Key_Return);QTest::qWait(50);
        auto position=window.findChild<QSlider*>("sectionSlider");moveSlider(position);
        QJsonArray after;for(int row=0;row<bomTable->rowCount();++row)after.append(bomTable->item(row,2)->text());
        const bool interacted=exploded>0&&reset&&transparency->value()>0&&axis->currentIndex()==1&&position->value()!=50&&before==after;
        report["interaction"]=QJsonObject{{"passed",interacted},{"explosionAfterDrag",exploded},{"resetToZero",reset},
            {"transparencyAfterDrag",transparency->value()},{"sectionAxisAfterKeyboard",axis->currentIndex()},
            {"sectionPositionAfterDrag",position->value()},{"bomBefore",before},{"bomAfter",after}};
        passed=passed&&interacted;view->saveScreenshot(output+"/drag-section-transparent.png");
        const auto camera=view->view()->Camera();const Handle(Graphic3d_Camera) savedCamera=new Graphic3d_Camera(camera);
        auto viewport=view->findChild<QWidget*>("cadViewport");
        std::atomic_bool cancel(false);auto snapshot=Stp::loadStep(QString::fromLocal8Bit(argv[2]),{},cancel);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        auto grabResult=testPartGrab(window,*snapshot,output);report["partGrab"]=grabResult;passed=passed&&grabResult.value("passed").toBool();
#endif
        int firstLeaf=-1;for(size_t i=0;i<snapshot->occurrences.size();++i)if(snapshot->occurrences[i].partIndex>=0){firstLeaf=int(i);break;}
        view->selectOccurrences({firstLeaf});view->selectionChanged({firstLeaf});
        bool scenePreserved=true;
        for(int cycle=0;cycle<3;++cycle){
            openPage();scenePreserved=scenePreserved&&view->selectedOccurrences()==std::vector<int>{firstLeaf};
            QTest::mouseClick(back,Qt::LeftButton);QTest::qWait(100);
            scenePreserved=scenePreserved&&camera->Center().Distance(savedCamera->Center())<1.e-6
                &&camera->Eye().Distance(savedCamera->Eye())<1.e-6&&camera->Direction().IsEqual(savedCamera->Direction(),1.e-6)
                &&std::abs(camera->Scale()-savedCamera->Scale())<1.e-6&&view->findChild<QWidget*>("cadViewport")==viewport
                &&transparency->value()>0&&axis->currentIndex()==1&&position->value()!=50;
        }
        openPage();bomTabs->setCurrentIndex(0);QTest::qWait(50);
        bomTable->clearSelection();
        QTest::mouseClick(bomTable->viewport(),Qt::LeftButton,Qt::NoModifier,bomTable->visualItemRect(bomTable->item(0,0)).center());
        const auto quantities=Stp::partQuantities(*snapshot);
        const bool bomSelectionLinked=int(view->selectedOccurrences().size())==quantities[0];
        QTest::mouseClick(back,Qt::LeftButton);QTest::qWait(100);
        const bool framebuffer=view->saveScreenshot(output+"/model-after-page-return.png");
        report["pageState"]=QJsonObject{{"scenePreserved",scenePreserved},{"bomSelectionLinked",bomSelectionLinked},{"framebufferAfterReturn",framebuffer}};
        passed=passed&&scenePreserved&&bomSelectionLinked&&framebuffer;

        auto properties=window.findChild<QTableWidget*>("engineeringProperties");
        auto tree=window.findChild<QTreeWidget*>("assemblyTree");
        auto exportBom=window.findChild<QAction*>("exportBom");
        auto exportParts=window.findChild<QAction*>("exportParts");
        const bool editingFixture=properties&&tree&&exportBom&&exportParts&&snapshot->parts.size()==2
            &&snapshot->parts[0].material==QStringLiteral("Aluminium")&&quantities==std::vector<int>({5,2})
            &&snapshot->parts[0].densityKgM3&&std::abs(*snapshot->parts[0].densityKgM3-2700.)<1.e-8
            &&snapshot->parts[0].massKg&&std::abs(*snapshot->parts[0].massKg-.0162)<1.e-10
            &&snapshot->parts[1].material.isEmpty()&&!snapshot->parts[1].densityKgM3
            &&snapshot->parts[1].volumeMm3&&std::abs(*snapshot->parts[1].volumeMm3-125.)<1.e-8;
        QJsonObject editingAndScope{{"fixtureValid",editingFixture}};
        bool editingPassed=editingFixture;
        if(editingFixture){
            const auto bomValue=[&](int part,int column)->QTableWidgetItem*{
                const int row=bomRowForPart(bomTable,part);return row>=0?bomTable->item(row,column):nullptr;
            };
            const auto text=[](const QTableWidgetItem* item){return item?item->text():QString();};
            const auto selectPart=[&](int part){
                int occurrence=-1;
                for(size_t i=0;i<snapshot->occurrences.size();++i)
                    if(snapshot->occurrences[i].partIndex==part){occurrence=int(i);break;}
                view->selectOccurrences({occurrence});view->selectionChanged({occurrence});QTest::qWait(50);
            };
            selectPart(0);
            const bool importedReadOnly=!editable(propertyValue(properties,QStringLiteral("材料")))
                &&!editable(propertyValue(properties,QStringLiteral("密度 kg/m³")))
                &&!editable(bomValue(0,3))&&!editable(bomValue(0,4))
                &&rejectsEditor(properties,propertyValue(properties,QStringLiteral("材料")))
                &&rejectsEditor(properties,propertyValue(properties,QStringLiteral("密度 kg/m³")))
                &&text(propertyValue(properties,QStringLiteral("材料")))==QStringLiteral("Aluminium")
                &&numericCell(propertyValue(properties,QStringLiteral("密度 kg/m³")),2700.);
            selectPart(1);
            const bool missingEditable=editable(propertyValue(properties,QStringLiteral("材料")))
                &&editable(propertyValue(properties,QStringLiteral("密度 kg/m³")))
                &&editable(bomValue(1,3))&&editable(bomValue(1,4))
                &&text(propertyValue(properties,QStringLiteral("材料"))).contains(QStringLiteral("双击填写"))
                &&text(propertyValue(properties,QStringLiteral("密度 kg/m³"))).contains(QStringLiteral("双击填写"));
            const bool materialEntered=editField(properties,propertyValue(properties,QStringLiteral("材料")),QStringLiteral("钢"),{})
                &&text(propertyValue(properties,QStringLiteral("材料")))==QStringLiteral("钢")
                &&text(bomValue(1,3))==QStringLiteral("钢")&&editable(bomValue(1,3));
            const bool densityEntered=editField(properties,propertyValue(properties,QStringLiteral("密度 kg/m³")),QStringLiteral("7800"),{})
                &&numericCell(propertyValue(properties,QStringLiteral("密度 kg/m³")),7800.)
                &&numericCell(bomValue(1,4),7800.)
                &&numericCell(propertyValue(properties,QStringLiteral("质量 kg")),.000975)
                &&numericCell(bomValue(1,10),.000975)
                &&editable(propertyValue(properties,QStringLiteral("密度 kg/m³")))&&editable(bomValue(1,4));
            const bool engineeringScreenshot=window.grab().save(output+"/engineering-manual.png");
            openPage();bomTabs->setCurrentIndex(0);QTest::qWait(50);
            const bool bomScreenshot=window.grab().save(output+"/bom-manual.png");
            const bool importedBomReadOnly=rejectsEditor(bomTable,bomValue(0,3))&&rejectsEditor(bomTable,bomValue(0,4));
            const bool editedFromBom=editField(bomTable,bomValue(1,4),QStringLiteral("2700"),QStringLiteral("7800"))
                &&numericCell(bomValue(1,4),2700.)&&numericCell(bomValue(1,10),.0003375);
            QTest::mouseClick(back,Qt::LeftButton);QTest::qWait(50);
            const bool engineeringSynchronized=numericCell(propertyValue(properties,QStringLiteral("密度 kg/m³")),2700.)
                &&numericCell(propertyValue(properties,QStringLiteral("质量 kg")),.0003375);
            openPage();bomTabs->setCurrentIndex(0);QTest::qWait(50);
            bool invalidDensityRejected=true;QJsonArray invalidInputs;
            for(const auto invalid:{QStringLiteral("0"),QStringLiteral("-1"),QStringLiteral("nan")}){
                const bool rejected=editField(bomTable,bomValue(1,4),invalid,QStringLiteral("2700"))
                    &&numericCell(bomValue(1,4),2700.)&&numericCell(bomValue(1,10),.0003375)
                    &&editable(bomValue(1,4))&&!QApplication::activeModalWidget();
                invalidInputs.append(QJsonObject{{"input",invalid},{"rejected",rejected}});
                invalidDensityRejected=invalidDensityRejected&&rejected;
            }
            const bool densityCleared=editField(bomTable,bomValue(1,4),{},QStringLiteral("2700"))
                &&missingNumber(bomValue(1,4))&&missingNumber(bomValue(1,10))&&editable(bomValue(1,4));
            QTest::mouseClick(back,Qt::LeftButton);QTest::qWait(50);
            const bool clearSynchronized=missingNumber(propertyValue(properties,QStringLiteral("密度 kg/m³")))
                &&missingNumber(propertyValue(properties,QStringLiteral("质量 kg")))
                &&editable(propertyValue(properties,QStringLiteral("密度 kg/m³")));
            const bool materialCleared=editField(properties,propertyValue(properties,QStringLiteral("材料")),{},QStringLiteral("钢"))
                &&text(propertyValue(properties,QStringLiteral("材料"))).contains(QStringLiteral("双击填写"))
                &&text(bomValue(1,3)).contains(QStringLiteral("双击填写"))
                &&editable(propertyValue(properties,QStringLiteral("材料")))&&editable(bomValue(1,3));
            const bool manualValuesRestored=editField(properties,propertyValue(properties,QStringLiteral("材料")),QStringLiteral("钢"),{})
                &&editField(properties,propertyValue(properties,QStringLiteral("密度 kg/m³")),QStringLiteral("2700"),{})
                &&numericCell(bomValue(1,10),.0003375);

            tree->expandAll();QTest::qWait(50);
            std::vector<QTreeWidgetItem*> importedLeaves,missingLeaves;
            for(QTreeWidgetItemIterator it(tree);*it;++it){
                const int index=(*it)->data(0,Qt::UserRole).toInt();
                if(index<0||size_t(index)>=snapshot->occurrences.size())continue;
                const int part=snapshot->occurrences[size_t(index)].partIndex;
                if(part==0)importedLeaves.push_back(*it);else if(part==1)missingLeaves.push_back(*it);
            }
            bool checkboxNavigationStayed=pages->currentIndex()==0&&!pageAction->isChecked();
            const bool oneInstanceUnchecked=importedLeaves.size()==5
                &&clickCheckState(tree,importedLeaves.front(),Qt::Unchecked)
                &&numericCell(bomValue(0,2),4.)&&bomTable->rowCount()==2;
            checkboxNavigationStayed=checkboxNavigationStayed&&pages->currentIndex()==0&&!pageAction->isChecked();
            openPage();bomTabs->setCurrentIndex(0);QTest::qWait(50);
            bomTable->clearSelection();
            const int scopedRow=bomRowForPart(bomTable,0);
            if(scopedRow>=0)QTest::mouseClick(bomTable->viewport(),Qt::LeftButton,Qt::NoModifier,
                bomTable->visualItemRect(bomTable->item(scopedRow,0)).center());
            QTest::qWait(50);
            std::vector<int> expectedSelected;
            for(auto item:importedLeaves)if(item->checkState(0)==Qt::Checked)expectedSelected.push_back(item->data(0,Qt::UserRole).toInt());
            auto selected=view->selectedOccurrences();std::sort(selected.begin(),selected.end());std::sort(expectedSelected.begin(),expectedSelected.end());
            const bool selectionRestrictedToChecked=selected==expectedSelected&&selected.size()==4;
            QTest::mouseClick(back,Qt::LeftButton);QTest::qWait(50);
            bool missingInstancesUnchecked=missingLeaves.size()==2;
            for(auto item:missingLeaves){
                missingInstancesUnchecked=clickCheckState(tree,item,Qt::Unchecked)&&missingInstancesUnchecked;
                checkboxNavigationStayed=checkboxNavigationStayed&&pages->currentIndex()==0&&!pageAction->isChecked();
            }
            const bool uncheckedDefinitionExcluded=missingInstancesUnchecked&&bomRowForPart(bomTable,1)<0
                &&bomTable->rowCount()==1&&numericCell(bomValue(0,2),4.);
            openPage();bomTabs->setCurrentIndex(0);QTest::qWait(50);
            const bool scopedScreenshot=window.grab().save(output+"/bom-scoped.png");
            const QString selectedBomPath=QDir(output).absoluteFilePath("BOM-selected.xlsx");
            QFile::remove(selectedBomPath);
            auto uiBomExport=exportThroughUi(exportBom,selectedBomPath,QStringLiteral("导出完成"));
            const bool selectedBomWritten=uiBomExport.value("passed").toBool()&&QFileInfo(selectedBomPath).size()>0;
            uiBomExport.insert("fileWritten",selectedBomWritten);
            const QString selectedPartsPath=QDir(output).absoluteFilePath("ui-selected-parts");
            QDir().mkpath(selectedPartsPath);const auto oldPartDirectories=QDir(selectedPartsPath).entryList(QDir::Dirs|QDir::NoDotAndDotDot);
            auto uiPartsExport=exportThroughUi(exportParts,selectedPartsPath,QStringLiteral("拆分导出完成"));
            QString newPartDirectory;
            for(const auto& directory:QDir(selectedPartsPath).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
                if(!oldPartDirectories.contains(directory))newPartDirectory=QDir(selectedPartsPath).filePath(directory);
            const auto splitFiles=QDir(newPartDirectory).entryList({"*.step","*.stp"},QDir::Files);
            const bool selectedPartsWritten=uiPartsExport.value("passed").toBool()&&!newPartDirectory.isEmpty()
                &&splitFiles.size()==1&&QFileInfo(QDir(newPartDirectory).filePath("manifest.xlsx")).size()>0;
            uiPartsExport.insert("fileWritten",selectedPartsWritten);uiPartsExport.insert("resultDirectory",newPartDirectory);
            uiPartsExport.insert("stepFileCount",splitFiles.size());
            QTest::mouseClick(back,Qt::LeftButton);QTest::qWait(50);
            const bool emptyScope=tree->topLevelItemCount()==1&&clickCheckState(tree,tree->topLevelItem(0),Qt::Unchecked)
                &&bomTable->rowCount()==0&&!exportBom->isEnabled()&&!exportParts->isEnabled();
            checkboxNavigationStayed=checkboxNavigationStayed&&pages->currentIndex()==0&&!pageAction->isChecked();
            const bool recheckedRestores=clickCheckState(tree,tree->topLevelItem(0),Qt::Checked)
                &&bomTable->rowCount()==2&&numericCell(bomValue(0,2),5.)&&numericCell(bomValue(1,2),2.)
                &&text(bomValue(1,3))==QStringLiteral("钢")&&numericCell(bomValue(1,4),2700.)
                &&numericCell(bomValue(1,10),.0003375)&&editable(bomValue(1,3))&&editable(bomValue(1,4))
                &&!editable(bomValue(0,3))&&!editable(bomValue(0,4))
                &&exportBom->isEnabled()&&exportParts->isEnabled();
            checkboxNavigationStayed=checkboxNavigationStayed&&pages->currentIndex()==0&&!pageAction->isChecked();
            const QString manualBomPath=QDir(output).absoluteFilePath("BOM-manual.xlsx");QFile::remove(manualBomPath);
            openPage();bomTabs->setCurrentIndex(0);QTest::qWait(50);
            const bool cancellationEditorOpened=beginFieldEdit(bomTable,bomValue(1,4),QStringLiteral("7800"),QStringLiteral("2700"));
            const bool cancelledExportRetainsInput=cancelExportThroughUi(toolbar->widgetForAction(exportBom))
                &&cancellationEditorOpened&&numericCell(bomValue(1,4),7800.)&&numericCell(bomValue(1,10),.000975);
            const bool uncommittedEditorOpened=beginFieldEdit(bomTable,bomValue(1,4),QStringLiteral("2700"),QStringLiteral("7800"));
            auto uiManualBomExport=exportThroughUi(exportBom,manualBomPath,QStringLiteral("导出完成"),toolbar->widgetForAction(exportBom));
            const bool uncommittedEditorExported=uncommittedEditorOpened&&!visibleEditor(bomTable)
                &&numericCell(bomValue(1,4),2700.)&&numericCell(bomValue(1,10),.0003375);
            const bool manualBomWritten=uiManualBomExport.value("passed").toBool()&&QFileInfo(manualBomPath).size()>0&&uncommittedEditorExported;
            uiManualBomExport.insert("fileWritten",manualBomWritten);
            uiManualBomExport.insert("pendingEditorCommitted",uncommittedEditorExported);
            QTest::mouseClick(back,Qt::LeftButton);QTest::qWait(50);
            editingAndScope.insert("importedFieldsReadOnly",importedReadOnly&&importedBomReadOnly);
            editingAndScope.insert("missingFieldsEditable",missingEditable);
            editingAndScope.insert("materialEnteredAndLinked",materialEntered);
            editingAndScope.insert("densityEnteredAndMassComputed",densityEntered);
            editingAndScope.insert("densityEditedFromBomAndLinked",editedFromBom&&engineeringSynchronized);
            editingAndScope.insert("invalidDensityInputs",invalidInputs);
            editingAndScope.insert("densityClearedAndMassReset",densityCleared&&clearSynchronized);
            editingAndScope.insert("materialCleared",materialCleared);
            editingAndScope.insert("manualValuesRestored",manualValuesRestored);
            editingAndScope.insert("oneUncheckedInstanceChangesQuantity",oneInstanceUnchecked);
            editingAndScope.insert("bomSelectionUsesOnlyCheckedInstances",selectionRestrictedToChecked);
            editingAndScope.insert("uncheckedDefinitionExcluded",uncheckedDefinitionExcluded);
            editingAndScope.insert("emptyScopeDisablesExports",emptyScope);
            editingAndScope.insert("recheckingRestoresValuesAndQuantity",recheckedRestores);
            editingAndScope.insert("checkboxChangesKeepModelPage",checkboxNavigationStayed);
            editingAndScope.insert("screenshotsSaved",engineeringScreenshot&&bomScreenshot&&scopedScreenshot);
            editingAndScope.insert("selectedBomUiExport",uiBomExport);
            editingAndScope.insert("selectedPartsUiExport",uiPartsExport);
            editingAndScope.insert("manualBomUiExport",uiManualBomExport);
            editingAndScope.insert("cancelledExportRetainsInput",cancelledExportRetainsInput);
            editingPassed=importedReadOnly&&importedBomReadOnly&&missingEditable&&materialEntered&&densityEntered
                &&editedFromBom&&engineeringSynchronized&&invalidDensityRejected&&densityCleared&&clearSynchronized
                &&materialCleared&&manualValuesRestored&&oneInstanceUnchecked&&selectionRestrictedToChecked
                &&uncheckedDefinitionExcluded&&emptyScope&&recheckedRestores&&checkboxNavigationStayed
                &&engineeringScreenshot&&bomScreenshot&&scopedScreenshot&&selectedBomWritten&&selectedPartsWritten&&manualBomWritten
                &&cancelledExportRetainsInput;
        }
        editingAndScope.insert("passed",editingPassed);report["editingAndScope"]=editingAndScope;passed=passed&&editingPassed;
    }
    report["scenarios"]=scenarios;report["passed"]=passed;
    QSaveFile file(output+"/layout-report.json");
    if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(report).toJson())<0||!file.commit())return 2;
    std::cout<<QJsonDocument(report).toJson().toStdString();return passed?0:1;
}
