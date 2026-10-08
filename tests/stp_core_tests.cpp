#include "cad_document.h"
#include "bom_export.h"
#include "part_export.h"
#include "pmi.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QSet>
#include <QMap>
#include <QXmlStreamReader>
#include <QtEndian>
#include <zlib.h>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <DESTEP_Parameters.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <StepBasic_Product.hxx>
#include <StepData_StepModel.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TColStd_SequenceOfHAsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <TDataStd_NamedData.hxx>
#include <TDF_LabelSequence.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_Datum.hxx>
#include <XCAFDoc_Dimension.hxx>
#include <XCAFDoc_DimTolTool.hxx>
#include <XCAFDoc_MaterialTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDimTolObjects_DatumObject.hxx>
#include <XCAFDimTolObjects_DimensionObject.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pln.hxx>
#include <cmath>
#include <algorithm>
#include <clocale>
#include <iostream>
#include <limits>
#include <stdexcept>
#ifdef Q_OS_MACOS
#include <sys/stat.h>
#include <sys/xattr.h>
#endif

namespace {
void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
bool nearEqual(double a,double b,double tolerance=1.e-6) { return std::abs(a-b)<tolerance; }

#ifdef Q_OS_MACOS
void checkXlsx(const QString& path);
void checkMacExportVisibility(const Stp::CadDocument& doc,const QString& parentDirectory,
                             std::atomic_bool& cancel) {
    struct InjectedMetadata { unsigned int flags; QByteArray finderInfo; };
    QMap<QString,InjectedMetadata> injected;
    const auto hide=[&](const QString& path,const QString& relativePath) {
        if(injected.contains(relativePath)) return;
        const auto native=QFile::encodeName(path);
        struct stat metadata{};
        require(::stat(native.constData(),&metadata)==0,"cannot inspect staged export metadata");
        const auto flags=metadata.st_flags|UF_HIDDEN;
        require(::chflags(native.constData(),flags)==0,"cannot inject staged Finder-hidden flag");
        QByteArray finderInfo(32,'\0');
        finderInfo.replace(0,4,"TEST");
        finderInfo[8]=char(0x41); // kIsInvisible plus an unrelated flag.
        finderInfo[9]=char(0x02); // Preserve the Finder colour label.
        finderInfo[16]=char(0x3a);
        require(::setxattr(native.constData(),"com.apple.FinderInfo",finderInfo.constData(),
                           size_t(finderInfo.size()),0,0)==0,"cannot inject staged FinderInfo");
        injected.insert(relativePath,{flags,finderInfo});
    };
    const auto result=Stp::exportParts(doc,{},parentDirectory,[&](int progress,const QString&) {
        if(progress==100) return;
        const QDir parent(parentDirectory);
        const auto staged=parent.entryList({QStringLiteral("STP_exporting-*")},
            QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot);
        require(staged.size()==1,"visibility regression requires one staging directory");
        const QString stagingPath=parent.filePath(staged.front());
        hide(stagingPath,QStringLiteral("."));
        const QDir staging(stagingPath);
        // The next-part callback runs after the preceding STEP file is committed.
        for(const auto& filename:staging.entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot))
            hide(staging.filePath(filename),filename);
    },cancel);
    require(injected.size()>=2,"visibility regression did not exercise both directory and file metadata");
    for(auto it=injected.cbegin();it!=injected.cend();++it) {
        const auto path=it.key()==QStringLiteral(".")?result.directory:QDir(result.directory).filePath(it.key());
        const auto native=QFile::encodeName(path);
        struct stat metadata{};
        require(::stat(native.constData(),&metadata)==0,"published visibility fixture is missing");
        require(metadata.st_flags==(it->flags&~UF_HIDDEN),"export must clear only the BSD hidden flag");
        QByteArray finderInfo(32,'\0');
        require(::getxattr(native.constData(),"com.apple.FinderInfo",finderInfo.data(),
                           size_t(finderInfo.size()),0,0)==finderInfo.size(),"published FinderInfo is missing");
        auto expected=it->finderInfo; expected[8]=char(static_cast<unsigned char>(expected[8])&~0x40);
        require(finderInfo==expected,"export must clear Finder invisibility and preserve all other FinderInfo bytes");
    }
    const QStringList artifacts=QStringList(result.files)<<result.manifestPath;
    for(const auto& path:artifacts) {
        const auto native=QFile::encodeName(path);
        struct stat metadata{};
        require(::stat(native.constData(),&metadata)==0&&(metadata.st_flags&UF_HIDDEN)==0,
                "published STEP/manifest must not remain Finder-hidden");
        unsigned char finderInfo[32]{};
        const auto size=::getxattr(native.constData(),"com.apple.FinderInfo",finderInfo,sizeof(finderInfo),0,0);
        require(size<10||(finderInfo[8]&0x40)==0,"published STEP/manifest FinderInfo remains invisible");
        QFile file(path);
        require(file.open(QIODevice::ReadOnly)&&!file.read(32).isEmpty(),"published STEP/manifest is unreadable or empty");
    }
    require(QDir(parentDirectory).entryList(QDir::Dirs|QDir::NoDotAndDotDot)
                .contains(QFileInfo(result.directory).fileName()),"export directory must appear in a normal directory listing");
    require(QDir(result.directory).entryList(QDir::Files|QDir::NoDotAndDotDot).size()==artifacts.size(),
            "all published export artifacts must appear in a normal directory listing");
    checkXlsx(result.manifestPath);
}
#endif

Handle(TDocStd_Document) newDocument() {
    Handle(TDocStd_Document) doc=new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(doc->Main());
    XCAFDoc_DocumentTool::SetLengthUnit(doc,0.001);
    return doc;
}
TDF_Label emptyAssembly(const Handle(XCAFDoc_ShapeTool)& tool,const char* name) {
    TopoDS_Compound compound; BRep_Builder().MakeCompound(compound);
    auto label=tool->AddShape(compound,true);
    TDataStd_Name::Set(label,TCollection_ExtendedString(name,true));
    return label;
}
TopLoc_Location translation(double x,double y,double z) {
    gp_Trsf transform; transform.SetTranslation(gp_Vec(x,y,z)); return TopLoc_Location(transform);
}

Stp::CadDocument fixture() {
    Stp::CadDocument doc; doc.xde=newDocument(); doc.sourceUnits=QStringLiteral("millimetre");
    const auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc.xde->Main());
    const auto colors=XCAFDoc_DocumentTool::ColorTool(doc.xde->Main());
    auto a=shapes->AddShape(BRepPrimAPI_MakeBox(10,20,30).Shape(),false);
    auto b=shapes->AddShape(BRepPrimAPI_MakeBox(5,5,5).Shape(),false);
    TDataStd_Name::Set(a,TCollection_ExtendedString("同名零件",true));
    TDataStd_Name::Set(b,TCollection_ExtendedString("同名零件",true));
    shapes->GetNamedProperties(a,true)->SetString("ProductID","CON");
    colors->SetColor(a,Quantity_Color(1,0,0,Quantity_TOC_RGB),XCAFDoc_ColorSurf);
    colors->SetColor(b,Quantity_Color(0.1,0.3,1,Quantity_TOC_RGB),XCAFDoc_ColorSurf);
    TopExp_Explorer face;
    const auto aShape=shapes->GetShape(a);
    face.Init(aShape,TopAbs_FACE);
    require(face.More(),"fixture face missing");
    const auto faceLabel=shapes->AddSubShape(a,face.Current());
    colors->SetColor(faceLabel,Quantity_Color(0,1,0,Quantity_TOC_RGB),XCAFDoc_ColorSurf);
    XCAFDoc_DocumentTool::MaterialTool(doc.xde->Main())->SetMaterial(a,
        new TCollection_HAsciiString("Aluminium"),new TCollection_HAsciiString("test material"),2.7,
        new TCollection_HAsciiString("density"),new TCollection_HAsciiString("MASS_DENSITY_MEASURE"));
    auto sub=emptyAssembly(shapes,"子装配");
    auto overridden=shapes->AddComponent(sub,a,translation(10,0,0));
    colors->SetColor(overridden,Quantity_Color(0,1,1,Quantity_TOC_RGB),XCAFDoc_ColorSurf);
    shapes->AddComponent(sub,a,translation(20,0,0));
    shapes->AddComponent(sub,b,translation(0,30,0));
    auto root=emptyAssembly(shapes,"总装");
    gp_Trsf rotated; rotated.SetRotation(gp_Ax1(gp_Pnt(0,0,0),gp_Dir(0,0,1)),std::acos(-1.)/2);
    rotated.SetTranslationPart(gp_Vec(100,0,0));
    shapes->AddComponent(root,sub,TopLoc_Location(rotated));
    shapes->AddComponent(root,sub,translation(200,0,0));
    auto namedInstance=shapes->AddComponent(root,a,translation(0,0,100));
    TDataStd_Name::Set(namedInstance,TCollection_ExtendedString("独立实例",true));
    shapes->UpdateAssemblies();
    Stp::rebuildIndex(doc);
    return doc;
}

Stp::CadDocument repeatedFixture(int count) {
    require(count>0 && count<=100000,"benchmark instance count must be 1..100000");
    Stp::CadDocument doc; doc.xde=newDocument(); doc.sourceUnits=QStringLiteral("millimetre");
    auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc.xde->Main());
    auto part=shapes->AddShape(BRepPrimAPI_MakeBox(8,12,6).Shape(),false);
    TDataStd_Name::Set(part,TCollection_ExtendedString("重复零件",true));
    XCAFDoc_DocumentTool::ColorTool(doc.xde->Main())->SetColor(part,
        Quantity_Color(0.12,0.55,0.85,Quantity_TOC_RGB),XCAFDoc_ColorSurf);
    auto root=emptyAssembly(shapes,"重复实例性能样例");
    for(int i=0;i<count;++i) {
        auto occurrence=shapes->AddComponent(root,part,translation((i%40)*15.,((i/40)%50)*18.,(i/2000)*12.));
        const QByteArray name=QStringLiteral("实例_%1").arg(i+1,4,10,QLatin1Char('0')).toUtf8();
        TDataStd_Name::Set(occurrence,TCollection_ExtendedString(name.constData(),true));
    }
    shapes->UpdateAssemblies(); Stp::rebuildIndex(doc);
    require(doc.parts.size()==1 && doc.occurrences.size()==size_t(count+1),"benchmark definition/instance count");
    require(Stp::partQuantities(doc)[0]==count,"benchmark leaf quantity");
    return doc;
}
void writeStep(const Stp::CadDocument& doc,const QString& path,
               DESTEP_Parameters::WriteMode_StepSchema schema,
               UnitsMethods_LengthUnit unit=UnitsMethods_LengthUnit_Millimeter,
               bool repairDensity=true,bool distinctProductIds=false) {
    STEPCAFControl_Writer writer;
    writer.SetColorMode(true); writer.SetNameMode(true); writer.SetMaterialMode(true);
    DESTEP_Parameters parameters; parameters.WriteSchema=schema; parameters.WriteUnit=unit;
    require(writer.Transfer(doc.xde,parameters),"fixture STEP transfer failed");
    if(distinctProductIds) {
        const auto model=writer.ChangeWriter().Model();
        for(int i=1;i<=model->NbEntities();++i)
            if(const auto product=Handle(StepBasic_Product)::DownCast(model->Value(i)); !product.IsNull())
                product->SetId(new TCollection_HAsciiString(("ASCII_ID_"+std::to_string(i)).c_str()));
    }
    if(repairDensity) Stp::prepareStepMaterialExport(writer);
    require(writer.Write(path.toUtf8().constData())==IFSelect_RetDone,"fixture STEP write failed");
    if(qEnvironmentVariableIsSet("STP_TEST_NAME_DIAGNOSTICS")) {
        TCollection_HAsciiString unicodeName("独立实例");
        std::cerr << "UTF-8 byte length / kernel useful length: " << unicodeName.Length() << " / " << unicodeName.UsefullLength() << '\n';
        QFile written(path);
        require(written.open(QIODevice::ReadOnly),"cannot read written fixture for name diagnostics");
        const auto content=written.readAll();
        qsizetype position=0;
        while((position=content.indexOf("NEXT_ASSEMBLY_USAGE_OCCURRENCE",position))>=0) {
            const auto end=content.indexOf(';',position);
            if(end<0) break;
            std::cerr << "Written NAUO: " << content.mid(position,end-position+1).constData() << '\n';
            position=end+1;
        }
    }
}
void checkCounts(const Stp::CadDocument& doc) {
    require(doc.parts.size()==2,"same names must not merge distinct definitions");
    auto quantities=Stp::partQuantities(doc);
    require(quantities.size()==2,"quantity vector size");
    bool foundA=false,foundB=false;
    for(size_t i=0;i<doc.parts.size();++i) {
        const auto& part=doc.parts[i];
        require(part.validSolid && part.volumeMm3,"valid fixture volume missing");
        if(nearEqual(*part.volumeMm3,6000.,0.001)) { require(quantities[i]==5,"nested first definition quantity should be five"); foundA=true; }
        if(nearEqual(*part.volumeMm3,125.,0.001)) { require(quantities[i]==2,"nested second definition quantity should be two"); foundB=true; }
    }
    require(foundA&&foundB,"roundtrip volumes changed (possible unit or transform error)");
    require(doc.occurrences.size()==10,"nested assembly must retain ten occurrences including three assemblies");
    QSet<QString> ids;
    bool preservedName=false;
    for(const auto& occurrence:doc.occurrences) {
        ids.insert(occurrence.id);
        require(!occurrence.name.startsWith(QStringLiteral("=>[")),"internal XDE names leaked into occurrences");
        if(occurrence.name==QStringLiteral("独立实例")) preservedName=true;
    }
    require(ids.size()==10,"repeated subassembly occurrences must have unique IDs");
    if(!preservedName) {
        std::cerr << "Expected occurrence UTF-8: " << QStringLiteral("独立实例").toUtf8().toHex().constData() << '\n';
        for(const auto& occurrence:doc.occurrences)
            std::cerr << "Occurrence " << occurrence.id.toUtf8().constData() << " UTF-8: "
                      << occurrence.name.toUtf8().toHex().constData() << '\n';
    }
    require(preservedName,"explicit occurrence name must be preserved");
    for(const auto& warning:doc.diagnostics)
        require(!warning.startsWith(QStringLiteral("外部引用：")),"internal references incorrectly reported as external files");
}

void checkColors(const Stp::CadDocument& doc) {
    const auto colors=XCAFDoc_DocumentTool::ColorTool(doc.xde->Main());
    const auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc.xde->Main());
    bool red=false,blue=false,greenFace=false,cyanOccurrence=false;
    for(const auto& part:doc.parts) {
        Quantity_Color color;
        if(colors->GetColor(part.label,XCAFDoc_ColorSurf,color)) {
            red=red || (color.Red()>0.9 && color.Green()<0.1 && color.Blue()<0.1);
            blue=blue || (color.Blue()>0.9 && color.Red()<0.2);
        }
        TDF_LabelSequence children; shapes->GetSubShapes(part.label,children);
        for(int i=1;i<=children.Length();++i)
            if(colors->GetColor(children(i),XCAFDoc_ColorSurf,color))
                greenFace=greenFace || (color.Green()>0.9 && color.Red()<0.1 && color.Blue()<0.1);
    }
    for(const auto& occurrence:doc.occurrences) {
        Quantity_Color color;
        if(colors->GetColor(occurrence.label,XCAFDoc_ColorSurf,color))
            cyanOccurrence=cyanOccurrence || (color.Red()<0.1 && color.Green()>0.9 && color.Blue()>0.9);
    }
    require(red && blue,"part definition colors missing or changed");
    require(greenFace,"face-level green color missing or changed");
    require(cyanOccurrence,"instance-level cyan override missing or changed");
}

void checkTransforms(const Stp::CadDocument& doc) {
    QSet<QString> placements;
    for(const auto& occurrence:doc.occurrences) if(occurrence.partIndex>=0) {
        const auto& transform=occurrence.worldTransform;
        QStringList values;
        for(int r=1;r<=3;++r) for(int c=1;c<=4;++c)
            values<<QString::number(std::round(transform.Value(r,c)*1.e6)/1.e6,'f',3);
        placements.insert(values.join(QLatin1Char(',')));
    }
    require(placements.size()==7,"distinct leaf placements were lost");
    const auto expected=fixture();
    for(const auto& occurrence:expected.occurrences) if(occurrence.partIndex>=0) {
        QStringList values;
        for(int r=1;r<=3;++r) for(int c=1;c<=4;++c)
            values<<QString::number(std::round(occurrence.worldTransform.Value(r,c)*1.e6)/1.e6,'f',3);
        require(placements.contains(values.join(QLatin1Char(','))),"roundtrip changed full nested placement matrix");
    }
}

void checkMaterial(const Stp::CadDocument& doc) {
    bool found=false;
    for(const auto& part:doc.parts) {
      if(part.volumeMm3 && nearEqual(*part.volumeMm3,6000.,0.001)) {
        require(part.material==QStringLiteral("Aluminium"),"material name lost in STEP roundtrip");
        if(!part.densityKgM3 || !nearEqual(*part.densityKgM3,2700.))
            std::cerr<<"density="<<(part.densityKgM3?*part.densityKgM3:-1)<<" diagnostics="<<doc.diagnostics.join('\n').toStdString()<<'\n';
        require(part.densityKgM3 && nearEqual(*part.densityKgM3,2700.),"STEP density missing or wrong");
        require(part.massKg && nearEqual(*part.massKg,0.0162),"STEP mass missing or wrong");
        found=true;
      } else {
        require(!part.densityKgM3 && !part.massKg,"a different part's density was incorrectly reused");
      }
    }
    require(found,"material test definition not found");
}
void checkXlsx(const QString& path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly),"XLSX did not open");
    const QByteArray bytes=file.readAll();
    require(bytes.startsWith("PK\003\004"),"XLSX is not a ZIP workbook");
    require(bytes.contains("xl/workbook.xml"),"XLSX workbook part missing");
    require(bytes.contains("xl/worksheets/sheet1.xml"),"XLSX first sheet missing");
}

// Read the generated workbook contents to verify exported values and provenance,
// using the already linked zlib dependency. Test workbooks are below ZIP64 limits.
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

void checkManualProperties() {
    Stp::PartDefinition part;
    part.volumeMm3=6000.; part.validSolid=true;
    require(Stp::materialEditable(part) && Stp::densityEditable(part),"missing source fields must be editable");
    require(Stp::effectiveMaterial(part).isEmpty() && !Stp::effectiveDensity(part) && !Stp::effectiveMass(part),
            "missing properties must not invent material, density or mass");
    require(Stp::setManualMaterial(part,QStringLiteral("  铝合金 ADC12  ")),"manual material input rejected");
    require(Stp::effectiveMaterial(part)==QStringLiteral("铝合金 ADC12") && part.material.isEmpty(),
            "manual material must be trimmed and preserve the source field");
    require(!Stp::effectiveDensity(part) && !Stp::effectiveMass(part),"material names must not infer density");
    QString error;
    require(Stp::setManualDensity(part,QStringLiteral("2700"),&error) && error.isEmpty(),"valid manual density rejected");
    require(Stp::effectiveDensity(part) && nearEqual(*Stp::effectiveDensity(part),2700.)
            && Stp::effectiveMass(part) && nearEqual(*Stp::effectiveMass(part),0.0162),"manual density mass conversion wrong");
    require(!part.densityKgM3 && !part.massKg,"manual input must not replace original density or cached original mass");
    require(Stp::materialEditable(part) && Stp::densityEditable(part),"manual fields must remain editable after input");
    for(const auto& invalid:{QStringLiteral("0"),QStringLiteral("-1"),QStringLiteral("NaN"),QStringLiteral("inf"),
                            QStringLiteral("1e400"),QStringLiteral("2,700"),QStringLiteral("steel")}) {
        require(!Stp::setManualDensity(part,invalid,&error) && !error.isEmpty(),"invalid density accepted or error absent");
        require(Stp::effectiveDensity(part) && nearEqual(*Stp::effectiveDensity(part),2700.),
                "invalid density input must retain previous accepted value");
    }
    require(Stp::setManualDensity(part,QStringLiteral(" 7.85e3 "),&error)
            && Stp::effectiveMass(part) && nearEqual(*Stp::effectiveMass(part),0.0471),"density update not used for mass");
    for(double invalid:{0.,-1.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        part.volumeMm3=invalid;
        require(!Stp::effectiveMass(part),"non-positive or non-finite volume produced mass");
    }
    part.volumeMm3=6000.; part.validSolid=false;
    require(!Stp::effectiveMass(part),"open or unverified geometry produced manual mass");
    part.validSolid=true; part.volumeMm3.reset();
    require(!Stp::effectiveMass(part),"missing volume produced manual mass");
    part.volumeMm3=6000.; part.manualDensityKgM3=std::numeric_limits<double>::infinity();
    require(!Stp::effectiveDensity(part) && !Stp::effectiveMass(part),"invalid stored manual density escaped validation");
    require(Stp::setManualDensity(part,QStringLiteral(" "),&error) && !part.manualDensityKgM3 && error.isEmpty(),
            "empty density must clear the supplement");
    require(Stp::setManualMaterial(part,QStringLiteral("")) && !part.manualMaterial,"empty material must clear the supplement");
    part.material=QStringLiteral("文件材料"); part.densityKgM3=2700.;
    require(!Stp::materialEditable(part) && !Stp::densityEditable(part),"file-provided fields must stay read-only");
    require(!Stp::setManualMaterial(part,QStringLiteral("人工材料")) && !Stp::setManualDensity(part,QStringLiteral("7850"),&error),
            "manual setters must reject overriding file fields");
    // Even stale supplement values cannot supersede the imported attributes.
    part.manualMaterial=QStringLiteral("ignored"); part.manualDensityKgM3=7850.;
    require(Stp::effectiveMaterial(part)==part.material && Stp::effectiveDensity(part)
            && nearEqual(*Stp::effectiveDensity(part),2700.),"source fields must take precedence over supplements");
    part.densityKgM3.reset();
    require(Stp::setManualDensity(part,QStringLiteral("8000")) && !Stp::setManualMaterial(part,QStringLiteral("changed")),
            "density and material editability must be independent");
    part.material.clear(); part.densityKgM3=2700.;
    require(Stp::setManualMaterial(part,QStringLiteral("独立补充")) && !Stp::setManualDensity(part,QStringLiteral("8000")),
            "material editability must not depend on density");
}

void checkScope(const Stp::CadDocument& document) {
    std::vector<int> firstPartLeaves;
    int firstPart=-1;
    for(size_t i=0;i<document.occurrences.size();++i) {
        const auto& occurrence=document.occurrences[i];
        if(occurrence.partIndex>=0 && nearEqual(*document.parts[occurrence.partIndex].volumeMm3,6000.)) {
            firstPart=occurrence.partIndex; firstPartLeaves.push_back(int(i));
        }
    }
    require(firstPartLeaves.size()==5,"scope fixture requires five repeated instances");
    const auto subset=Stp::scopedBomDocument(document,{firstPartLeaves[0],firstPartLeaves[1],firstPartLeaves[0],-1,99999,0});
    require(subset.occurrenceScopeApplied && !document.occurrenceScopeApplied
            && subset.parts.size()==1 && Stp::partQuantities(subset)==std::vector<int>{2},
            "scope must count exact checked occurrences and deduplicate duplicate indices");
    require(subset.parts[0].id==document.parts[firstPart].id && subset.parts[0].label==document.parts[firstPart].label
            && subset.xde==document.xde,"scope must retain original definition labels and XDE");
    for(size_t i=0;i<subset.occurrences.size();++i) {
        const auto& occurrence=subset.occurrences[i];
        require(occurrence.parent<int(subset.occurrences.size()) && occurrence.partIndex<int(subset.parts.size()),
                "scope vector references must be remapped");
        if(occurrence.parent>=0) {
            const auto& parent=subset.occurrences[occurrence.parent];
            require(std::find(parent.children.begin(),parent.children.end(),int(i))!=parent.children.end(),
                    "scope must retain parent-child links");
        }
        for(int child:occurrence.children)
            require(child>=0 && child<int(subset.occurrences.size()) && subset.occurrences[child].parent==int(i),
                    "scope contains a dangling or wrong child reference");
        const auto original=std::find_if(document.occurrences.begin(),document.occurrences.end(),[&](const auto& source) {
            return source.id==occurrence.id;
        });
        require(original!=document.occurrences.end() && original->path==occurrence.path,"scope must preserve stable IDs and paths");
        for(int row=1;row<=3;++row) for(int col=1;col<=4;++col)
            require(nearEqual(original->worldTransform.Value(row,col),occurrence.worldTransform.Value(row,col)),
                    "scope must not alter original instance placements");
    }
    const auto empty=Stp::scopedBomDocument(document,{});
    require(empty.parts.empty() && empty.occurrences.empty() && Stp::selectedPartIndices(empty,{}).empty(),
            "empty checked scope must never fall back to the full assembly");
    const auto invalidOnly=Stp::scopedBomDocument(document,{0,-1,99999});
    require(invalidOnly.parts.empty() && invalidOnly.occurrences.empty(),"scope must ignore non-leaf and invalid indices");
    std::vector<int> allLeaves;
    for(size_t i=0;i<document.occurrences.size();++i) if(document.occurrences[i].partIndex>=0) allLeaves.push_back(int(i));
    const auto excluded=firstPartLeaves.back();
    allLeaves.erase(std::remove(allLeaves.begin(),allLeaves.end(),excluded),allLeaves.end());
    const auto partial=Stp::scopedBomDocument(document,allLeaves);
    require(partial.parts.size()==2,"excluding one repeated instance must not erase its definition");
    const auto quantities=Stp::partQuantities(partial);
    require(quantities[firstPart]==4 && quantities[1-firstPart]==2,"scope counts must exclude only the unchecked instance");
}
}

int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        // Exercise STEP Unicode names independently of the host's C locale.
        require(std::setlocale(LC_CTYPE,"C")!=nullptr,"cannot set regression C character locale");
        auto doc=fixture(); checkCounts(doc); checkManualProperties(); checkScope(doc);
        if(application.arguments().size()==4 && application.arguments()[1]==QStringLiteral("--generate-benchmark")) {
            bool ok=false; const int count=application.arguments()[2].toInt(&ok);
            require(ok,"benchmark instance count must be numeric");
            auto benchmark=repeatedFixture(count);
            writeStep(benchmark,application.arguments()[3],DESTEP_Parameters::WriteMode_StepSchema_AP242DIS);
            std::atomic_bool cancel(false);
            const auto imported=Stp::loadStep(application.arguments()[3],{},cancel);
            require(imported->parts.size()==1 && imported->occurrences.size()==size_t(count+1),"benchmark STEP roundtrip count changed");
            require(Stp::partQuantities(*imported)[0]==count,"benchmark STEP roundtrip leaf quantity changed");
            std::cout<<"Generated benchmark: 1 definition, "<<count<<" leaf instances, "<<count+1<<" total occurrences\n";
            return 0;
        }
        if(application.arguments().size()==3 && application.arguments()[1]==QStringLiteral("--generate-sample")) {
            writeStep(doc,application.arguments()[2],DESTEP_Parameters::WriteMode_StepSchema_AP242DIS);
            std::atomic_bool cancel(false);
            const auto imported=Stp::loadStep(application.arguments()[2],{},cancel);
            checkCounts(*imported); checkTransforms(*imported); checkMaterial(*imported); checkColors(*imported);
            std::cout<<"Generated AP242 sample: 2 definitions, 7 leaf instances, 10 total occurrences; "
                         "nested placements, part/face/instance colors, Aluminium 2700 kg/m3 and 0.0162 kg verified\n";
            return 0;
        }
        QTemporaryDir temporary; require(temporary.isValid(),"temporary directory failed");
        const QString unicodeDir=temporary.filePath(QStringLiteral("中文 路径"));
        require(QDir().mkdir(unicodeDir),"Unicode test directory failed");
        const QDir output(unicodeDir);
        std::atomic_bool cancel(false);
        bool foundTransform=false;
        for(const auto& occurrence:doc.occurrences)
            if(occurrence.partIndex>=0 && nearEqual(occurrence.worldTransform.Value(1,4),100.)
               && nearEqual(occurrence.worldTransform.Value(2,4),10.)) foundTransform=true;
        require(foundTransform,"nested rotation/translation order wrong");
        bool foundMass=false;
        for(const auto& part:doc.parts) {
            if(part.densityKgM3) {
                require(nearEqual(*part.densityKgM3,2700.),"density kg/m3 conversion incorrect");
                require(part.massKg && nearEqual(*part.massKg,0.0162),"mass conversion incorrect"); foundMass=true;
            } else require(!part.massKg,"mass invented for unspecified density");
        }
        require(foundMass,"fixture density not read");
        require(Stp::safePartFilename(QStringLiteral("CON.txt"),1)==QStringLiteral("_CON.txt"),"Windows reserved name handling failed");
        require(Stp::safePartFilename(QStringLiteral("a/b:c?*"),1)==QStringLiteral("a_b_c__"),"filename sanitization failed");
        require(Stp::safePartFilename(QStringLiteral("..."),9)==QStringLiteral("Part_0009"),"empty filename fallback failed");

        for(auto schema:{DESTEP_Parameters::WriteMode_StepSchema_AP203,
                         DESTEP_Parameters::WriteMode_StepSchema_AP214IS,
                         DESTEP_Parameters::WriteMode_StepSchema_AP242DIS}) {
            const QString path=output.filePath(QStringLiteral("装配_%1.step").arg(int(schema)));
            writeStep(doc,path,schema);
            auto imported=Stp::loadStep(path,{},cancel); checkCounts(*imported); checkTransforms(*imported);
            if(schema!=DESTEP_Parameters::WriteMode_StepSchema_AP203) { checkMaterial(*imported); checkColors(*imported); }
        }
        const QString distinctNamesFile=output.filePath(QStringLiteral("unicode-name-ascii-id.step"));
        writeStep(doc,distinctNamesFile,DESTEP_Parameters::WriteMode_StepSchema_AP242DIS,
                  UnitsMethods_LengthUnit_Millimeter,true,true);
        const auto distinctNames=Stp::loadStep(distinctNamesFile,{},cancel);
        checkCounts(*distinctNames);
        for(const auto& part:distinctNames->parts)
            require(part.name==QStringLiteral("同名零件"),"Unicode product name must not fall back to its distinct ASCII ID");
        const QString inchFile=output.filePath(QStringLiteral("inch.step"));
        writeStep(doc,inchFile,DESTEP_Parameters::WriteMode_StepSchema_AP242DIS,UnitsMethods_LengthUnit_Inch);
        auto inch=Stp::loadStep(inchFile,{},cancel); checkCounts(*inch); checkTransforms(*inch); checkMaterial(*inch);

        // The pinned OCCT writer emits an invalid density derived-unit exponent
        // pair without our targeted output repair. Input must reject that value.
        const QString malformedDensityFile=output.filePath(QStringLiteral("invalid-density-units.step"));
        writeStep(doc,malformedDensityFile,DESTEP_Parameters::WriteMode_StepSchema_AP242DIS,
                  UnitsMethods_LengthUnit_Millimeter,false);
        const auto malformedDensity=Stp::loadStep(malformedDensityFile,{},cancel);
        for(const auto& part:malformedDensity->parts)
            require(!part.densityKgM3 && !part.massKg,"invalid density unit exponents produced a mass");
        require(malformedDensity->diagnostics.join('\n').contains(QStringLiteral("密度、质量留空")),
                "invalid density dimensions must produce a diagnostic");

        Stp::CadDocument external; external.xde=newDocument();
        const auto externalShapes=XCAFDoc_DocumentTool::ShapeTool(external.xde->Main());
        const auto externalPart=externalShapes->AddShape(BRepPrimAPI_MakeBox(1,2,3).Shape(),false);
        TColStd_SequenceOfHAsciiString references;
        references.Append(new TCollection_HAsciiString("missing-component.step"));
        externalShapes->SetExternRefs(externalPart,references);
        Stp::rebuildIndex(external);
        require(external.diagnostics.join('\n').contains(QStringLiteral("外部引用：missing-component.step")),
                "actual external reference marker must be reported");

        const QString bom=output.filePath(QStringLiteral("物料清单.xlsx"));
        Stp::exportBom(doc,bom,&cancel); checkXlsx(bom);
        QFile workbook(bom); workbook.open(QIODevice::ReadOnly);
        const QByteArray originalWorkbook=workbook.readAll(); workbook.close();
        require(originalWorkbook.contains("xl/worksheets/sheet4.xml"),"BOM needs four sheets");
        require(!xlsxSheetCells(bom).contains(QStringLiteral("Q1")),"source-only BOM must keep its existing column structure");
        cancel=true;
        bool bomCancelled=false;
        try { Stp::exportBom(doc,bom,&cancel); } catch(const std::exception& e) {
            bomCancelled=QString::fromUtf8(e.what()).contains(QStringLiteral("取消"));
        }
        require(bomCancelled,"cancelled BOM export must report cancellation");
        require(workbook.open(QIODevice::ReadOnly) && workbook.readAll()==originalWorkbook,
                "cancelled BOM export must preserve existing target bytes"); workbook.close();
        const QString cancelledBom=output.filePath(QStringLiteral("取消的新清单.xlsx"));
        bomCancelled=false;
        try { Stp::exportBom(doc,cancelledBom,&cancel); } catch(const std::exception&) { bomCancelled=true; }
        require(bomCancelled && !QFileInfo::exists(cancelledBom),"cancelled BOM export must not create a target");
        cancel=false;
        Stp::exportBom(doc,bom,&cancel); checkXlsx(bom);

        // A checked subset includes only one of the two repeated small-part
        // instances. Supplements participate in its BOM, without becoming STEP
        // source attributes or changing the original full-document quantities.
        auto manualDoc=doc;
        int manualPart=-1, manualLeaf=-1;
        for(size_t i=0;i<manualDoc.occurrences.size();++i) {
            const int partIndex=manualDoc.occurrences[i].partIndex;
            if(partIndex>=0 && !manualDoc.parts[partIndex].densityKgM3) {
                manualPart=partIndex; manualLeaf=int(i); break;
            }
        }
        require(manualPart>=0 && manualLeaf>=0,"fixture needs a part missing material and density");
        require(Stp::setManualMaterial(manualDoc.parts[manualPart],QStringLiteral("人工钢材"))
            && Stp::setManualDensity(manualDoc.parts[manualPart],QStringLiteral("7850")),"manual export fixture setup failed");
        auto manualSubset=Stp::scopedBomDocument(manualDoc,{manualLeaf,manualLeaf});
        require(manualSubset.parts.size()==1 && Stp::partQuantities(manualSubset)==std::vector<int>{1}
            && Stp::effectiveMass(manualSubset.parts[0]) && nearEqual(*Stp::effectiveMass(manualSubset.parts[0]),0.00098125),
                "scoped manual data or mass lost");
        const QString manualBom=output.filePath(QStringLiteral("人工补充选中清单.xlsx"));
        Stp::exportBom(manualSubset,manualBom,&cancel); checkXlsx(manualBom);
        const auto manualCells=xlsxSheetCells(manualBom);
        require(manualCells.value(QStringLiteral("E2"))==QStringLiteral("1")
                && manualCells.value(QStringLiteral("F2"))==QStringLiteral("人工钢材")
                && nearEqual(manualCells.value(QStringLiteral("G2")).toDouble(),7850.)
                && nearEqual(manualCells.value(QStringLiteral("M2")).toDouble(),0.00098125)
                && nearEqual(manualCells.value(QStringLiteral("N2")).toDouble(),0.00098125),
                "scoped BOM workbook contains wrong quantity/manual material/density/mass");
        require(manualCells.value(QStringLiteral("Q1"))==QStringLiteral("材料来源")
                && manualCells.value(QStringLiteral("R1"))==QStringLiteral("密度来源")
                && manualCells.value(QStringLiteral("Q2"))==QStringLiteral("人工输入")
                && manualCells.value(QStringLiteral("R2"))==QStringLiteral("人工输入")
                && manualCells.value(QStringLiteral("S2"))==QStringLiteral("几何计算＋人工密度"),
                "manual BOM workbook must disclose field and mass provenance");
        const auto manualPositions=xlsxSheetCells(manualBom,"xl/worksheets/sheet3.xml");
        require(manualPositions.value(QStringLiteral("A2"))==manualDoc.occurrences[manualLeaf].id
                && !manualPositions.contains(QStringLiteral("A3")),"scoped workbook must contain only the checked instance");
        const auto manualExport=Stp::exportParts(manualSubset,{},unicodeDir,{},cancel);
        require(manualExport.files.size()==1,"scoped split export must emit only checked definitions");
        checkXlsx(manualExport.manifestPath);
        const auto scopedManifest=xlsxSheetCells(manualExport.manifestPath);
        require(scopedManifest.value(QStringLiteral("E1"))==QStringLiteral("本次勾选数量")
                && scopedManifest.value(QStringLiteral("E2"))==QStringLiteral("1")
                && !scopedManifest.contains(QStringLiteral("A3")),"split manifest must label and count only the checked scope");
        const auto manualReadBack=Stp::loadStep(manualExport.files.front(),{},cancel);
        require(manualReadBack->parts.size()==1 && manualReadBack->parts[0].material.isEmpty()
                && !manualReadBack->parts[0].densityKgM3 && !manualReadBack->parts[0].massKg,
                "manual internal attributes must not leak into split STEP");
        require(Stp::partQuantities(doc)[manualPart]==2 && !doc.parts[manualPart].manualMaterial
            && !doc.parts[manualPart].manualDensityKgM3,"scope/manual copy mutated the source document");
        bool emptyScopeRejected=false;
        try { Stp::exportParts(Stp::scopedBomDocument(doc,{}),{},unicodeDir,{},cancel); }
        catch(const std::exception& error) { emptyScopeRejected=QString::fromUtf8(error.what()).contains(QStringLiteral("没有可导出")); }
        require(emptyScopeRejected,"empty checked scope silently exported the full model");
        const auto exported=Stp::exportParts(doc,{},unicodeDir,{},cancel);
        require(exported.files.size()==2,"leaf export must deduplicate by definition"); checkXlsx(exported.manifestPath);
        const auto originalManifest=xlsxSheetCells(exported.manifestPath);
        require(originalManifest.value(QStringLiteral("E1"))==QStringLiteral("原装配总数量")
                && originalManifest.value(QStringLiteral("E2")).toInt()==Stp::partQuantities(doc)[0]
                && originalManifest.value(QStringLiteral("E3")).toInt()==Stp::partQuantities(doc)[1],
                "unscoped split manifest must preserve its full-source quantity meaning");
        bool exportedRed=false;
        for(const auto& path:exported.files) {
            auto imported=Stp::loadStep(path,{},cancel);
            require(imported->parts.size()==1,"part export contains assembly or multiple definitions");
            require(imported->occurrences.size()==1,"export retained source assembly positions");
            const auto& part=imported->parts[0]; require(part.volumeMm3.has_value(),"exported volume invalid");
            require(nearEqual(*part.volumeMm3,6000.,0.001)||nearEqual(*part.volumeMm3,125.,0.001),"export changed volume");
            require(nearEqual(imported->occurrences[0].worldTransform.Value(1,4),0.),"export should be local coordinates");
            if(nearEqual(*part.volumeMm3,6000.,0.001)) {
                checkMaterial(*imported);
                Quantity_Color color;
                auto colors=XCAFDoc_DocumentTool::ColorTool(imported->xde->Main());
                require(colors->GetColor(part.label,XCAFDoc_ColorSurf,color),"part color lost on export");
                require(color.Red()>0.9 && color.Green()<0.1,"part color changed");
                bool greenFace=false; TDF_LabelSequence subLabels;
                XCAFDoc_DocumentTool::ShapeTool(imported->xde->Main())->GetSubShapes(part.label,subLabels);
                for(int i=1;i<=subLabels.Length();++i)
                    if(colors->GetColor(subLabels(i),XCAFDoc_ColorSurf,color) && color.Green()>0.9 && color.Red()<0.1) greenFace=true;
                require(greenFace,"face color lost on export"); exportedRed=true;
            }
        }
        require(exportedRed,"colored part absent");

        // OCCT can copy PMI into XDE but drop/change it while serializing STEP.
        // A standalone datum is intentionally not referenced by a tolerance.
        Stp::CadDocument pmiDocument; pmiDocument.xde=newDocument();
        const auto pmiShape=XCAFDoc_DocumentTool::ShapeTool(pmiDocument.xde->Main())->AddShape(
            BRepPrimAPI_MakeBox(10,20,30).Shape(),false);
        const auto pmiTool=XCAFDoc_DocumentTool::DimTolTool(pmiDocument.xde->Main());
        const auto dimensionLabel=pmiTool->AddDimension();
        Handle(XCAFDimTolObjects_DimensionObject) dimension=new XCAFDimTolObjects_DimensionObject;
        dimension->SetType(XCAFDimTolObjects_DimensionType_Location_LinearDistance);
        dimension->SetValue(10.); dimension->SetPoint(gp_Pnt(0,0,0)); dimension->SetPoint2(gp_Pnt(10,0,0));
        dimension->SetSemanticName(new TCollection_HAsciiString("Nominal width"));
        XCAFDoc_Dimension::Set(dimensionLabel)->SetObject(dimension);
        pmiTool->SetDimension(pmiShape,dimensionLabel);
        const auto datumLabel=pmiTool->AddDatum();
        Handle(XCAFDimTolObjects_DatumObject) datum=new XCAFDimTolObjects_DatumObject;
        datum->SetName(new TCollection_HAsciiString("A"));
        XCAFDoc_Datum::Set(datumLabel)->SetObject(datum);
        TDF_LabelSequence pmiReferences; pmiReferences.Append(pmiShape); pmiTool->SetDatum(pmiReferences,datumLabel);
        Stp::rebuildIndex(pmiDocument);
        require(Stp::buildPmiCatalog(pmiDocument).records.size()==2,"PMI export fixture needs two records");
        const auto pmiExport=Stp::exportParts(pmiDocument,{},unicodeDir,{},cancel);
        const auto pmiReadBack=Stp::loadStep(pmiExport.files.front(),{},cancel);
        const auto readBackCatalog=Stp::buildPmiCatalog(*pmiReadBack);
        const QString pmiReport=pmiExport.diagnostics.join('\n');
        require(pmiReport.contains(QStringLiteral("PMI 写入回读核验：临时 XDE 2 项")),"PMI export skipped write/readback audit");
        require(pmiReport.contains(QStringLiteral("STEP 回读 %1 项").arg(readBackCatalog.records.size())),"PMI export reported incorrect readback count");
        require(readBackCatalog.records.size()<2,"standalone datum fixture should exercise writer data loss");
        require(pmiReport.contains(QStringLiteral("可能丢失或改变")),"PMI writer data loss was silently ignored");
        require(pmiReport.contains(QStringLiteral("也不证明全部修饰符")),"PMI audit must disclose its equivalence limits");

        // A scoped view keeps source XDE labels. Removing the other part must
        // not turn a cross-part dimension into apparently independent PMI.
        const auto otherPmiShape=XCAFDoc_DocumentTool::ShapeTool(pmiDocument.xde->Main())->AddShape(
            BRepPrimAPI_MakeBox(gp_Pnt(60,0,0),5,5,5).Shape(),false);
        const auto crossDimensionLabel=pmiTool->AddDimension();
        Handle(XCAFDimTolObjects_DimensionObject) crossDimension=new XCAFDimTolObjects_DimensionObject;
        crossDimension->SetType(XCAFDimTolObjects_DimensionType_Location_LinearDistance);
        crossDimension->SetValue(60.); crossDimension->SetPoint(gp_Pnt(0,0,0)); crossDimension->SetPoint2(gp_Pnt(60,0,0));
        crossDimension->SetSemanticName(new TCollection_HAsciiString("Across parts"));
        XCAFDoc_Dimension::Set(crossDimensionLabel)->SetObject(crossDimension);
        TDF_LabelSequence crossFirst,crossSecond; crossFirst.Append(pmiShape); crossSecond.Append(otherPmiShape);
        pmiTool->SetDimension(crossFirst,crossSecond,crossDimensionLabel);
        Stp::rebuildIndex(pmiDocument);
        int pmiLeaf=-1;
        for(size_t i=0;i<pmiDocument.occurrences.size();++i)
            if(pmiDocument.occurrences[i].definitionLabel==pmiShape) pmiLeaf=int(i);
        require(pmiLeaf>=0,"scoped PMI fixture leaf missing");
        const auto scopedPmi=Stp::scopedBomDocument(pmiDocument,{pmiLeaf});
        const auto scopedCatalog=Stp::buildPmiCatalog(scopedPmi);
        bool independentKept=false,crossRejected=false;
        for(const auto& record:scopedCatalog.records) {
            if(record.name==QStringLiteral("Nominal width")) independentKept=record.safeForPartExport;
            if(record.name==QStringLiteral("Across parts")) crossRejected=record.crossPart && !record.safeForPartExport;
        }
        require(independentKept && crossRejected,"scope incorrectly lost independent PMI or promoted cross-part PMI");
        const auto scopedPmiExport=Stp::exportParts(scopedPmi,{},unicodeDir,{},cancel);
        require(scopedPmiExport.files.size()==1 && scopedPmiExport.diagnostics.join('\n').contains(
            QStringLiteral("Across parts：未复制，关联跨越多个零件")),"scoped export must explicitly report excluded cross-part PMI");

        Stp::CadDocument repeatedPmi; repeatedPmi.xde=newDocument();
        const auto repeatedShapes=XCAFDoc_DocumentTool::ShapeTool(repeatedPmi.xde->Main());
        const auto repeatedPart=repeatedShapes->AddShape(BRepPrimAPI_MakeBox(10,20,30).Shape(),false);
        const auto repeatedRoot=emptyAssembly(repeatedShapes,"Repeated PMI assembly");
        repeatedShapes->AddComponent(repeatedRoot,repeatedPart,translation(0,0,0));
        repeatedShapes->AddComponent(repeatedRoot,repeatedPart,translation(100,0,0));
        repeatedShapes->UpdateAssemblies();
        const auto repeatedTool=XCAFDoc_DocumentTool::DimTolTool(repeatedPmi.xde->Main());
        const auto repeatedDimensionLabel=repeatedTool->AddDimension();
        Handle(XCAFDimTolObjects_DimensionObject) repeatedDimension=new XCAFDimTolObjects_DimensionObject;
        repeatedDimension->SetType(XCAFDimTolObjects_DimensionType_Location_LinearDistance);
        repeatedDimension->SetValue(10.); repeatedDimension->SetPoint(gp_Pnt(0,0,0)); repeatedDimension->SetPoint2(gp_Pnt(10,0,0));
        XCAFDoc_Dimension::Set(repeatedDimensionLabel)->SetObject(repeatedDimension);
        repeatedTool->SetDimension(repeatedPart,repeatedDimensionLabel);
        Stp::rebuildIndex(repeatedPmi);
        int identityLeaf=-1;
        for(size_t i=0;i<repeatedPmi.occurrences.size();++i)
            if(repeatedPmi.occurrences[i].partIndex>=0 && nearEqual(repeatedPmi.occurrences[i].worldTransform.Value(1,4),0.))
                identityLeaf=int(i);
        require(identityLeaf>=0,"repeated PMI identity instance missing");
        const auto repeatedCatalog=Stp::buildPmiCatalog(repeatedPmi);
        const auto identitySubset=Stp::scopedBomDocument(repeatedPmi,{identityLeaf});
        const auto subsetRepeatedCatalog=Stp::buildPmiCatalog(identitySubset);
        require(identitySubset.sourceAssemblyContext && identitySubset.sourceAssemblyContext->occurrences.size()==3
                && identitySubset.occurrences.size()==2,"PMI scope must retain the original complete assembly context");
        require(repeatedCatalog.records.size()==1 && subsetRepeatedCatalog.records.size()==1
                && !repeatedCatalog.records[0].occurrencePlacementVerified && !repeatedCatalog.records[0].safeForPartExport
                && !subsetRepeatedCatalog.records[0].occurrencePlacementVerified && !subsetRepeatedCatalog.records[0].safeForPartExport
                && subsetRepeatedCatalog.records[0].ownerOccurrenceIds.size()==2,
                "filtering repeated instances must not upgrade PMI placement or split-export confidence");
        const auto second=Stp::exportParts(doc,{},unicodeDir,{},cancel);
        require(second.directory!=exported.directory,"second export must never overwrite first");
#ifdef Q_OS_MACOS
        checkMacExportVisibility(doc,unicodeDir,cancel);
#endif

        // A root may itself be a located XDE reference. Its transform belongs to
        // the occurrence; the definition retains even a non-zero intrinsic origin.
        Stp::CadDocument located; located.xde=newDocument();
        auto locatedShapes=XCAFDoc_DocumentTool::ShapeTool(located.xde->Main());
        const auto offsetBox=BRepPrimAPI_MakeBox(gp_Pnt(7,-3,11),4,5,6).Shape();
        locatedShapes->AddShape(offsetBox.Moved(translation(41,52,63)),false);
        Stp::rebuildIndex(located);
        require(located.parts.size()==1 && located.occurrences.size()==1,"located root creates one definition/occurrence");
        const auto& rootTransform=located.occurrences[0].worldTransform;
        require(nearEqual(rootTransform.Value(1,4),41) && nearEqual(rootTransform.Value(2,4),52)
            && nearEqual(rootTransform.Value(3,4),63),"located root placement lost");
        auto localExport=Stp::exportParts(located,{},unicodeDir,{},cancel);
        auto localImport=Stp::loadStep(localExport.files[0],{},cancel);
        Bnd_Box bounds; BRepBndLib::AddOptimal(localImport->parts[0].shape,bounds,false,false);
        double x1,y1,z1,x2,y2,z2; bounds.Get(x1,y1,z1,x2,y2,z2);
        require(nearEqual(x1,7) && nearEqual(y1,-3) && nearEqual(z1,11) && nearEqual(x2,11) && nearEqual(y2,2) && nearEqual(z2,17),
            "split export changed definition's intrinsic coordinates or baked in occurrence placement");
        const auto selected=Stp::selectedPartIndices(doc,{1,1,2});
        require(selected.size()==2,"subassembly selection must deduplicate descendant definitions");
        const auto onePart=Stp::exportParts(doc,{2,3},unicodeDir,{},cancel);
        require(onePart.files.size()==1,"two selected instances must export one definition");
        const auto beforeCancel=QDir(unicodeDir).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot);
        cancel=true; bool exportCancelled=false;
        try { Stp::exportParts(doc,{},unicodeDir,{},cancel); } catch(const std::exception&) { exportCancelled=true; }
        cancel=false;
        require(exportCancelled && beforeCancel==QDir(unicodeDir).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot),
            "cancelled export must not leave partial output");

        Stp::CadDocument flat; flat.xde=newDocument();
        TopoDS_Compound compound; BRep_Builder builder; builder.MakeCompound(compound);
        builder.Add(compound,BRepPrimAPI_MakeBox(2,3,4).Shape());
        builder.Add(compound,BRepPrimAPI_MakeBox(gp_Pnt(10,0,0),2,3,4).Shape());
        XCAFDoc_DocumentTool::ShapeTool(flat.xde->Main())->AddShape(compound,false);
        Stp::rebuildIndex(flat);
        require(flat.parts.size()==1,"flat compound must not invent separate product definitions");
        require(!flat.diagnostics.empty(),"flat compound needs structure warning");
        Stp::CadDocument open; open.xde=newDocument();
        XCAFDoc_DocumentTool::ShapeTool(open.xde->Main())->AddShape(
            BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0,0,0),gp_Dir(0,0,1)),0.,10.,0.,10.).Shape(),false);
        Stp::rebuildIndex(open);
        require(!open.parts[0].validSolid && !open.parts[0].volumeMm3 && !open.parts[0].massKg,"open surface must not claim volume or mass");

        bool cancelled=false; cancel=true;
        try { Stp::loadStep(inchFile,{},cancel); } catch(const std::exception&) { cancelled=true; }
        require(cancelled,"cancelled import must not produce document"); cancel=false;
        const QString broken=output.filePath(QStringLiteral("broken.step"));
        QFile brokenFile(broken); brokenFile.open(QIODevice::WriteOnly); brokenFile.write("not a step"); brokenFile.close();
        bool rejected=false;
        try { Stp::loadStep(broken,{},cancel); } catch(const std::exception&) { rejected=true; }
        require(rejected,"corrupt input accepted");
        std::cout << "PASS: assembly definitions/counts, nested transforms, AP203/AP214/AP242, inch conversion, "
                     "density/mass, manual supplement validation/clear/source preservation, exact checked scope, "
                     "Unicode paths, XLSX sheets, exact STEP split roundtrip, scoped PMI safety, part/face colors, "
                     "safe names, no overwrite, flat/open geometry, cancellation, corrupt input\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    } catch(const Standard_Failure& error) {
        std::cerr << "FAIL OCCT: " << error.GetMessageString() << '\n'; return 1;
    }
}
