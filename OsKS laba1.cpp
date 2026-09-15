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

#pragma comment(lib, "comctl32.lib")

// Идентификаторы элементов интерфейса
#define IDC_COMBO_PORT      101
#define IDC_COMBO_STOPBITS  102
#define IDC_EDIT_INPUT      103
#define IDC_EDIT_OUTPUT     104
#define IDC_STATIC_STATUS   105

// Таймеры
#define IDT_STATUS_TIMER    201
#define IDT_CLEAR_INPUT     203 // Таймер задержки стирания (500 мс)

// Дескрипторы окон и элементы состояния
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

bool OpenAndConfigureSerial(int portNumber, BYTE stopBits) {
    CloseSerial();

    std::wstring portName = L"\\\\.\\COM" + std::to_wstring(portNumber);
    hSerial = CreateFileW(
        portName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,              // Эксклюзивный доступ
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

    // Фиксированные параметры UART 16550
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
}

// Перехват клавиш: отправка сразу, показ символа и запуск таймера на стирание через 500 мс
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
                // Визуальная подсказка о переходе на новую строку
                SetWindowTextW(hWnd, L"[Enter]");
                SetTimer(GetParent(hWnd), IDT_CLEAR_INPUT, 500, NULL);
                return 0;
            }
            else if (wch >= 32) {
                char ch = 0;
                if (WideCharToMultiByte(CP_ACP, 0, &wch, 1, &ch, 1, NULL, NULL) > 0) {
                    DWORD written = 0;
                    if (WriteFile(hSerial, &ch, 1, &written, NULL) && written == 1) {
                        g_txCount++;
                    }
                }
                // Показываем отправленный символ
                std::wstring s(1, wch);
                SetWindowTextW(hWnd, s.c_str());

                // Запускаем задержку 500 мс до стирания
                SetTimer(GetParent(hWnd), IDT_CLEAR_INPUT, 500, NULL);
                return 0;
            }
        }
    }
    return CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
}

BYTE GetSelectedStopBits() {
    int idx = (int)SendMessageW(hComboStopBits, CB_GETCURSEL, 0, 0);
    return (idx == 1) ? TWOSTOPBITS : ONESTOPBIT;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // Окно управления (ровно 2 элемента)
        CreateWindowW(L"BUTTON", L"Окно управления (параметры)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            15, 10, 550, 80, hWnd, NULL, NULL, NULL);

        // 1. Выбор COM-порта
        hComboPort = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            30, 40, 240, 250, hWnd, (HMENU)IDC_COMBO_PORT, NULL, NULL);
        SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)L"-- Выберите COM-порт --");
        for (int i = 1; i <= 16; ++i) {
            std::wstring name = L"СОМ-порт: COM" + std::to_wstring(i);
            SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        }
        SendMessageW(hComboPort, CB_SETCURSEL, 0, 0);

        // 2. Выбор стоп-битов (1 или 2 стоп-бита)
        hComboStopBits = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            290, 40, 250, 200, hWnd, (HMENU)IDC_COMBO_STOPBITS, NULL, NULL);
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 1 стоп-бит");
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 2 стоп-бита");
        SendMessageW(hComboStopBits, CB_SETCURSEL, 0, 0);

        // Окно ввода (с визуальным отображением и задержкой перед стиранием)
        CreateWindowW(L"STATIC", L"Окно ввода:",
            WS_CHILD | WS_VISIBLE, 15, 105, 550, 18, hWnd, NULL, NULL, NULL);

        hEditInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            15, 125, 550, 28, hWnd, (HMENU)IDC_EDIT_INPUT, NULL, NULL);
        origEditProc = (WNDPROC)SetWindowLongPtrW(hEditInput, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        // Окно вывода
        CreateWindowW(L"STATIC", L"Окно вывода сообщений (прием данных):",
            WS_CHILD | WS_VISIBLE, 15, 165, 550, 18, hWnd, NULL, NULL, NULL);

        hEditOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_READONLY | WS_VSCROLL,
            15, 185, 550, 160, hWnd, (HMENU)IDC_EDIT_OUTPUT, NULL, NULL);

        // Окно состояния
        CreateWindowW(L"BUTTON", L"Окно состояния",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            15, 360, 550, 60, hWnd, NULL, NULL, NULL);

        hStaticStatus = CreateWindowW(L"STATIC", L"Выберите COM-порт в окне управления...",
            WS_CHILD | WS_VISIBLE,
            30, 385, 500, 20, hWnd, (HMENU)IDC_STATIC_STATUS, NULL, NULL);

        SetTimer(hWnd, IDT_STATUS_TIMER, 300, NULL);
        SetFocus(hEditInput);
        break;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);

        if (wmId == IDC_COMBO_PORT && wmEvent == CBN_SELCHANGE) {
            int portIdx = (int)SendMessageW(hComboPort, CB_GETCURSEL, 0, 0);
            if (portIdx > 0 && !g_portLocked) {
                if (OpenAndConfigureSerial(portIdx, GetSelectedStopBits())) {
                    std::wstring okMsg = L"COM" + std::to_wstring(portIdx) + L" открыт. Количество переданных символов: 0";
                    SetWindowTextW(hStaticStatus, okMsg.c_str());
                }
                SetFocus(hEditInput);
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

    case WM_TIMER: {
        // Стирание поля ввода после истечения задержки
        if (wParam == IDT_CLEAR_INPUT) {
            KillTimer(hWnd, IDT_CLEAR_INPUT);
            SetWindowTextW(hEditInput, L"");
        }

        // Обновление счетчика отправленных символов
        if (wParam == IDT_STATUS_TIMER && hSerial != INVALID_HANDLE_VALUE) {
            std::wstring statusText = L"Порт активен. Количество переданных символов: " +
                std::to_wstring(g_txCount.load());
            SetWindowTextW(hStaticStatus, statusText.c_str());
        }
        break;
    }

    case WM_DESTROY:
        KillTimer(hWnd, IDT_CLEAR_INPUT);
        KillTimer(hWnd, IDT_STATUS_TIMER);
        CloseSerial();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"OSKS_Lab1_Var3_Class";

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(
        0, CLASS_NAME,
        L"ОСКС. Лабораторная работа №1 — Вариант 3",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 595, 470,
        NULL, NULL, hInstance, NULL
    );

    if (hWnd == NULL) return 0;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}