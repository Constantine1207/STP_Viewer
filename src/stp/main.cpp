#include "main_window.h"
#include "document_workspace.h"
#include "../app/qtopengl_utils.h"
#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QStyleFactory>
#include <QPalette>
#include <QTimer>
#include <QTranslator>
#include <QLibraryInfo>
#include <QVariant>
#include <Standard_Failure.hxx>

static void setupResources()
{
    const QDir bin(QCoreApplication::applicationDirPath());
    const QStringList roots {bin.absoluteFilePath("../Resources/opencascade"),
        bin.absoluteFilePath("resources/opencascade"),QStringLiteral(STP_OCCT_RESOURCE_DIR)};
    for(const QString& root:roots){
        if(!QFileInfo::exists(root+"/Shaders"))continue;
        qputenv("CSF_ShadersDirectory",QFile::encodeName(root+"/Shaders"));
        qputenv("CSF_XSMessage",QFile::encodeName(root+"/XSMessage"));
        qputenv("CSF_SHMessage",QFile::encodeName(root+"/SHMessage"));
        qputenv("CSF_STEPDefaults",QFile::encodeName(root+"/XSTEPResource"));
        qputenv("CSF_StandardDefaults",QFile::encodeName(root+"/StdResource"));
        qputenv("CSF_PluginDefaults",QFile::encodeName(root+"/StdResource"));
        break;
    }
}

int main(int argc,char** argv)
{
    Mayo::QtOpenGlUtils::platformSetup(argc,argv);
#if defined(Q_OS_MACOS)
    // Apple's OpenGL implementation supports 4.1; the upstream generic core
    // profile requests 4.5, which cannot create a native context on macOS.
    auto format=QSurfaceFormat::defaultFormat();format.setVersion(4,1);format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
#endif
    QApplication app(argc,argv);
#ifdef Q_OS_WIN
    app.setWindowIcon(QIcon(QStringLiteral(":/stp/aeme.png")));
#endif
    // A fixed light palette keeps the engineering tables legible even when
    // macOS/Windows uses a dark system theme.
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#eef2f7"));
    palette.setColor(QPalette::WindowText, QColor("#233a56"));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor("#f6f8fb"));
    palette.setColor(QPalette::Text, QColor("#233a56"));
    palette.setColor(QPalette::PlaceholderText, QColor("#60758b"));
    palette.setColor(QPalette::Button, QColor("#f0f4f9"));
    palette.setColor(QPalette::ButtonText, QColor("#233a56"));
    palette.setColor(QPalette::Highlight, QColor("#2f6cb0"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, Qt::white);
    palette.setColor(QPalette::ToolTipText, QColor("#233a56"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#8997a7"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#8997a7"));
    app.setPalette(palette);
    QCoreApplication::setApplicationName("STPViewer");QCoreApplication::setApplicationVersion("1.0.0");
    QCoreApplication::setOrganizationName("STPViewer");
    QCommandLineParser parser;parser.setApplicationDescription(QStringLiteral("离线 STEP 工程查看器"));parser.addHelpOption();parser.addVersionOption();
    parser.addPositionalArgument("file",QStringLiteral("要打开的 .stp / .step 文件"));
    QCommandLineOption smoke("smoke-test",QStringLiteral("读取位置参数中的文件，并输出截图、Excel 和拆分结果后退出"),"output-dir");parser.addOption(smoke);parser.process(app);
    setupResources();
    QTranslator translator;
    const QDir bin(QCoreApplication::applicationDirPath());
    const QStringList translationRoots {bin.absoluteFilePath("../Resources/translations"),
        bin.absoluteFilePath("translations"),QLibraryInfo::path(QLibraryInfo::TranslationsPath),
        QStringLiteral(STP_TRANSLATION_DIR)};
    app.setProperty("qtChineseTranslationLoaded",false);
    for(const auto& root:translationRoots) {
        if(!translator.load("qtbase_zh_CN",root)) continue;
        app.installTranslator(&translator);
        app.setProperty("qtChineseTranslationLoaded",true);
        break;
    }
    try {
        if(!parser.isSet(smoke)){
            Stp::DocumentWorkspace workspace;workspace.show();
            const auto paths=parser.positionalArguments();
            if(!paths.isEmpty())QTimer::singleShot(100,&workspace,[&workspace,paths]{workspace.openFiles(paths);});
            return app.exec();
        }
        Stp::MainWindow window;window.show();
        if(!parser.positionalArguments().isEmpty()){
            const auto path=QFileInfo(parser.positionalArguments().front()).absoluteFilePath();
            if(parser.isSet(smoke))window.runSmokeTest(path,QDir(parser.value(smoke)).absolutePath());
            else QTimer::singleShot(100,&window,[&window,path]{window.openStep(path);});
        }
        return app.exec();
    }catch(const Standard_Failure& error){QMessageBox::critical(nullptr,QStringLiteral("三维引擎启动失败"),QString::fromUtf8(error.GetMessageString()));return 2;}
    catch(const std::exception& error){QMessageBox::critical(nullptr,QStringLiteral("启动失败"),QString::fromUtf8(error.what()));return 2;}
}
