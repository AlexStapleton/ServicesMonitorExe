// tray.cpp — System tray management
#include "app.h"

HICON load_app_icon(HINSTANCE hInst, int cx, int cy) {
#ifdef IDI_APPICON
    return (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, cx, cy, 0);
#else
    (void)hInst; (void)cx; (void)cy;
    return LoadIconW(NULL, IDI_APPLICATION);
#endif
}

void tray_remove(App& self) {
    if (self.tray.added) {
        Shell_NotifyIconW(NIM_DELETE, &self.tray.nid);
        self.tray.added = false;
        ZeroMemory(&self.tray.nid, sizeof(self.tray.nid));
    }
}

void tray_add(App& self, HINSTANCE hInst) {
    if (!self.hwnd) return;
    if (!self.tray.enabled) return;

    ZeroMemory(&self.tray.nid, sizeof(self.tray.nid));
    self.tray.nid.cbSize = sizeof(self.tray.nid);
    self.tray.nid.hWnd = self.hwnd;
    self.tray.nid.uID = TRAY_UID;
    self.tray.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    self.tray.nid.uCallbackMessage = WM_APP_TRAYICON;

    HICON ico = load_app_icon(hInst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    self.tray.nid.hIcon = ico ? ico : LoadIconW(NULL, IDI_APPLICATION);
    StringCchCopyW(self.tray.nid.szTip, ARRAYSIZE(self.tray.nid.szTip), self.app_title.c_str());

    if (Shell_NotifyIconW(NIM_ADD, &self.tray.nid)) {
        self.tray.added = true;
        self.tray.nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &self.tray.nid);
    }
}

void tray_sync(App& self, HINSTANCE hInst) {
    if (!self.tray.enabled) {
        tray_remove(self);
        return;
    }
    if (!self.tray.added) tray_add(self, hInst);
}

void tray_show_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOWHIDE, L"Show window");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Quit");

    // Required so the menu dismisses correctly
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}
