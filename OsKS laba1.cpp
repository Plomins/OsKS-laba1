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

// Идентификаторы элементов управления (Control IDs)
#define IDC_COMBO_PORT      101
#define IDC_COMBO_BYTESIZE  102
#define IDC_EDIT_INPUT      103
#define IDC_EDIT_OUTPUT     104
#define IDC_STATIC_STATUS   105
#define IDT_STATUS_TIMER    201

// Глобальные дескрипторы и переменные состояния
HWND hComboPort     = NULL;
HWND hComboByteSize = NULL;
HWND hEditInput     = NULL;
HWND hEditOutput    = NULL;
HWND hStaticStatus  = NULL;
WNDPROC origEditProc = NULL;

HANDLE hSerial = INVALID_HANDLE_VALUE;
HANDLE hReadThread = NULL;
std::atomic<bool> g_bRunning(false);
std::atomic<unsigned long long> g_txCount(0); // Счётчик переданных символов

// Прототипы функций
bool OpenAndConfigureSerial(int portNumber, BYTE byteSize);
void CloseSerial();
DWORD WINAPI SerialReadThread(LPVOID lpParam);
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void SendCurrentLine();
void AppendTextToOutput(const char* data, DWORD len);

/*
================================================================================
 СТРУКТУРА DCB И ВСЕ ПАРАМЕТРЫ ИНИЦИАЛИЗАЦИИ СОМ-ПОРТА (UART 16550):
 
 1. DCBlength: размер структуры в байтах sizeof(DCB).
 2. BaudRate (Скорость передачи):
    Возможные значения Win32: CBR_110, CBR_300, CBR_600, CBR_1200, CBR_2400,
    CBR_4800, CBR_9600, CBR_14400, CBR_19200, CBR_38400, CBR_57600, CBR_115200,
    CBR_128000, CBR_256000. В UART 16550 задаётся делителем базовой частоты.
 3. ByteSize (Длина информационного слова / байта) — ВАРИАНТ 2:
    Возможные значения по стандарту UART 16550 / Win32: 5, 6, 7, 8 бит.
 4. Parity (Контроль четности):
    - NOPARITY (0)   : без бита четности;
    - ODDPARITY (1)  : нечетность (сумма единиц нечетна);
    - EVENPARITY (2) : четность (сумма единиц четна);
    - MARKPARITY (3) : бит четности всегда 1;
    - SPACEPARITY (4): бит четности всегда 0.
 5. StopBits (Количество стоп-битов):
    - ONESTOPBIT (0)   : 1 стоп-бит;
    - ONE5STOPBITS (1) : 1.5 стоп-бита (для 5-битных данных);
    - TWOSTOPBITS (2)  : 2 стоп-бита.
 6. Флаги управления потоком (Flow Control) и протоколом:
    - fBinary: двоичный режим (для Win32 всегда TRUE);
    - fParity: включение проверки четности (TRUE/FALSE);
    - fOutxCtsFlow / fOutxDsrFlow: аппаратный контроль CTS/DSR;
    - fDtrControl: DTR_CONTROL_DISABLE, DTR_CONTROL_ENABLE, DTR_CONTROL_HANDSHAKE;
    - fRtsControl: RTS_CONTROL_DISABLE, RTS_CONTROL_ENABLE, RTS_CONTROL_HANDSHAKE, RTS_CONTROL_TOGGLE;
    - fOutX / fInX: программный контроль XON/XOFF;
    - XonChar / XoffChar: символы паузы/возобновления (обычно 0x11 и 0x13).
================================================================================
*/

bool OpenAndConfigureSerial(int portNumber, BYTE byteSize) {
    CloseSerial();

    std::wstring portName = L"\\\\.\\COM" + std::to_wstring(portNumber);
    hSerial = CreateFileW(
        portName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,              // Эксклюзивный доступ
        NULL,           // Безопасность по умолчанию
        OPEN_EXISTING,  // Только существующий порт
        0,              // Блокирующий ввод/вывод (без OVERLAPPED)
        NULL
    );

    if (hSerial == INVALID_HANDLE_VALUE) {
        MessageBoxW(NULL, L"Ошибка открытия СОМ-порта!", L"Ошибка", MB_ICONERROR | MB_OK);
        return false;
    }

    // Настройка аппаратных буферов драйвера
    SetupComm(hSerial, 4096, 4096);

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(hSerial, &dcb)) {
        CloseSerial();
        MessageBoxW(NULL, L"Не удалось получить состояние порта!", L"Ошибка", MB_ICONERROR | MB_OK);
        return false;
    }

    // Фиксированные параметры согласно заданию:
    dcb.BaudRate = CBR_9600;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity   = NOPARITY;

    // Изменяемый параметр (Вариант 2):
    dcb.ByteSize = byteSize; // 5, 6, 7 или 8 бит

    // Режим работы UART
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(hSerial, &dcb)) {
        CloseSerial();
        MessageBoxW(NULL, L"Не удалось установить параметры порта (SetCommState)!", L"Ошибка", MB_ICONERROR | MB_OK);
        return false;
    }

    // Настройка таймаутов (минимальная задержка чтения для мгновенного отображения)
    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(hSerial, &timeouts);

    PurgeComm(hSerial, PURGE_TXCLEAR | PURGE_RXCLEAR);

    // Запуск фонового потока циклического приема данных
    g_bRunning = true;
    hReadThread = CreateThread(NULL, 0, SerialReadThread, NULL, 0, NULL);

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

// Поток непрерывного приема данных
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
        } else {
            Sleep(10);
        }
    }
    return 0;
}

// Отображение принятых символов в окне вывода
void AppendTextToOutput(const char* data, DWORD len) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, data, len, NULL, 0);
    if (wlen <= 0) return;
    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, data, len, &wstr[0], wlen);

    int textLen = GetWindowTextLengthW(hEditOutput);
    SendMessageW(hEditOutput, EM_SETSEL, (WPARAM)textLen, (LPARAM)textLen);
    SendMessageW(hEditOutput, EM_REPLACESEL, FALSE, (LPARAM)wstr.c_str());
}

// Отправка введенной строки построчно по Enter (сырой посимвольный поток)
void SendCurrentLine() {
    if (hSerial == INVALID_HANDLE_VALUE) {
        MessageBoxW(NULL, L"СОМ-порт не открыт!", L"Предупреждение", MB_ICONWARNING | MB_OK);
        return;
    }

    int len = GetWindowTextLengthW(hEditInput);
    std::wstring wbuffer(len + 1, 0);
    GetWindowTextW(hEditInput, &wbuffer[0], len + 1);
    wbuffer.resize(len);

    // Очищаем окно ввода сразу после отправки
    SetWindowTextW(hEditInput, L"");

    // Добавляем перевод строки
    wbuffer += L"\r\n";

    // Преобразуем в сырые байты (ANSI)
    int byteCount = WideCharToMultiByte(CP_ACP, 0, wbuffer.c_str(), -1, NULL, 0, NULL, NULL);
    std::vector<char> rawData(byteCount);
    WideCharToMultiByte(CP_ACP, 0, wbuffer.c_str(), -1, rawData.data(), byteCount, NULL, NULL);

    // Передача данных посимвольно в соответствии с п.8 задания
    for (size_t i = 0; i < rawData.size() - 1; ++i) {
        char ch = rawData[i];
        DWORD bytesWritten = 0;
        if (WriteFile(hSerial, &ch, 1, &bytesWritten, NULL) && bytesWritten == 1) {
            g_txCount++;
        }
    }
}

// Перехват клавиши Enter в поле ввода сообщений
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        SendCurrentLine();
        return 0;
    }
    return CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
}

// Применение настроек из Окна управления (содержит ровно 2 элемента)
void ApplyPortSettings() {
    int portIdx = (int)SendMessageW(hComboPort, CB_GETCURSEL, 0, 0);
    int sizeIdx = (int)SendMessageW(hComboByteSize, CB_GETCURSEL, 0, 0);

    if (portIdx == CB_ERR || sizeIdx == CB_ERR) return;

    int portNumber = portIdx + 1;       // Индекс 0 = COM1, 1 = COM2, 2 = COM3...
    BYTE byteSize  = (BYTE)(5 + sizeIdx); // Индекс 0 = 5 бит, 1 = 6 бит, 2 = 7 бит, 3 = 8 бит

    OpenAndConfigureSerial(portNumber, byteSize);
}

// Главная оконная процедура
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // --- 1. ОКНО УПРАВЛЕНИЯ (GROUPBOX) ---
        CreateWindowW(L"BUTTON", L"Окно управления (параметры)", 
                      WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                      15, 10, 550, 80, hWnd, NULL, NULL, NULL);

        // Элемент 1 из 2: Выбор COM-порта
        hComboPort = CreateWindowW(L"COMBOBOX", NULL, 
                                   WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   30, 40, 240, 200, hWnd, (HMENU)IDC_COMBO_PORT, NULL, NULL);
        for (int i = 1; i <= 16; ++i) {
            std::wstring name = L"СОМ-порт: COM" + std::to_wstring(i);
            SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        }
        SendMessageW(hComboPort, CB_SETCURSEL, 2, 0); // По умолчанию COM3

        // Элемент 2 из 2: Выбор длины байта (Вариант 2)
        hComboByteSize = CreateWindowW(L"COMBOBOX", NULL, 
                                       WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                       290, 40, 250, 200, hWnd, (HMENU)IDC_COMBO_BYTESIZE, NULL, NULL);
        SendMessageW(hComboByteSize, CB_ADDSTRING, 0, (LPARAM)L"Длина байта: 5 бит");
        SendMessageW(hComboByteSize, CB_ADDSTRING, 0, (LPARAM)L"Длина байта: 6 бит");
        SendMessageW(hComboByteSize, CB_ADDSTRING, 0, (LPARAM)L"Длина байта: 7 бит");
        SendMessageW(hComboByteSize, CB_ADDSTRING, 0, (LPARAM)L"Длина байта: 8 бит");
        SendMessageW(hComboByteSize, CB_SETCURSEL, 3, 0); // По умолчанию 8 бит

        // --- 2. ОКНО ВВОДА СООБЩЕНИЙ ---
        CreateWindowW(L"STATIC", L"Окно ввода сообщений (отправка по нажатию Enter):", 
                      WS_CHILD | WS_VISIBLE, 15, 105, 550, 18, hWnd, NULL, NULL, NULL);

        hEditInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", 
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    15, 125, 550, 28, hWnd, (HMENU)IDC_EDIT_INPUT, NULL, NULL);
        origEditProc = (WNDPROC)SetWindowLongPtrW(hEditInput, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        // --- 3. ОКНО ВЫВОДА СООБЩЕНИЙ ---
        CreateWindowW(L"STATIC", L"Окно вывода сообщений (прием данных):", 
                      WS_CHILD | WS_VISIBLE, 15, 165, 550, 18, hWnd, NULL, NULL, NULL);

        hEditOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", 
                                     WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | 
                                     ES_READONLY | WS_VSCROLL,
                                     15, 185, 550, 160, hWnd, (HMENU)IDC_EDIT_OUTPUT, NULL, NULL);

        // --- 4. ОКНО СОСТОЯНИЯ ---
        CreateWindowW(L"BUTTON", L"Окно состояния", 
                      WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                      15, 360, 550, 60, hWnd, NULL, NULL, NULL);

        hStaticStatus = CreateWindowW(L"STATIC", L"Количество переданных символов: 0", 
                                     WS_CHILD | WS_VISIBLE,
                                     30, 385, 500, 20, hWnd, (HMENU)IDC_STATIC_STATUS, NULL, NULL);

        // Периодическое обновление окна состояния (каждые 300 мс)
        SetTimer(hWnd, IDT_STATUS_TIMER, 300, NULL);

        // Первоначальное открытие порта с выбранными значениями
        ApplyPortSettings();
        SetFocus(hEditInput);
        break;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);

        // Реагируем на выбор в выпадающих списках окна управления
        if ((wmId == IDC_COMBO_PORT || wmId == IDC_COMBO_BYTESIZE) && wmEvent == CBN_SELCHANGE) {
            ApplyPortSettings();
            SetFocus(hEditInput); // Возвращаем фокус на поле ввода
        }
        break;
    }

    case WM_TIMER: {
        if (wParam == IDT_STATUS_TIMER) {
            std::wstring statusText = L"Количество переданных символов: " + std::to_wstring(g_txCount.load());
            SetWindowTextW(hStaticStatus, statusText.c_str());
        }
        break;
    }

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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"OSKS_Lab1_Variant2_Class";

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(
        0, CLASS_NAME, 
        L"ОсКС. Лабораторная работа №1 — Вариант 2",
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