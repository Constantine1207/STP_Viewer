#include "part_placement.h"
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopLoc_Location.hxx>
#include <cmath>
#include <stdexcept>

namespace Stp {
bool samePlacement(const gp_Trsf& a, const gp_Trsf& b, double tolerance) {
    for(int r=1;r<=3;++r) for(int c=1;c<=4;++c)
        if(std::abs(a.Value(r,c)-b.Value(r,c))>tolerance) return false;
    return true;
}
gp_Trsf placementDelta(const PlacementEdits& edits,int occurrence) {
    const auto it=edits.find(occurrence);return it==edits.end()?gp_Trsf():it->second;
}
gp_Trsf placedWorldTransform(const CadDocument& doc,const PlacementEdits& edits,int occurrence) {
    return placementDelta(edits,occurrence)*doc.occurrences.at(occurrence).worldTransform;
}
gp_Pnt placedPartCenter(const CadDocument& doc,const PlacementEdits& edits,int index) {
    const auto& occurrence=doc.occurrences.at(index);
    if(occurrence.partIndex<0)throw std::runtime_error("请选择单个叶级零件实例");
    Bnd_Box box;
    BRepBndLib::AddOptimal(doc.parts.at(occurrence.partIndex).shape.Moved(
        TopLoc_Location(placedWorldTransform(doc,edits,index))),box,false,false);
    if(box.IsVoid()||box.IsOpen())throw std::runtime_error("零件没有有限包围盒，请指定旋转中心");
    double x0,y0,z0,x1,y1,z1;box.Get(x0,y0,z0,x1,y1,z1);
    return gp_Pnt((x0+x1)/2,(y0+y1)/2,(z0+z1)/2);
}
void validatePlacementEdits(const CadDocument& doc,const PlacementEdits& edits) {
    for(const auto& entry:edits) {
        if(entry.first<0||size_t(entry.first)>=doc.occurrences.size()
           ||doc.occurrences[entry.first].partIndex<0)
            throw std::runtime_error("位置编辑引用了无效零件实例");
        const auto& t=entry.second;
        for(int r=1;r<=3;++r)for(int c=1;c<=4;++c)
            if(!std::isfinite(t.Value(r,c)))throw std::runtime_error("位置参数必须为有限数值");
        if(t.IsNegative()||std::abs(t.ScaleFactor()-1.)>1.e-10)
            throw std::runtime_error("位置编辑仅支持刚体平移和旋转");
    }
}
} // namespace Stp
