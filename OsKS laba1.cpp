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

// Подключение современных стилей Windows
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Идентификаторы элементов интерфейса
#define IDC_COMBO_PORT      101
#define IDC_COMBO_STOPBITS  102
#define IDC_EDIT_INPUT      103
#define IDC_EDIT_OUTPUT     104
#define IDC_STATIC_STATUS   105
#define IDT_STATUS_TIMER    201

// Дескрипторы окон и рамок-групп
HWND hWndMain      = NULL;
HWND hGroupControl = NULL;
HWND hGroupStatus  = NULL;
HWND hGroupInput   = NULL;
HWND hGroupOutput  = NULL;

// Элементы управления
HWND hComboPort     = NULL;
HWND hComboStopBits = NULL;
HWND hEditInput     = NULL;
HWND hEditOutput    = NULL;
HWND hStaticStatus  = NULL;

WNDPROC origEditProc = NULL;
HANDLE hSerial = INVALID_HANDLE_VALUE;
HANDLE hReadThread = NULL;
std::atomic<bool> g_bRunning(false);
std::atomic<unsigned long long> g_txCount(0); // Счетчик переданных символов
bool g_portLocked = false;

// Прототипы функций
bool OpenAndConfigureSerial(int portNumber, BYTE stopBits);
void CloseSerial();
DWORD WINAPI SerialReadThread(LPVOID lpParam);
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void AppendTextToOutput(const char* data, DWORD len);
BYTE GetSelectedStopBits();
std::vector<int> GetAvailableComPorts();

// Поиск COM-портов в системе
std::vector<int> GetAvailableComPorts() {
    std::vector<int> ports;
    wchar_t targetPath[256];

    // 1. Опрос системных DOS-устройств COM1..COM256
    for (int i = 1; i <= 256; ++i) {
        std::wstring portName = L"COM" + std::to_wstring(i);
        if (QueryDosDeviceW(portName.c_str(), targetPath, 256) != 0) {
            ports.push_back(i);
        }
    }

    // 2. Сканирование системного реестра (переходники USB-to-COM)
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

// Инициализация и настройка COM-порта
bool OpenAndConfigureSerial(int portNumber, BYTE stopBits) {
    CloseSerial();

    std::wstring portName = L"\\\\.\\COM" + std::to_wstring(portNumber);
    hSerial = CreateFileW(
        portName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,              // Эксклюзивный доступ
        NULL,           // Дескриптор безопасности по умолчанию
        OPEN_EXISTING,  // Порт должен реально существовать
        0,              // Синхронный ввод/вывод
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

    // Параметры порта
    dcb.BaudRate = CBR_9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = stopBits;

    // Режим работы контроллера UART
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

    // Мгновенный таймаут чтения
    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(hSerial, &timeouts);

    PurgeComm(hSerial, PURGE_TXCLEAR | PURGE_RXCLEAR);

    // Запуск фонового потока приема данных
    g_bRunning = true;
    hReadThread = CreateThread(NULL, 0, SerialReadThread, NULL, 0, NULL);

    // Блокировка списка после открытия порта
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

// Фоновый поток чтения из порта
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

// Вывод принятых символов в окно вывода
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

// Получение выбранных стоп-битов (0xFF = не выбрано)
BYTE GetSelectedStopBits() {
    int idx = (int)SendMessageW(hComboStopBits, CB_GETCURSEL, 0, 0);
    if (idx == 1) return ONESTOPBIT;  // 1 стоп-бит
    if (idx == 2) return TWOSTOPBITS;  // 2 стоп-бита
    return 0xFF;                       // Подсказка / не выбрано
}

// Отправка символов при вводе (WM_CHAR)
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // 1. Блокируем Backspace и Delete
    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_BACK || wParam == VK_DELETE) {
            return 0; // Игнорируем: стирать текст нельзя
        }
    }

    // 2. Блокируем вырезание и очистку (Ctrl+X и меню)
    if (uMsg == WM_CUT || uMsg == WM_CLEAR) {
        return 0; // Запрещаем удаление через буфер обмена
    }

    // 3. Обработка ввода символов и Enter
    if (uMsg == WM_CHAR) {
        wchar_t wch = (wchar_t)wParam;

        // Блокируем символ Backspace
        if (wch == VK_BACK) {
            return 0;
        }

        if (hSerial != INVALID_HANDLE_VALUE) {
            // Защита от затирания: если был выделен текст, снимаем выделение 
            // и переносим каретку строго в конец текста перед вводом нового символа
            int textLen = GetWindowTextLengthW(hWnd);
            SendMessageW(hWnd, EM_SETSEL, (WPARAM)textLen, (LPARAM)textLen);

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
        else {
            // Если порт ещё не открыт, не даём печатать впустую
            return 0;
        }
    }

    return CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
}

// Главное окно приложения (сетка 2х2)
LRESULT CALLBACK WndProcMain(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // 1. Окно управления
        hGroupControl = CreateWindowW(L"BUTTON", L"Окно управления (Параметры COM-порта)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupControl, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Выбор COM-порта
        hComboPort = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_COMBO_PORT, NULL, NULL);
        SendMessageW(hComboPort, WM_SETFONT, (WPARAM)hFont, TRUE);

        std::vector<int> ports = GetAvailableComPorts();

        if (ports.empty()) {
            SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)L"COM-порты не обнаружены");
            SendMessageW(hComboPort, CB_SETITEMDATA, 0, (LPARAM)0);
            SendMessageW(hComboPort, CB_SETCURSEL, 0, 0);
        }
        else {
            SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)L"-- Выберите COM-порт --");
            SendMessageW(hComboPort, CB_SETITEMDATA, 0, (LPARAM)0);

            for (int p : ports) {
                std::wstring name = L"COM-порт: COM" + std::to_wstring(p);
                int idx = (int)SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)name.c_str());
                SendMessageW(hComboPort, CB_SETITEMDATA, idx, (LPARAM)p);
            }
            SendMessageW(hComboPort, CB_SETCURSEL, 0, 0);
        }

        // Выбор количества стоп-битов
        hComboStopBits = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_COMBO_STOPBITS, NULL, NULL);
        SendMessageW(hComboStopBits, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"-- Выберите стоп-биты --");
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 1 стоп-бит");
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 2 стоп-бита");
        SendMessageW(hComboStopBits, CB_SETCURSEL, 0, 0);

        // 2. Окно состояния
        hGroupStatus = CreateWindowW(L"BUTTON", L"Окно состояния",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        hStaticStatus = CreateWindowW(L"STATIC", L"Выберите параметры в окне управления...",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_STATUS, NULL, NULL);
        SendMessageW(hStaticStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        SetTimer(hWnd, IDT_STATUS_TIMER, 300, NULL);

        // 3. Окно ввода сообщений
        hGroupInput = CreateWindowW(L"BUTTON", L"Окно ввода сообщений",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupInput, WM_SETFONT, (WPARAM)hFont, TRUE);

        hEditInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_INPUT, NULL, NULL);
        SendMessageW(hEditInput, WM_SETFONT, (WPARAM)hFont, TRUE);
        origEditProc = (WNDPROC)SetWindowLongPtrW(hEditInput, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        // 4. Окно вывода сообщений
        hGroupOutput = CreateWindowW(L"BUTTON", L"Окно вывода сообщений",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupOutput, WM_SETFONT, (WPARAM)hFont, TRUE);

        hEditOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_OUTPUT, NULL, NULL);
        SendMessageW(hEditOutput, WM_SETFONT, (WPARAM)hFont, TRUE);
        break;
    }

    case WM_SIZE: {
        int totalW = LOWORD(lParam);
        int totalH = HIWORD(lParam);
        int margin = 12;
        int gap = 12;
        int topH = 85;
        int botH = totalH - topH - (margin * 2) - gap;
        int colW = (totalW - (margin * 2) - gap) / 2;

        if (colW > 120 && botH > 60) {
            // Верхний левый блок: Управление
            MoveWindow(hGroupControl, margin, margin, colW, topH, TRUE);
            int comboW = (colW - 36) / 2;
            MoveWindow(hComboPort, margin + 12, margin + 30, comboW, 250, TRUE);
            MoveWindow(hComboStopBits, margin + 24 + comboW, margin + 30, comboW, 250, TRUE);

            // Верхний правый блок: Состояние
            MoveWindow(hGroupStatus, margin + colW + gap, margin, colW, topH, TRUE);
            MoveWindow(hStaticStatus, margin + colW + gap + 15, margin + 34, colW - 30, 36, TRUE);

            // Нижний левый блок: Ввод
            MoveWindow(hGroupInput, margin, margin + topH + gap, colW, botH, TRUE);
            MoveWindow(hEditInput, margin + 12, margin + topH + gap + 25, colW - 24, botH - 38, TRUE);

            // Нижний правый блок: Вывод
            MoveWindow(hGroupOutput, margin + colW + gap, margin + topH + gap, colW, botH, TRUE);
            MoveWindow(hEditOutput, margin + colW + gap + 12, margin + topH + gap + 25, colW - 24, botH - 38, TRUE);
        }
        break;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);

        // Выбор COM-порта
        if (wmId == IDC_COMBO_PORT && wmEvent == CBN_SELCHANGE) {
            int curSel = (int)SendMessageW(hComboPort, CB_GETCURSEL, 0, 0);
            if (curSel != CB_ERR && !g_portLocked) {
                int portNumber = (int)SendMessageW(hComboPort, CB_GETITEMDATA, curSel, 0);
                if (portNumber > 0) {
                    BYTE stopBits = GetSelectedStopBits();
                    if (stopBits == 0xFF) {
                        MessageBoxW(hWnd, L"Сначала выберите количество стоп-битов в окне управления!",
                            L"Параметры COM-порта", MB_ICONINFORMATION | MB_OK);
                        SendMessageW(hComboPort, CB_SETCURSEL, 0, 0);
                        SetFocus(hComboStopBits);
                        break;
                    }

                    if (OpenAndConfigureSerial(portNumber, stopBits)) {
                        std::wstring okMsg = L"COM" + std::to_wstring(portNumber) + L" открыт. Количество переданных символов: 0";
                        SetWindowTextW(hStaticStatus, okMsg.c_str());
                    }
                    SetFocus(hEditInput);
                }
            }
        }

        // Изменение стоп-битов
        if (wmId == IDC_COMBO_STOPBITS && wmEvent == CBN_SELCHANGE) {
            BYTE stopBits = GetSelectedStopBits();
            if (stopBits != 0xFF && hSerial != INVALID_HANDLE_VALUE) {
                DCB dcb = { 0 };
                dcb.DCBlength = sizeof(DCB);
                if (GetCommState(hSerial, &dcb)) {
                    dcb.StopBits = stopBits;
                    SetCommState(hSerial, &dcb);
                }
            }
            SetFocus(hEditInput);
        }
        break;
    }

    case WM_TIMER:
        if (wParam == IDT_STATUS_TIMER && hSerial != INVALID_HANDLE_VALUE) {
            std::wstring statusText = L"Порт активен. Количество переданных символов: " +
                std::to_wstring(g_txCount.load());
            SetWindowTextW(hStaticStatus, statusText.c_str());
        }
        break;

    case WM_SETFOCUS:
        SetFocus(hEditInput);
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

// Точка входа WinMain
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"OSKS_Main_Class";

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProcMain;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClassW(&wc);

    // Создание главного окна
    hWndMain = CreateWindowExW(0, CLASS_NAME,
        L"ОсКС — Лабораторная работа №1 (COM-порты, Вариант 3)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 580,
        NULL, NULL, hInstance, NULL);

    if (!hWndMain) return 0;

    ShowWindow(hWndMain, nCmdShow);
    UpdateWindow(hWndMain);
    SetFocus(hEditInput);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}