// Give hosted Windows GUI tests a desktop large enough for the 1440x831 cases.
// This helper changes only the disposable runner session, never the registry.
#include <windows.h>
#include <iostream>

int main()
{
    wchar_t githubActions[16]{};
    GetEnvironmentVariableW(L"GITHUB_ACTIONS",githubActions,16);
    if(lstrcmpiW(githubActions,L"true")!=0){
        std::cerr<<"This helper is restricted to GitHub Actions test hosts.\n";return 2;
    }
    DEVMODEW mode{};mode.dmSize=sizeof(mode);
    if(!EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode)){
        std::cerr<<"Cannot read the current display mode.\n";return 1;
    }
    std::cout<<"Initial CI display: "<<mode.dmPelsWidth<<'x'<<mode.dmPelsHeight<<'\n';
    mode.dmPelsWidth=1920;mode.dmPelsHeight=1080;
    mode.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT;
    const auto supported=ChangeDisplaySettingsW(&mode,CDS_TEST);
    if(supported!=DISP_CHANGE_SUCCESSFUL){
        std::cerr<<"1920x1080 is unsupported: "<<supported<<'\n';return 1;
    }
    const auto applied=ChangeDisplaySettingsW(&mode,0);
    if(applied!=DISP_CHANGE_SUCCESSFUL){
        std::cerr<<"Cannot apply CI display mode: "<<applied<<'\n';return 1;
    }
    DEVMODEW actual{};actual.dmSize=sizeof(actual);
    if(!EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&actual))return 1;
    std::cout<<"GUI test display: "<<actual.dmPelsWidth<<'x'<<actual.dmPelsHeight<<'\n';
    return actual.dmPelsWidth==1920&&actual.dmPelsHeight==1080?0:1;
}
