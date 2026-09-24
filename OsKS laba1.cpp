#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <atomic>
#include <vector>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

#define IDC_COMBO_PORT      101
#define IDC_COMBO_STOPBITS  102
#define IDC_EDIT_INPUT      103
#define IDC_EDIT_OUTPUT     104
#define IDC_STATIC_STATUS   105

#define IDT_STATUS_TIMER    201

// Дескрипторы 4 отдельных окон
HWND hWndControl = NULL;
HWND hWndStatus = NULL;
HWND hWndInput = NULL;
HWND hWndOutput = NULL;

// Элементы управления внутри окон
HWND hComboPort = NULL;
HWND hComboStopBits = NULL;
HWND hEditInput = NULL;
HWND hEditOutput = NULL;
HWND hStaticStatus = NULL;

WNDPROC origEditProc = NULL;
HANDLE hSerial = INVALID_HANDLE_VALUE;
HANDLE hReadThread = NULL;

std::atomic<bool> g_bRunning(false);
std::atomic<unsigned long long> g_txCount(0);
bool g_portLocked = false;

// Прототипы функций
bool OpenAndConfigureSerial(int portNumber, BYTE stopBits);
void CloseSerial();
DWORD WINAPI SerialReadThread(LPVOID lpParam);
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void AppendTextToOutput(const char* data, DWORD len);
BYTE GetSelectedStopBits();
std::vector<int> GetAvailableComPorts();

// ============================================================================
// ПОИСК ДОСТУПНЫХ COM-ПОРТОВ В СИСТЕМЕ
// ============================================================================

std::vector<int> GetAvailableComPorts() {
    std::vector<int> ports;
    wchar_t targetPath[256];

    // 1. Опрос системных DOS-устройств COM1..COM256 (находит com0com, VSPE и аппаратные порты)
    for (int i = 1; i <= 256; ++i) {
        std::wstring portName = L"COM" + std::to_wstring(i);
        if (QueryDosDeviceW(portName.c_str(), targetPath, 256) != 0) {
            ports.push_back(i);
        }
    }

    // 2. Проверка реестра Windows (на случай USB-to-COM переходников FTDI, CH340, CP2102)
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t valueName[256];
        BYTE data[256];
        DWORD index = 0;
        DWORD valNameLen = 256;
        DWORD dataSize = 256;
        DWORD type = 0;

        while (RegEnumValueW(hKey, index++, valueName, &valNameLen, NULL, &type, data, &dataSize) == ERROR_SUCCESS) {
            if (type == REG_SZ) {
                std::wstring portStr((wchar_t*)data);
                if (portStr.rfind(L"COM", 0) == 0) {
                    try {
                        int num = std::stoi(portStr.substr(3));
                        if (std::find(ports.begin(), ports.end(), num) == ports.end()) {
                            ports.push_back(num);
                        }
                    }
                    catch (...) {}
                }
            }
            valNameLen = 256;
            dataSize = 256;
        }
        RegCloseKey(hKey);
    }

    std::sort(ports.begin(), ports.end());
    return ports;
}

// ============================================================================
// БЭКЕНД: Логика работы с COM-портом
// ============================================================================

bool OpenAndConfigureSerial(int portNumber, BYTE stopBits) {
    CloseSerial();

    std::wstring portName = L"\\\\.\\COM" + std::to_wstring(portNumber);
    hSerial = CreateFileW(
        portName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hSerial == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::wstring msg = L"Не удалось открыть " + portName + L"\n";
        if (err == ERROR_FILE_NOT_FOUND) {
            msg += L"Причина: Порт отсутствует в системе.";
        }
        else if (err == ERROR_ACCESS_DENIED) {
            msg += L"Причина: Порт уже занят другой программой.";
        }
        else {
            msg += L"Код ошибки: " + std::to_wstring(err);
        }
        MessageBoxW(NULL, msg.c_str(), L"Ошибка подключения", MB_ICONWARNING | MB_OK);
        return false;
    }

    SetupComm(hSerial, 4096, 4096);

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(hSerial, &dcb)) {
        CloseSerial();
        MessageBoxW(NULL, L"Не удалось получить параметры DCB!", L"Ошибка", MB_ICONERROR | MB_OK);
        return false;
    }

    dcb.BaudRate = CBR_9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = stopBits;

    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(hSerial, &dcb)) {
        CloseSerial();
        MessageBoxW(NULL, L"Ошибка SetCommState!", L"Ошибка", MB_ICONERROR | MB_OK);
        return false;
    }

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(hSerial, &timeouts);

    PurgeComm(hSerial, PURGE_TXCLEAR | PURGE_RXCLEAR);

    g_bRunning = true;
    hReadThread = CreateThread(NULL, 0, SerialReadThread, NULL, 0, NULL);

    if (!g_portLocked) {
        g_portLocked = true;
        EnableWindow(hComboPort, FALSE);
    }

    return true;
}

void CloseSerial() {
    g_bRunning = false;
    if (hReadThread != NULL) {
        WaitForSingleObject(hReadThread, 500);
        CloseHandle(hReadThread);
        hReadThread = NULL;
    }
    if (hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(hSerial);
        hSerial = INVALID_HANDLE_VALUE;
    }
}

DWORD WINAPI SerialReadThread(LPVOID lpParam) {
    char buf[128];
    DWORD bytesRead = 0;

    while (g_bRunning) {
        if (hSerial == INVALID_HANDLE_VALUE) {
            Sleep(50);
            continue;
        }

        BOOL success = ReadFile(hSerial, buf, sizeof(buf) - 1, &bytesRead, NULL);
        if (success && bytesRead > 0) {
            buf[bytesRead] = '\0';
            AppendTextToOutput(buf, bytesRead);
        }
        else {
            Sleep(10);
        }
    }
    return 0;
}

void AppendTextToOutput(const char* data, DWORD len) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, data, len, NULL, 0);
    if (wlen <= 0) return;

    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, data, len, &wstr[0], wlen);

    int textLen = GetWindowTextLengthW(hEditOutput);
    SendMessageW(hEditOutput, EM_SETSEL, (WPARAM)textLen, (LPARAM)textLen);
    SendMessageW(hEditOutput, EM_REPLACESEL, FALSE, (LPARAM)wstr.c_str());
    SendMessageW(hEditOutput, EM_SCROLLCARET, 0, 0);
}

BYTE GetSelectedStopBits() {
    int idx = (int)SendMessageW(hComboStopBits, CB_GETCURSEL, 0, 0);
    return (idx == 1) ? TWOSTOPBITS : ONESTOPBIT;
}

// ============================================================================
// ФРОНТЕНД: Посимвольная отправка без стирания (текст остаётся в окне)
// ============================================================================

LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CHAR) {
        wchar_t wch = (wchar_t)wParam;

        if (hSerial != INVALID_HANDLE_VALUE) {
            if (wch == VK_RETURN) {
                char crlf[2] = { '\r', '\n' };
                DWORD written = 0;
                for (int i = 0; i < 2; ++i) {
                    if (WriteFile(hSerial, &crlf[i], 1, &written, NULL) && written == 1) {
                        g_txCount++;
                    }
                }
                return CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
            }
            else if (wch >= 32) {
                char ch = 0;
                if (WideCharToMultiByte(CP_ACP, 0, &wch, 1, &ch, 1, NULL, NULL) > 0) {
                    DWORD written = 0;
                    if (WriteFile(hSerial, &ch, 1, &written, NULL) && written == 1) {
                        g_txCount++;
                    }
                }
                return CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
            }
        }
    }
    return CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
}

// ============================================================================
// ФРОНТЕНД: Оконные процедуры
// ============================================================================

// 1. Окно управления (параметры)
LRESULT CALLBACK WndProcControl(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // 1. Выбор COM-порта (заполняется только обнаруженными портами)
        hComboPort = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            15, 20, 185, 200, hWnd, (HMENU)IDC_COMBO_PORT, NULL, NULL);

        std::vector<int> ports = GetAvailableComPorts();
        if (ports.empty()) {
            SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)L"Порты не найдены");
            SendMessageW(hComboPort, CB_SETITEMDATA, 0, (LPARAM)0);
            SendMessageW(hComboPort, CB_SETCURSEL, 0, 0);
            EnableWindow(hComboPort, FALSE);
        }
        else {
            SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)L"-- Выберите порт --");
            SendMessageW(hComboPort, CB_SETITEMDATA, 0, (LPARAM)0);

            for (int p : ports) {
                std::wstring name = L"COM-порт: COM" + std::to_wstring(p);
                int idx = (int)SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)name.c_str());
                SendMessageW(hComboPort, CB_SETITEMDATA, idx, (LPARAM)p);
            }
            SendMessageW(hComboPort, CB_SETCURSEL, 0, 0);
        }

        // 2. Выбор стоп-битов
        hComboStopBits = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            215, 20, 185, 200, hWnd, (HMENU)IDC_COMBO_STOPBITS, NULL, NULL);
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 1 стоп-бит");
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 2 стоп-бита");
        SendMessageW(hComboStopBits, CB_SETCURSEL, 0, 0);
        break;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);

        if (wmId == IDC_COMBO_PORT && wmEvent == CBN_SELCHANGE) {
            int curSel = (int)SendMessageW(hComboPort, CB_GETCURSEL, 0, 0);
            if (curSel != CB_ERR && !g_portLocked) {
                int portNumber = (int)SendMessageW(hComboPort, CB_GETITEMDATA, curSel, 0);
                if (portNumber > 0) {
                    if (OpenAndConfigureSerial(portNumber, GetSelectedStopBits())) {
                        std::wstring okMsg = L"COM" + std::to_wstring(portNumber) + L" открыт. Передано символов: 0";
                        SetWindowTextW(hStaticStatus, okMsg.c_str());
                    }
                    SetFocus(hEditInput);
                }
            }
        }
        if (wmId == IDC_COMBO_STOPBITS && wmEvent == CBN_SELCHANGE) {
            if (hSerial != INVALID_HANDLE_VALUE) {
                DCB dcb = { 0 };
                dcb.DCBlength = sizeof(DCB);
                if (GetCommState(hSerial, &dcb)) {
                    dcb.StopBits = GetSelectedStopBits();
                    SetCommState(hSerial, &dcb);
                }
            }
            SetFocus(hEditInput);
        }
        break;
    }
    case WM_DESTROY:
        CloseSerial();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// 2. Окно состояния
LRESULT CALLBACK WndProcStatus(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        hStaticStatus = CreateWindowW(L"STATIC", L"Выберите COM-порт в окне управления...",
            WS_CHILD | WS_VISIBLE,
            15, 25, 380, 40, hWnd, (HMENU)IDC_STATIC_STATUS, NULL, NULL);
        SetTimer(hWnd, IDT_STATUS_TIMER, 300, NULL);
        break;
    case WM_TIMER:
        if (wParam == IDT_STATUS_TIMER && hSerial != INVALID_HANDLE_VALUE) {
            std::wstring statusText = L"Порт активен. Количество переданных символов: " +
                std::to_wstring(g_txCount.load());
            SetWindowTextW(hStaticStatus, statusText.c_str());
        }
        break;
    case WM_DESTROY:
        KillTimer(hWnd, IDT_STATUS_TIMER);
        CloseSerial();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// 3. Окно ввода сообщений
LRESULT CALLBACK WndProcInput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        hEditInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
            10, 10, 395, 230, hWnd, (HMENU)IDC_EDIT_INPUT, NULL, NULL);
        origEditProc = (WNDPROC)SetWindowLongPtrW(hEditInput, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
        break;
    case WM_SIZE:
        MoveWindow(hEditInput, 10, 10, LOWORD(lParam) - 20, HIWORD(lParam) - 20, TRUE);
        break;
    case WM_SETFOCUS:
        SetFocus(hEditInput);
        break;
    case WM_DESTROY:
        CloseSerial();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// 4. Окно вывода сообщений
LRESULT CALLBACK WndProcOutput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        hEditOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            10, 10, 395, 230, hWnd, (HMENU)IDC_EDIT_OUTPUT, NULL, NULL);
        break;
    case WM_SIZE:
        MoveWindow(hEditOutput, 10, 10, LOWORD(lParam) - 20, HIWORD(lParam) - 20, TRUE);
        break;
    case WM_DESTROY:
        CloseSerial();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ============================================================================
// Точка входа WinMain
// ============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    auto RegisterCustomClass = [hInstance](const wchar_t* className, WNDPROC proc) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = proc;
        wc.hInstance = hInstance;
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        };

    RegisterCustomClass(L"OSKS_Control_Class", WndProcControl);
    RegisterCustomClass(L"OSKS_Status_Class", WndProcStatus);
    RegisterCustomClass(L"OSKS_Input_Class", WndProcInput);
    RegisterCustomClass(L"OSKS_Output_Class", WndProcOutput);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    int winW = 430;
    int topH = 110;
    int botH = 290;

    int totalW = winW * 2 + 15;
    int totalH = topH + botH + 15;

    int startX = (screenW - totalW) / 2;
    int startY = (screenH - totalH) / 2;
    if (startX < 10) startX = 10;
    if (startY < 10) startY = 10;

    // 1. Окно управления (верхнее левое)
    hWndControl = CreateWindowExW(0, L"OSKS_Control_Class", L"Окно управления (Параметры)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        startX, startY, winW, topH, NULL, NULL, hInstance, NULL);

    // 2. Окно состояния (верхнее правое)
    hWndStatus = CreateWindowExW(0, L"OSKS_Status_Class", L"Окно состояния",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        startX + winW + 15, startY, winW, topH, NULL, NULL, hInstance, NULL);

    // 3. Окно ввода (нижнее левое)
    hWndInput = CreateWindowExW(0, L"OSKS_Input_Class", L"Окно ввода сообщений",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
        startX, startY + topH + 15, winW, botH, NULL, NULL, hInstance, NULL);

    // 4. Окно вывода (нижнее правое)
    hWndOutput = CreateWindowExW(0, L"OSKS_Output_Class", L"Окно вывода сообщений",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
        startX + winW + 15, startY + topH + 15, winW, botH, NULL, NULL, hInstance, NULL);

    if (!hWndControl || !hWndStatus || !hWndInput || !hWndOutput) return 0;

    ShowWindow(hWndControl, nCmdShow);
    UpdateWindow(hWndControl);

    ShowWindow(hWndStatus, nCmdShow);
    UpdateWindow(hWndStatus);

    ShowWindow(hWndInput, nCmdShow);
    UpdateWindow(hWndInput);

    ShowWindow(hWndOutput, nCmdShow);
    UpdateWindow(hWndOutput);

    SetFocus(hEditInput);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}