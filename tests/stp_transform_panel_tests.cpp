#include "part_transform_panel.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <iostream>
#include <stdexcept>
using namespace Stp;
namespace{void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}}
int main(int argc,char** argv){QApplication app(argc,argv);try{
    auto doc=std::make_shared<CadDocument>();PartDefinition p;p.shape=BRepPrimAPI_MakeBox(2,4,6).Shape();doc->parts.push_back(p);
    Occurrence o;o.partIndex=0;o.path="Part";doc->occurrences.push_back(o);
    PartTransformPanel panel;panel.setDocument(doc);panel.setSceneEnabled(true);panel.setSelection(0);
    auto click=[&](const char* n){auto b=panel.findChild<QPushButton*>(n);require(b&&b->isEnabled(),"button unavailable");b->click();};
    auto x=panel.findChild<QDoubleSpinBox*>("placementTranslation0");x->setValue(20);click("previewPlacement");
    require(panel.hasPreview()&&panel.committedEdits().empty(),"preview committed prematurely");
    require(placementDelta(panel.displayEdits(),0).TranslationPart().X()==20,"preview delta");
    click("previewPlacement");require(placementDelta(panel.displayEdits(),0).TranslationPart().X()==20,"preview accumulated");
    click("applyPlacement");require(panel.isModified()&&!panel.hasPreview(),"apply state");require(x->value()==0,"delta fields must reset after apply");
    click("applyPlacement");require(panel.history()->count()==1,"zero delta created command");
    panel.markSaved();require(!panel.isModified(),"save clean state");
    x->setValue(7);click("previewPlacement");panel.setSelection(-1);require(!panel.hasPreview(),"selection change must cancel preview");panel.setSelection(0);
    panel.findChild<QComboBox*>("placementMode")->setCurrentIndex(1);panel.findChild<QComboBox*>("placementAxis")->setCurrentIndex(2);
    panel.findChild<QDoubleSpinBox*>("placementAngle")->setValue(90);const auto center=placedPartCenter(*doc,panel.committedEdits(),0);
    click("applyPlacement");require(placedPartCenter(*doc,panel.committedEdits(),0).Distance(center)<1.e-8,"center rotation translated part");
    click("undoPlacement");require(!panel.isModified(),"undo to saved state should be clean");click("redoPlacement");require(panel.isModified(),"redo state");
    click("resetPlacement");require(panel.committedEdits().empty(),"reset selected");
    panel.setDocument(doc);require(!panel.isModified()&&panel.history()->count()==0,"new document retained history");
    std::cout<<"PASS: panel preview/apply/cancel, pivot rotation, clean state, undo/redo, reset, selection and document isolation\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
