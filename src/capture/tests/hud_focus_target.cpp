// Non-GPU foreground sink for the HUD input acceptance experiment.
#include <windows.h>
#include <string>
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM key,LPARAM flags){
    static unsigned chords=0;
    if(message==WM_KEYDOWN && !(flags&(LPARAM(1)<<30)) && (key==VK_F1 || key=='C')){
        const auto title=L"Flora input focus sink - keys "+std::to_wstring(++chords);SetWindowTextW(window,title.c_str());
    }
    if(message==WM_CLOSE){DestroyWindow(window);return 0;}
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,key,flags);
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    WNDCLASSW type{};type.lpfnWndProc=procedure;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"FloraHudFocusSink";if(!RegisterClassW(&type))return 1;
    HWND window=CreateWindowW(type.lpszClassName,L"Flora input focus sink - keys 0",WS_OVERLAPPEDWINDOW,700,100,400,200,nullptr,nullptr,type.hInstance,nullptr);if(!window)return 1;
    ShowWindow(window,SW_SHOW);SetForegroundWindow(window);MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}return 0;
}
