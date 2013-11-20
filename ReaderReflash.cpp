/*
* ReaderReflash.c --
*
* Reader Configuration console.
*
* History:
*
* Multi-threaded I/O.
*
* Hunt for a reader at startup.  Look for "CMD>" 
*
* Try to rewrite it without multithreaded I/O.  
*  Don't keep opening and closing port.
*  Should be structured then as:
*  1 thread GUI.
*  1 thread for serial IO.
* When idle, 
*  serial thread sits in a loop reading from com port with a fast timeout.
*  GUI thread can write to com port.
*
* When reflashing,
*  serial thread reads and writes.
*  GUI sets flag to indicate cancel.
*

*/

//#include "stdafx.h"
#include "../common/ui/winclass.h"

#include <afxres.h>
#include <shellapi.h>
#include <commdlg.h>

#include <limits.h>

#include "resource.h"


#pragma warning(disable : 4511 4512 4100 4127 4505)


typedef unsigned char byte;

#include "serial.h"

using namespace winclass;

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

#define CTRL(x)		        (x - 'A' + 1)

static const TCHAR org[] = _T("BridgePoint");
static const TCHAR app[] = _T("ReaderConsole");
static const TCHAR title[] = _T("Reader Configuration Console");


#define IDLE_TIMEOUT            100
#define CONSOLE_TIMEOUT         500

#define CMD_TIMEOUT		500
#define LINE_TIMEOUT		20

//=========================================================================
// Window routines.
//

/*
 * Gets location of window with respect to its parent's client area,
 * probably so we can move it with SetWindowPos().
 */
void 
GetWindowPos(HWND hwnd, RECT *rc)
{
  GetWindowRect(hwnd, rc);
  MapWindowPoints(NULL, GetParent(hwnd), (LPPOINT) rc, 2);
}


void
GetWindowSize(HWND hwnd, SIZE *size)
{
  RECT rc;
  
  GetWindowRect(hwnd, &rc);
  size->cx = rc.right - rc.left;
  size->cy = rc.bottom - rc.top;
}

/*
 * When a window is resized and we want to reposition its children.
 *
 * dwp - [in] DeferWindowPos handle, or NULL to move child right now.
 *
 * hwnd - [in] Window being resized.
 *
 * id - [in] ID of child window.
 *
 * dx, dy - [in] How much window has been resized.
 *
 * align - [in] Sides of control to move.  Mask.
 *      WVR_ALIGNLEFT - move left side of control by dx, don't resize.
 *      WVR_ALIGNRIGHT - resize right side of control by dx.
 *      WVR_ALIGNTOP - move top of control by dy, don't resize.
 *      WVR_ALIGNBOTTOM - resize bottom of control by dy.
 */
HDWP 
AlignDlgItem(HDWP dwp, HWND hwnd, UINT id, int dx, int dy, DWORD flags)
{
  HWND ctl = GetDlgItem(hwnd, id);

  RECT rc;
  GetWindowPos(ctl, &rc);
  
  SIZE size;
  size.cx = rc.right - rc.left;
  size.cy = rc.bottom - rc.top;
  
  if (flags & WVR_ALIGNLEFT) {
    rc.left += dx;
  }
  if (flags & WVR_ALIGNTOP) {
    rc.top += dy;
  }
  if (flags & WVR_ALIGNRIGHT) {
    size.cx += dx;
  }
  if (flags & WVR_ALIGNBOTTOM) {
    size.cy += dy;
  }
  if (dwp != NULL) {
    dwp = DeferWindowPos(dwp, ctl, NULL, rc.left, rc.top, size.cx, size.cy, SWP_NOZORDER);
  } else {
    MoveWindow(ctl, rc.left, rc.top, size.cx, size.cy, TRUE);
  }
  return dwp;
}

/*
 * Callback from OPENFILENAME dialog that centers it.
 */
static UINT CALLBACK 
OFNHookProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg == WM_INITDIALOG) {
    CenterWindow(GetParent(hwnd));
  }
  return 0;
}

struct TTY 
{
  SerialPort comm;
  BOOL echo;
  BOOL pause;
  int lineDelay;
};

//=========================================================================
// Main program
//

struct AboutDlg : public Dialog
{
  AboutDlg() : Dialog(IDD_ABOUT) {}
  
  virtual INT_PTR DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    switch (msg) {
      HANDLE_MSG(hwnd, WM_INITDIALOG, OnInitDialog);
    }
    return Dialog::DialogProc(hwnd, msg, wParam, lParam);
  }
  
  BOOL OnInitDialog(HWND hwnd, HWND hwndFocus, LPARAM lParam) 
  {
    SetVersionText(hwnd, IDC_NAME, NULL);
    SetVersionText(hwnd, IDC_VERSION, NULL);
    SetVersionText(hwnd, IDC_COMPANY, NULL);
    SetVersionText(hwnd, IDC_COPYRIGHT, NULL);
    
    return TRUE;
  }
};

struct SerialDlg : public Dialog
{
  TTY *_settings;

  SerialDlg(TTY *settings) : Dialog(IDD_SERIAL), _settings(settings) {}

  virtual INT_PTR DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    switch (msg) {
      HANDLE_MSG(hwnd, WM_INITDIALOG, OnInitDialog);
      HANDLE_MSG(hwnd, WM_COMMAND, OnCommand);
    }
    return 0;
  }

  BOOL OnInitDialog(HWND hwnd, HWND hwndFocus, LPARAM lParam)
  {
    SetDlgItemText(hwnd, IDC_BAUD, _T("115200"));
    SetDlgItemText(hwnd, IDC_FLOW, _T("XON/XOFF"));
    SetDlgItemInt(hwnd, IDC_LINEDELAY, _settings->lineDelay, FALSE);

    return TRUE;
  }

  void OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify) 
  {
    if (id == IDOK) {
      BOOL good;
      int delay = GetDlgItemInt(hwnd, IDC_LINEDELAY, &good, FALSE);
      if (!good || (delay < 0) || (delay > 100)) {
        SetFocus(GetDlgItem(hwnd, IDC_LINEDELAY));
        Edit_SetSel(GetDlgItem(hwnd, IDC_LINEDELAY), 0, -1);
        return;
      }
      _settings->lineDelay = delay;
      EndDialog(hwnd, IDOK);
    }
    if (id == IDCANCEL) {
      EndDialog(hwnd, IDCANCEL);
    }
  }
};

#define MAXOUT          200000

struct OutputWindow : public Window
{
  TTY &_tty;

  OutputWindow(TTY &tty) : _tty(tty) {}

  void ShowOutput(char *src, int len)
  {
    int i;
    
    if (_tty.pause) { return; }
    
    // Emitting characters one-by-one was taking forever in edit control.
    //

    if (len < 0) { len = lstrlenA(src); }
    
    DWORD start, end;
    Edit_GetSel(_hwnd, &start, &end);
    if (start == end) {
      // Move cursor to end
      Edit_SetSel(_hwnd, INT_MAX, INT_MAX);
    }
    
    while (len > 0) {
      byte ch = 0;
      
      // Find a character that needs to be handled specially.
      //
      for (i = 0; i < len; i++) {
        ch = src[i];
        if (ch == '\r' && src[i + 1] == '\n') {
          i++;
          continue;
        } 
        if (ch == '\t' || ch >= 32) {
          continue;
        }
        break;
      }
      
      // Emit all characters before the special character.
      //
      if (i > 0) {
        char old = src[i];
        src[i] = 0;
        Edit_ReplaceSelA(_hwnd, src);
        src[i] = old;
      }
      
      // Emit the special character.
      //
      if (i < len) {
        if (ch == '\x07') {
          MessageBeep(0);
        } else if (ch == '\x08') {
          Edit_GetSel(_hwnd, &start, &end);
          start--;
          Edit_SetSel(_hwnd, start, end);
          Edit_ReplaceSelA(_hwnd, "");
        } else if (ch == '\r') {
          ;
        } else if (ch == '\n') {
          Edit_ReplaceSelA(_hwnd, "\r\n");
        } else {
          char tmp[10];
          
          StringCchPrintfA(tmp, _countof(tmp), "^%c", ch + 64);
          Edit_ReplaceSelA(_hwnd, tmp);
        }
        i++;
      }
      
      src += i;
      len -= i; 
    }

    if (GetWindowTextLengthW(_hwnd) > MAXOUT) {
      Edit_SetSel(_hwnd, 0, MAXOUT / 10);
      Edit_ReplaceSelA(_hwnd, "");
      Edit_SetSel(_hwnd, INT_MAX, INT_MAX);
    }
    
    
    Edit_ScrollCaret(_hwnd);
  }
  
  LRESULT WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    switch (msg) {
      HANDLE_MSG(hwnd, WM_KEYDOWN, OnKeyDown);
      HANDLE_MSG(hwnd, WM_CHAR, OnChar);
      HANDLE_MSG(hwnd, WM_PASTE, OnPaste);
      HANDLE_MSG(hwnd, WM_SETFOCUS, OnSetFocus);
    }
    return DefWindowProc(msg, wParam, lParam);
  }
  
  // SetFocus() doesn't work across threads, so message was posted here
  // when reader thread wanted to give focus to this window.
  //
  void OnSetFocus(HWND hwnd, HWND hwndOldFocus) 
  {
    if (hwndOldFocus == NULL) { SetFocus(hwnd); }
    DefWindowProc();
  }
  
  void OnKeyDown(HWND hwnd, UINT vk, BOOL fDown, int cRepeat, UINT flags) 
  {
    // In Edit control, 
    // 1. ESC normally sends WM_CLOSE to parent.  Suppress.
    // 2. TAB normally sends WM_NEXTDLGCTL to parent.  Change it so
    // we keep the tab, but Ctrl+Tab sends WM_NEXTDLGCTL.
    //
    if (vk == VK_ESCAPE) { return; }
    if (vk == VK_TAB) { 
      if (GetKeyState(VK_CONTROL) & 0x8000) {
        SendMessage(GetParent(hwnd), WM_NEXTDLGCTL, GetKeyState(VK_SHIFT) & 0x8000, 0);
      }
      return;
    }
    
    DefWindowProc();
  }
  
  void OnChar(HWND hwnd, TCHAR ch, int cRepeat)
  {
    char c = (char) ch;
    
    if (ch == CTRL('P')) {
      PostMessage(GetParent(hwnd), WM_COMMAND, ID_TOOLS_PLAYLASTMACRO, NULL);
      return;
    }
    if (ch == CTRL('S') || ch == CTRL('Q')) { return; }
    
    if (ch == CTRL('C') || ch == CTRL('X') || ch == CTRL('V')) { 
      DefWindowProc(); 
      return;
    }
    if (ch == CTRL('A')) {
      Edit_SetSel(hwnd, 0, -1);
      return;
    }
    if (_tty.echo) { 
      if (ch == '\t') { 
        // Manually insert Tab here, since an edit embedded in a 
        // dialog normally ignores WM_CHAR(Tab).
        
        Edit_ReplaceSel(hwnd, _T("\t"));
      }
      DefWindowProc(); 
    }
    _tty.comm.Write(&c, 1);
  }
  
  void OnPaste(HWND hwnd)
  {
    Edit_SetSel(hwnd, (UINT) -2, (UINT) -2);
    if (_tty.echo) {
      DefWindowProc();
    }
    
    OpenClipboard(hwnd);
    HGLOBAL h = GetClipboardData(CF_TEXT);
    if (h != NULL) {
      char *src = (char *) GlobalLock(h);
      _tty.comm.Write(src, lstrlenA(src));
      GlobalUnlock(h);
    }
    CloseClipboard();
  }
};


#define MAXCOM		256

static void 
FillPortNames(HWND hwnd)
{
  TCHAR old[10], tmp[10];
  int i;
  
  ComboBox_GetText(hwnd, old, _countof(old));
  SetWindowRedraw(hwnd, FALSE);
  ComboBox_ResetContent(hwnd);
  
  for (i = 1; i <= MAXCOM; i++) {
    sprintf_t(tmp, _countof(tmp), _T("COM%d"), i);
    QueryDosDevice(tmp, NULL, 0);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
      StringCchCat(tmp, _countof(tmp), _T(":"));
      ComboBox_AddString(hwnd, tmp);
    }
  } 
  if (ComboBox_GetCount(hwnd) == 0) {
    ComboBox_AddString(hwnd, _T("COM1:"));
  }
  SetWindowRedraw(hwnd, TRUE);
  ComboBox_SelectString(hwnd, -1, old);
}

enum { QUITTING, IDLE, CONNECT, CONSOLE, DETECT, REFLASH, PLAYMACRO, RECORDMACRO };

struct ReflashDlg : public Dialog
{
  Control ctlPortName;  // Serial port name.
  Control ctlFileName;  // Reflash file name.
  Control ctlCancel;    // Cancel button to cancel current operation.
  Control ctlStatus;    // Status of current operation.
  
  OutputWindow ctlOutput;

  TCHAR _macroName[MAX_PATH];  

  SIZE _minSize;	/* Main window min size. */
  SIZE _curSize;	/* Main window current size, for aligning controls. */
  
  BOOL _statusErr;	/* Show status as error? */
  
  HANDLE _thread;       // I/O worker thread.
  int _threadState;    
  BOOL _stop;

  TTY _tty;
  
  ReflashDlg() : Dialog(IDD_REFLASH), ctlOutput(_tty)
  {
    _tty.echo = FALSE;
    _tty.pause = FALSE;

    _macroName[0] = '\0';

    _statusErr = 0;
    
    _thread = NULL;
  }
  
  ~ReflashDlg() 
  {
    WaitForSingleObject(_thread, INFINITE);
    CloseHandle(_thread);
  }

  /*
   * Update controls based on the current state.
   */ 
  void UpdateControls(void)
  {
    BOOL ctlEnable;
    int mnuEnable;

    HMENU menu = GetMenu(_hwnd);

    if (ComboBox_GetCurSel(ctlPortName) < 0) {
      ComboBox_SetCurSel(ctlPortName, 0);
    }
    
    // Update state of controls.
    //
    CheckDlgButton(_hwnd, ID_ECHO, (_tty.echo ? BST_CHECKED : BST_UNCHECKED));
    CheckMenuItem(menu, ID_ECHO, (_tty.echo ? MF_CHECKED : MF_UNCHECKED));

    CheckDlgButton(_hwnd, ID_PAUSE, (_tty.pause ? BST_CHECKED : BST_UNCHECKED));
    CheckMenuItem(menu, ID_PAUSE, (_tty.pause ? MF_CHECKED : MF_UNCHECKED));

    // Enable/disable controls based on whether connected to serial port.
    //
    ctlEnable = TRUE;
    mnuEnable = MF_ENABLED;
    if (_tty.comm == NULL) {
      ctlEnable = FALSE;
      mnuEnable = MF_GRAYED | MF_DISABLED;
    }

    EnableWindow(ctlOutput, ctlEnable);
    EnableDlgItem(_hwnd, IDC_REFLASH, ctlEnable);
    EnableMenuItem(menu, ID_FILE_TRANSFER, mnuEnable);
  }


  

  void SaveSettings()
  {
    Settings s(org, app);
    TCHAR buf[MAX_PATH];

    s.WriteInt(_T("width"), _curSize.cx);
    s.WriteInt(_T("height"), _curSize.cy);
    
    GetWindowText(ctlPortName, buf, sizeof(buf));
    s.WriteString(_T("port"), buf);
    
    GetWindowText(ctlFileName, buf, _countof(buf));
    s.WriteString(_T("file"), buf);

    s.WriteString(_T("macro"), _macroName);
    
    s.WriteInt(_T("echo"), _tty.echo);
    s.WriteInt(_T("line"), _tty.lineDelay);
  }
  
  void RestoreSettings()
  {
    Settings s(org, app);
    TCHAR buf[MAX_PATH];
    SIZE size;

    size.cx = s.GetInt(_T("width"), _curSize.cx);
    size.cy = s.GetInt(_T("height"), _curSize.cy);
    SetWindowPos(_hwnd, NULL, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow(_hwnd);
    
    s.GetString(_T("port"), NULL, buf, _countof(buf));
    ComboBox_SelectString(ctlPortName, -1, buf);
    
    s.GetString(_T("file"), NULL, buf, _countof(buf));
    DisplayFileName(buf);

    s.GetString(_T("macro"), NULL, _macroName, _countof(_macroName));
    
    _tty.echo = s.GetInt(_T("echo"), 0);
    _tty.lineDelay = s.GetInt(_T("line"), LINE_TIMEOUT);
  }
  
  /*
   * Display selected file name.
   */
  void DisplayFileName(const TCHAR *buf)
  {
    Edit_SetText(ctlFileName, buf);
    Edit_SetSel(ctlFileName, INT_MAX, INT_MAX);
  }
  
  // SetFocus() doesn't work across threads, so post message
  // telling output window to focus on itself.
  void SetFocus(HWND hwnd)
  {
    ::SetFocus(hwnd);
    if (hwnd == ctlOutput) { PostMessage(hwnd, WM_SETFOCUS, 0, 0); }
  }
  
  
  virtual INT_PTR DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    switch (msg) {
      HANDLE_MSG(hwnd, WM_INITDIALOG, OnInitDialog);
      HANDLE_MSG(hwnd, WM_GETMINMAXINFO, OnGetMinMaxInfo);
      HANDLE_MSG(hwnd, WM_SIZE, OnSize);
      
      HANDLE_MSG(hwnd, WM_CTLCOLOREDIT, OnCtlColor);
      HANDLE_MSG(hwnd, WM_CTLCOLORSTATIC, OnCtlColor);

      HANDLE_MSG(hwnd, WM_HELP, OnHelp);
      HANDLE_MSG(hwnd, WM_CLOSE, OnClose);
      HANDLE_MSG(hwnd, WM_COMMAND, OnCommand);

//      HANDLE_MSG(hwnd, WM_DEVICECHANGE, OnDeviceChange);
    }
    return FALSE;
  }

/*  BOOL OnDeviceChange(HWND hwnd, UINT uEvent, DWORD dwEventData)
  {
    Debug(_T("\nWM_DEVICECHANGE: %d"), uEvent);
    return FALSE;
  }*/
  
  BOOL OnInitDialog(HWND hwnd, HWND hwndFocus, LPARAM lParam) 
  {
    ctlPortName.Attach(hwnd, IDC_PORT);
    ctlFileName.Attach(hwnd, IDC_FILENAME);

    ctlCancel.Attach(hwnd, IDC_CANCEL);
    ctlStatus.Attach(hwnd, IDC_STATUS);
    
    ctlOutput.Attach(hwnd, IDC_OUTPUT);
    Edit_LimitText(ctlOutput, 0);
   
    SetWindowIcon(hwnd, ICON_BIG, 
        LoadIcon(GetResourceHandle(), IDI_REFLASH, ICON_BIG));

    // Get system Open Folder icon for choose file button.
    SHFILEINFO sfi = {0};
    SHGetFileInfo(_T("folder"), FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
        SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES | SHGFI_OPENICON);
    SendDlgItemMessage(hwnd, IDC_BROWSE, BM_SETIMAGE, IMAGE_ICON, (LPARAM) sfi.hIcon);

    SetWindowFont(ctlOutput, GetStockObject(OEM_FIXED_FONT), TRUE);

    // Minimum size for main window is its size in the dialog resource.
    GetWindowSize(hwnd, &_minSize);
    _curSize = _minSize;
    
    ShowWindow(ctlCancel, SW_HIDE);
    
    FillPortNames(ctlPortName);   
    
    // Restore window to how it was last time program ran.
    RestoreSettings();
    UpdateControls();
    
    StartReaderThread();
    SetFocus(ctlOutput);
    
    return FALSE;
  }
  
  void OnGetMinMaxInfo(HWND hwnd, LPMINMAXINFO lpMinMaxInfo) 
  {
    lpMinMaxInfo->ptMinTrackSize.x = _minSize.cx;
    lpMinMaxInfo->ptMinTrackSize.y = _minSize.cy;
  }
  
  void OnSize(HWND hwnd, UINT state, int cx, int cy) 
  {
    SIZE size;
    int dx, dy;

    if (state == SIZE_MINIMIZED) { return; }

    GetWindowSize(hwnd, &size);
    dx = size.cx - _curSize.cx;  
    dy = size.cy - _curSize.cy;
    _curSize = size;
  
    HDWP dwp = NULL; 

    AlignDlgItem(dwp, hwnd, IDC_LINE1, dx, dy, WVR_ALIGNRIGHT);
    AlignDlgItem(dwp, hwnd, IDC_LINE2, dx, dy, WVR_ALIGNRIGHT);
    AlignDlgItem(dwp, hwnd, IDC_FILENAME, dx, dy, WVR_ALIGNRIGHT);
    AlignDlgItem(dwp, hwnd, IDC_BROWSE, dx, dy, WVR_ALIGNLEFT);

    AlignDlgItem(dwp, hwnd, IDC_STATUS, dx, dy, WVR_ALIGNRIGHT);

    AlignDlgItem(dwp, hwnd, IDC_OUTPUT, dx, dy, WVR_ALIGNBOTTOM | WVR_ALIGNRIGHT);
  }
  
  HBRUSH OnCtlColor(HWND hwnd, HDC hdc, HWND hwndChild, int type)
  {
    if (_statusErr && (hwndChild == ctlStatus)) {
      SetTextColor(hdc, RGB(0xff, 0, 0));
      SetBkColor(hdc, RGB(0, 0, 0));
      return GetStockBrush(BLACK_BRUSH);;
    }
    return NULL;
  }	
    
  void OnHelp(HWND hwnd, LPHELPINFO lphi) 
  {
    AboutDlg().DoModal(hwnd);
  }
  
  void OnClose(HWND hwnd)
  {
    SetState(QUITTING);
    SaveSettings();
    EndDialog(hwnd, 0);
  }
  
  void OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify) 
  {
    switch (id) {

    // Menu commands.

    case ID_CLEAR:
      Edit_SetText(ctlOutput, NULL);
      _tty.pause = FALSE;
      UpdateControls();
      SetFocus(ctlOutput);
      break;
      
    case ID_FILE_SAVE:
      Cmd_Save();
      break;

    case ID_FILE_TRANSFER:
      if (Cmd_ChooseFile()) { Cmd_Reflash(); }
      break;
      
    case ID_FILE_EXIT:
      PostMessage(hwnd, WM_CLOSE, 0, 0);
      break;
      
    case ID_EDIT_CUT:
      SendMessage(GetFocus(), WM_CUT, 0, 0);
      break;
      
    case ID_EDIT_COPY:
      SendMessage(GetFocus(), WM_COPY, 0, 0);
      break;
      
    case ID_EDIT_PASTE:
      SendMessage(GetFocus(), WM_PASTE, 0, 0);
      break;
      
    case ID_EDIT_CLEAR:
      SendMessage(GetFocus(), WM_CLEAR, 0, 0);
      break;
      
    case ID_EDIT_SELECT_ALL:
      Edit_SetSel(GetFocus(), 0, -1);
      break;
      
    case ID_ECHO:
      _tty.echo = !_tty.echo;
      UpdateControls();
      SetFocus(ctlOutput);
      break;
      
    case ID_PAUSE:
      _tty.pause = !_tty.pause;
      UpdateControls();
      SetFocus(ctlOutput);
      break;

    case ID_SETTINGS_SERIAL:
      Cmd_SerialSettings();
      break;
      
    case ID_TOOLS_PLAYMACRO:
      Cmd_PlayMacro(FALSE);
      break;

    case ID_TOOLS_PLAYLASTMACRO:
      Cmd_PlayMacro(TRUE);
      break;
      
    case ID_HELP_ABOUT: 
      OnHelp(hwnd, NULL);
      break;

    // Control commands.
      
    case IDC_CONNECT:
      Cmd_Connect();
      break;
      
    case IDC_PORT:
      Cmd_ChoosePort(codeNotify);
      break;
      
    case IDC_REFLASH:
      Cmd_Reflash();
      break;
      
    case IDC_BROWSE:
      Cmd_ChooseFile();
      break;
      
    case IDC_CANCEL:
      SetState(CONSOLE);
      SetFocus(ctlOutput);
      break;

    }
    
   }
    
      
  void Cmd_Connect()
  {
    EnableDlgItem(hwnd, IDC_CONNECT, FALSE);
    SetFocus(ctlOutput);
    SetState(DETECT);
  }    

  void Cmd_ChoosePort(int codeNotify)
  {
    if (codeNotify == CBN_DROPDOWN) {
      // Don't try to reconnect while still selecting port.
      SetState(IDLE);
      FillPortNames(ctlPortName);
    } else if (codeNotify == CBN_CLOSEUP) {
      Status(_T("Trying..."), 0);
      SetFocus(ctlOutput);
      SetState(CONNECT);
    }
  }

  void EnableUI(BOOL enable)
  {
    HMENU menu = GetMenu(_hwnd);
    int mf, show, cancel;

    if (enable == FALSE) {
      _tty.pause = FALSE;
      UpdateControls();
      SetFocus(ctlOutput);
    
      mf = MF_BYCOMMAND | MF_GRAYED;
      show = SW_HIDE;
      cancel = SW_SHOW;
    } else {
      mf = MF_BYCOMMAND | MF_ENABLED;
      show = SW_SHOW;
      cancel = SW_HIDE;
    }

    // Disable UI while running a backgound task.
    EnableDlgItem(hwnd, IDC_CONNECT, enable);
    EnableDlgItem(hwnd, IDC_PORT, enable);
    EnableDlgItem(hwnd, IDC_REFLASH, enable);

    EnableDlgItem(hwnd, ID_ECHO, enable);
    EnableDlgItem(hwnd, ID_PAUSE, enable);
    EnableDlgItem(hwnd, ID_CLEAR, enable);

    EnableMenuItem(menu, ID_FILE_TRANSFER, mf);
//    EnableMenuItem(_menu, ID_TOOLS_RECORDMACRO, mf);
    EnableMenuItem(menu, ID_TOOLS_PLAYMACRO, mf);
    EnableMenuItem(menu, ID_TOOLS_PLAYLASTMACRO, mf);
    EnableMenuItem(menu, ID_ECHO, mf);
    EnableMenuItem(menu, ID_PAUSE, mf);
    EnableMenuItem(menu, ID_CLEAR, mf);

    ShowWindow(GetDlgItem(hwnd, IDC_STATUSLABEL), show);

    ShowWindow(ctlCancel, cancel);
  }

  void Cmd_Reflash()
  {
    EnableUI(FALSE);
    SetState(REFLASH);
  }

  BOOL Cmd_ChooseFile()
  {
    BOOL open;
    TCHAR buf[MAX_PATH];
    TCHAR dir[MAX_PATH];
    OPENFILENAME ofn = {0};
    
    Edit_GetText(ctlFileName, buf, _countof(buf));

    
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = _hwnd;
    ofn.lpstrFilter = _T("S-Record file (*.s19)\0*.s19\0All Files (*.*)\0*.*\0");
    if (PathIsDirectory(buf)) {
      strcpy_t(dir, MAX_PATH, buf);
      ofn.lpstrInitialDir = dir;
      buf[0] = '\0';
    }
    ofn.lpstrFile = buf;
    ofn.nMaxFile = _countof(buf);
    ofn.lpstrTitle = _T("Select File to Reflash Reader");
    ofn.lpfnHook = OFNHookProc;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ENABLEHOOK | OFN_EXPLORER;
    
    open = GetOpenFileName(&ofn);
    if (open != FALSE) {
      DisplayFileName(buf);
      Status(NULL, 0);
    }
 
    
    SetFocus(ctlOutput);
    return open;
  }
  
  void Cmd_PlayMacro(BOOL last)
  {
    if (last == FALSE) {
      OPENFILENAME ofn = {0};

      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hwnd;
      ofn.lpstrFilter = _T("Text file (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
      ofn.lpstrFile = _macroName;
      ofn.nMaxFile = _countof(_macroName);
      ofn.lpstrTitle = _T("Select Reader Command File");
      ofn.lpfnHook = OFNHookProc;
      ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ENABLEHOOK | OFN_EXPLORER;

      if (GetOpenFileName(&ofn) == FALSE) { return; }
    }
    EnableUI(FALSE);
    SetState(PLAYMACRO);
  }
    
  BOOL Cmd_Save()
  {
    TCHAR buf[MAX_PATH];
    OPENFILENAME ofn = {0};
    
    buf[0] = 0;
    
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = _hwnd;
    ofn.lpstrFilter = _T("Text file (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
    ofn.lpstrFile = buf;
    ofn.nMaxFile = _countof(buf);
    ofn.lpstrTitle = _T("Save Console Log");
    ofn.lpstrDefExt = _T("txt");
    ofn.lpfnHook = OFNHookProc;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_ENABLEHOOK | OFN_EXPLORER;
    
    BOOL save = GetSaveFileName(&ofn);
    if (save) {
      HANDLE out = CreateFile(buf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
      if (out != INVALID_HANDLE_VALUE) {
        DWORD l = GetWindowTextLengthA(ctlOutput);
        char *p = (char *) LocalAlloc(GPTR, l + 1);
        if (p != NULL) {
          GetWindowTextA(ctlOutput, p, l + 1);
          WriteFile(out, p, l, &l, NULL);
          LocalFree(p);
        }
        CloseHandle(out);
      }
    }
    
    SetFocus(ctlOutput);
    return save;
  }

  void Cmd_SerialSettings()
  {
    SerialDlg(&_tty).DoModal(hwnd);
  }

  void Status(const TCHAR *msg, BOOL err = TRUE)
  {
    _statusErr = err;
    SetWindowText(ctlStatus, msg);
  }

  /*
   * Sets the state of the background reader thread.
   * The reader thread will asynchronously check the state variable and
   * cleanly move into the desired new state.
   */
  void SetState(int state)
  {
    if (state == CONSOLE) { EnableUI(TRUE); }

    _threadState = state;
    _stop = TRUE;
  }
  
 
  
  void StartReaderThread()
  {
    _threadState = CONNECT;
    _thread = CreateThread(NULL, 0, ReaderThread, this, 0, NULL);
  }
  
  static DWORD CALLBACK ReaderThread(LPVOID param) 
  {
    ((ReflashDlg *) param)->ReaderThread();
    return 0;
  }

  void ReaderThread()
  {
    while (1) {
      int state = _threadState;
      _stop = FALSE;
      
      if (state == IDLE) {
        Sleep(IDLE_TIMEOUT);
      } else if (state == CONNECT) {
        Connect();
      } else if (state == CONSOLE) { 
        Console(); 
      } else if (state == DETECT) { 
        ScanForReader(); 
      } else if (state == REFLASH) { 
        Reflash(); 
      } else if (state == PLAYMACRO) {
        PlayMacro();
      } else {
        break;
      }
    }
  }

  void Connect()
  {
    Sleep(IDLE_TIMEOUT);
    if (Port_Connect()) { 
      Status(_T("OK"), 0); 
      SetState(CONSOLE); 
    }
  }
  
  
  void Console()
  {
    char buf[1000];

    _tty.comm.SetTimeout(CONSOLE_TIMEOUT, -1);
    while (_stop == FALSE) {
      int len = _tty.comm.Read(buf, sizeof(buf) - 1);
      
      if (len == 0) {
        if (_tty.comm.Error()
            || (   (GetFileAttributes(_tty.comm.Name()) == -1)
                && (GetLastError() == ERROR_FILE_NOT_FOUND))) {
          // usbser.sys: HANDLE is still valid but port name is gone.
          len = -1;
        }
      }
      
      if (len < 0) { 
        SetState(CONNECT);
        break;
      }
      
      if (len > 0) { ctlOutput.ShowOutput(buf, len); }
    }
  }
  
  void ScanForReader()
  {
    TCHAR first[16], name[16];
    int i, len;
    
    GetWindowText(ctlPortName, first, _countof(first));
    if (ConnectReader(first)) { goto end; }
    
    FillPortNames(ctlPortName);
    
    len = ComboBox_GetCount(ctlPortName);
    for (i = 0; i < len; i++) {
      ComboBox_SetCurSel(ctlPortName, i);
      GetWindowText(ctlPortName, name, _countof(name));
      if (lstrcmpi(first, name) == 0) { continue; }
      if (ConnectReader(name)) { goto end; }
    }
    
    if (GetLastError() != ERROR_ACCESS_DENIED) {
      Status(_T("No reader found"));
    }
    
    ComboBox_SelectString(ctlPortName, -1, first);
    Port_Connect();
    
end:
    SetState(CONSOLE);
    EnableDlgItem(_hwnd, IDC_CONNECT, TRUE);
  }
  
  BOOL ConnectReader(const TCHAR *name)
  {
    int i;
    TCHAR buf[MAX_PATH];
    
    StringCchPrintf(buf, _countof(buf), _T("Checking %s"), name);
    Status(buf, 0);
    
    if (Port_Connect() == FALSE) { return FALSE; }
    
    for (i = 0; i < 3; i++) {
      Port_Send("\r");
      if (Port_Expect("Boot>")) { goto found; }
      Port_Send("\r");
      if (Port_Expect("CMD>")) { goto found; }
  
    }
    return FALSE;
    
found:
    Status(_T("Connected to Reader"), 0);
    SetFocus(ctlOutput);
    return TRUE;
  }
  
  void Reflash()
  {
    FILE *file = NULL;
    TCHAR fileName[MAX_PATH];
    char tmp[128];
    int i;
    TCHAR *msg = NULL;
    
    Status(_T("Connecting to reader..."), 0);
    
    if (GetWindowText(ctlFileName, fileName, _countof(fileName)) == 0) { 
badfile:
      msg = _T("Cannot open file");
      goto end;
    }
    
    
    file = _tfopen(fileName, _T("r"));
    if (file == NULL) { goto badfile; }
    
    Port_Send("\r\r\r");
    if (Port_Expect("CMD>")) {
      // Try RF command.
      
      Port_Send("RF\r");
      if (Port_Expect("Send File") == FALSE) { goto try908; }
      Port_Expect(">");
      
      if (SendFile(file) == FALSE) {
        msg = _T("Error sending file");
        goto end;
      }
    } else {
      // Try 908
      
try908:
      memset(tmp, 27, sizeof(tmp));
      tmp[sizeof(tmp) - 1] = '\0';
    
      Port_Send(tmp);
      Port_Expect("Boot>");
    
      Port_Send("\r\n");
    
      if (Port_Expect("Boot>") == FALSE) {
        msg = _T("Reader didn't accept reflash command");
        goto end;
      }
    
      Port_Send("w");
      Port_Expect("Boot>");
      Port_Send("w");
      if (Port_Expect("Boot>") == FALSE) { 
        msg = _T("Could not erase reader");
        goto end;
      }
      Port_Send("p");
      if (Port_Expect("...") == FALSE) {
        msg = _T("Reader didn't accept program command");
        goto end;
      }
      Edit_ReplaceSel(ctlOutput, _T(" downloading ..."));
      if (SendFile(file) == FALSE) {
        msg = _T("Error sending file");
        goto end;
      } 
      Port_Expect("Boot>");
      Port_Send("x");
    }
    
    for (i = 0; i < 5; i++) {
      if (Port_Expect("CMD>")) { break; }
    }
    
    Status(_T("Reflash Complete"), 0);
    
end:
    if (_stop) { msg = _T("Reflash Cancelled"); }
    
    if (file != NULL) { fclose(file); }
    if (msg != NULL) { Status(msg); }

    SetState(CONSOLE);
  }
  
  BOOL SendFile(FILE *src)
  {
    int i, total, read;
    char line[1000];
    TCHAR tmp[32];
    
    _tty.comm.SetTimeout(_tty.lineDelay, -1);
    
    for (i = 0; fgets(line, sizeof(line) - 3, src) != NULL; i++) {
      // counting lines.
    }
    total = i;
    fseek(src, 0, SEEK_SET);
    
    for (i = 1; fgets(line, sizeof(line) - 3, src) != NULL; i++) {
      if (_stop) { goto err; }
      
      sprintf_t(tmp, _countof(tmp), _T("line %d/%d"), i, total);
      Status(tmp, 0);
      
      StrTrimA(line, "\r\n\t ");
      strcat_tA(line, _countof(line), "\r\n");
     
      if (Port_Send(line) == FALSE) { goto err; }
      
      // Line delay + get whatever feedback from reader.
      //
      while (_stop == FALSE) {
        read = _tty.comm.Read(line, sizeof(line) - 1);
        if (read == 0) { break; }
        if (read < 0) { goto err; }
        
        ctlOutput.ShowOutput(line, read); 
        if (StrStrA(line, "?") != NULL) { goto err; }
      }
    }
    
    return TRUE;
    
err:
    Port_Send("\r\n\r\n");
    return FALSE;
  }

#define MACRO_HEADER "# Reader Config macro file"

  void PlayMacro()
  {
    TCHAR *msg = NULL;
    FILE *f = NULL;
    char line[1000];
    
    Status(_T("Playing command file..."), 0);

    if (_macroName[0] == '\0') {
      msg = _T("No command file");
      goto end;
    }
    f = _tfopen(_macroName, _T("r"));
    if (f == NULL) { 
      msg = _T("Cannot open command file");
      goto end;
    }

/*    line[0] = '\0';
    fgets(line, sizeof(line), f);
    StrTrimA(line, "\r\n\t ");
    if (strcmp(line, MACRO_HEADER) != 0) {
      TCHAR *tmp = TmpStr();
      PathCanonicalize(tmp, _macroName);
      PathStripPath(tmp);
      strcat_t(tmp, TMPSTR_MAX, _T(": not a macro file"));
      msg = tmp;
      goto end;
    } */

    Port_Send("\r\r\r");
    if (Port_Expect("CMD>") == FALSE) {
      msg = _T("No Reader detected");
      goto end;
    }

    while (fgets(line, sizeof(line) - 2, f) != NULL) {
      StrTrimA(line, "\r\n\t ");
      strcat_tA(line, _countof(line), "\r");
      if (line[0] == '#') { 
          ctlOutput.ShowOutput(line, -1);
          strcpy_tA(line, _countof(line), "\nCMD>");
          ctlOutput.ShowOutput(line, -1);
          continue;
      }
      Port_Send(line);
      if (Port_Expect("CMD>") == FALSE) {
        msg = _T("Command file cancelled");
        goto end;
      }
    }

    Status(_T("Command file done"), 0);

end:
    if (f != NULL) { fclose(f); }
    if (msg != NULL) { Status(msg); }
    SetState(CONSOLE);
  }
  

// These should only be called from within the thread.

 /*
  * Open serial port specified by the dialog.
  */
  BOOL Port_Connect()
  {
    TCHAR name[16];
    TCHAR tmp[MAX_PATH];
    
    _tty.comm.Close();
    Sleep(500);
    
    sprintf_t(tmp, _countof(tmp), _T("%s: disconnected"), title);
    SetWindowText(_hwnd, tmp);

    ComboBox_GetText(ctlPortName, name, _countof(name));
    if (_tty.comm.Open(name) == FALSE) {
      DWORD err = GetLastError();
      if (err == ERROR_ACCESS_DENIED) {
        sprintf_t(tmp, _countof(tmp), _T("Another program is using %s"), name);
      } else if (err == ERROR_FILE_NOT_FOUND) {
        sprintf_t(tmp, _countof(tmp), _T("USB Port %s disconnected"), name);
      } else {
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0, tmp, _countof(tmp), NULL);
      }
      Status(tmp);
      UpdateControls();
      return FALSE;
    }

    sprintf_t(tmp, _countof(tmp), _T("%s: %s"), title, name);
    SetWindowText(_hwnd, tmp);
    
    SetupComm(_tty.comm, 8192, 2048);
    
    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(_tty.comm, &dcb);
    
    dcb.ByteSize = 8;
    dcb.BaudRate = 115200;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    
    dcb.fParity = TRUE;
    dcb.fBinary = TRUE;
    
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    

    // Flow control XON/XOFF
    dcb.fOutX = TRUE;
    dcb.fInX = TRUE;
    dcb.XonLim = 0;
    dcb.XoffLim = 0;
    dcb.XonChar = CTRL('Q');
    dcb.XoffChar = CTRL('S');
    
    SetCommState(_tty.comm, &dcb);
    EscapeCommFunction(_tty.comm, SETDTR);
    EscapeCommFunction(_tty.comm, SETRTS);
    
    UpdateControls();
    SetFocus(ctlOutput);
    return TRUE;
  }
  
  BOOL Port_Send(const char *buf, int len = -1)
  {
    if (_stop) { return FALSE; }
    if (len < 0) { len = lstrlenA(buf); }
    
    return _tty.comm.Write(buf, len);
  }
  
#define MAX_EXPECT			1000
  
  /*
  * Read up to MAX_EXPECT characters, waiting for:
  * 1. the expected pattern to appear.
  * 2. a timeout.
  */
  BOOL Port_Expect(const char *pat)
  {
    char tmp[MAX_EXPECT];
    size_t len;
    int read;

    _tty.comm.SetTimeout(CMD_TIMEOUT, CMD_TIMEOUT);
    
    for (len = 0; len < _countof(tmp) - 1; ) {
      read = _tty.comm.Read(tmp + len, _countof(tmp) - 1 - len);
      if (_stop) { break; }
      if (read <= 0) { break; }
      ctlOutput.ShowOutput(tmp + len, read);
      
      len += read;
      if (StrStrA(tmp, pat) != NULL) { return TRUE; }
    }
    return FALSE;
  }
  
};

int APIENTRY 
WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
  return ReflashDlg().DoModal(NULL);
}















