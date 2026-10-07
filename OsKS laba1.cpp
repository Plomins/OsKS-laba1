#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <string>
#include <atomic>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

// Идентификаторы элементов управления
#define IDC_COMBO_PORT      101
#define IDC_COMBO_STOPBITS  102
#define IDC_RICH_STATUS     103
#define IDC_EDIT_INPUT      104
#define IDC_EDIT_OUTPUT     105

// Параметры кадра для Варианта 3
#define MAX_FRAME_SIZE      300         // Максимальная длина кадра (Вариант 3)
#define FRAME_FLAG_STR      "45050103"  // 8 символов: 45050 + 1 + 3 + 0
#define FRAME_FLAG_LEN      8
#define RESERVED_BYTES_LEN  10
#define SERVICE_FIELDS_LEN  4           // SrcAddr(1) + DestAddr(1) + Type(1) + FCS(1)

// Максимальный объем полезных данных в кадре
#define MAX_PAYLOAD_SIZE    (MAX_FRAME_SIZE - FRAME_FLAG_LEN - SERVICE_FIELDS_LEN - 2 - RESERVED_BYTES_LEN)

// Дескрипторы элементов управления
HWND hMainWnd = NULL;
HWND hComboPort = NULL;
HWND hComboStopBits = NULL;
HWND hRichStatus = NULL;
HWND hEditInput = NULL;
HWND hEditOutput = NULL;
WNDPROC origEditProc = NULL;

HANDLE hSerial = INVALID_HANDLE_VALUE;
HANDLE hReadThread = NULL;

std::atomic<bool> g_bRunning(false);
std::atomic<unsigned long long> g_txCount(0);
bool g_portLocked = false;
bool g_statusHeaderPrinted = false;

// Прототипы функций
bool OpenAndConfigureSerial(int portNumber, BYTE stopBits);
void CloseSerial();
DWORD WINAPI SerialReadThread(LPVOID lpParam);
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void AppendTextToOutput(const std::string& str);
BYTE GetSelectedStopBits();
std::vector<int> GetAvailableComPorts();
void SendFramedMessage(const std::string& text);
void AppendStatusFormatted(const wchar_t* prefix, const std::vector<uint8_t>& frameBody, size_t dataLen, const std::vector<bool>& modifiedMask);

// ============================================================================
// АЛГОРИТМ БИТ-СТАФФИНГА (ВАРИАНТ 3: ОПТИМИЗИРОВАННЫЙ)
// ============================================================================
void BitStuff(const std::vector<uint8_t>& inData, std::vector<uint8_t>& outData, std::vector<bool>& modifiedBytes) {
    outData.clear();
    modifiedBytes.clear();

    std::vector<bool> inBits;
    for (uint8_t b : inData) {
        for (int i = 7; i >= 0; --i) {
            inBits.push_back((b >> i) & 1);
        }
    }

    std::vector<bool> outBits;
    std::vector<bool> bitModifiedMask;

    // Префикс первого символа флага '4' (0x34: 00110100b)
    const bool pattern[6] = { 0, 0, 1, 1, 0, 1 };
    int matchCount = 0;

    for (size_t i = 0; i < inBits.size(); ++i) {
        bool bit = inBits[i];
        outBits.push_back(bit);
        bitModifiedMask.push_back(false);

        if (bit == pattern[matchCount]) {
            matchCount++;
            if (matchCount == 6) {
                // Вставка инверсного бита '1'
                outBits.push_back(true);
                bitModifiedMask.push_back(true);
                matchCount = 0;
            }
        }
        else {
            matchCount = (bit == pattern[0]) ? 1 : 0;
        }
    }

    while (outBits.size() % 8 != 0) {
        outBits.push_back(false);
        bitModifiedMask.push_back(false);
    }

    for (size_t i = 0; i < outBits.size(); i += 8) {
        uint8_t b = 0;
        bool byteMod = false;
        for (int bitIdx = 0; bitIdx < 8; ++bitIdx) {
            if (outBits[i + bitIdx]) {
                b |= (1 << (7 - bitIdx));
            }
            if (bitModifiedMask[i + bitIdx]) {
                byteMod = true;
            }
        }
        outData.push_back(b);
        modifiedBytes.push_back(byteMod);
    }
}

std::vector<uint8_t> BitDestuff(const std::vector<uint8_t>& inData) {
    std::vector<bool> inBits;
    for (uint8_t b : inData) {
        for (int i = 7; i >= 0; --i) {
            inBits.push_back((b >> i) & 1);
        }
    }

    std::vector<bool> outBits;
    const bool pattern[6] = { 0, 0, 1, 1, 0, 1 };
    int matchCount = 0;

    for (size_t i = 0; i < inBits.size(); ++i) {
        bool bit = inBits[i];
        outBits.push_back(bit);

        if (bit == pattern[matchCount]) {
            matchCount++;
            if (matchCount == 6) {
                if (i + 1 < inBits.size()) {
                    i++; // Пропуск стаффинг-бита
                }
                matchCount = 0;
            }
        }
        else {
            matchCount = (bit == pattern[0]) ? 1 : 0;
        }
    }

    std::vector<uint8_t> outBytes;
    for (size_t i = 0; i + 8 <= outBits.size(); i += 8) {
        uint8_t b = 0;
        for (int bitIdx = 0; bitIdx < 8; ++bitIdx) {
            if (outBits[i + bitIdx]) {
                b |= (1 << (7 - bitIdx));
            }
        }
        outBytes.push_back(b);
    }
    return outBytes;
}

// ============================================================================
// ПОИСК ДОСТУПНЫХ COM-ПОРТОВ
// ============================================================================
std::vector<int> GetAvailableComPorts() {
    std::vector<int> ports;
    wchar_t targetPath[256];

    for (int i = 1; i <= 256; ++i) {
        std::wstring portName = L"COM" + std::to_wstring(i);
        if (QueryDosDeviceW(portName.c_str(), targetPath, 256) != 0) {
            ports.push_back(i);
        }
    }

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t valName[256];
        BYTE data[256];
        DWORD index = 0, nameLen = 256, dataSize = 256, type = 0;

        while (RegEnumValueW(hKey, index++, valName, &nameLen, NULL, &type, data, &dataSize) == ERROR_SUCCESS) {
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
            nameLen = 256;
            dataSize = 256;
        }
        RegCloseKey(hKey);
    }

    std::sort(ports.begin(), ports.end());
    return ports;
}

// ============================================================================
// РАБОТА С COM-ПОРТОМ
// ============================================================================
bool OpenAndConfigureSerial(int portNumber, BYTE stopBits) {
    CloseSerial();

    std::wstring portName = L"\\\\.\\COM" + std::to_wstring(portNumber);
    hSerial = CreateFileW(
        portName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL
    );

    if (hSerial == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::wstring msg = L"Не удалось открыть " + portName + L"\n";
        if (err == ERROR_FILE_NOT_FOUND) msg += L"Причина: Порт отсутствует в системе.";
        else if (err == ERROR_ACCESS_DENIED) msg += L"Причина: Порт уже занят другой программой.";
        else msg += L"Код ошибки: " + std::to_wstring(err);
        MessageBoxW(hMainWnd, msg.c_str(), L"Ошибка подключения", MB_ICONWARNING | MB_OK);
        return false;
    }

    SetupComm(hSerial, 4096, 4096);

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(hSerial, &dcb)) {
        CloseSerial();
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

    if (!SetCommState(hSerial, &dcb)) {
        CloseSerial();
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
    std::vector<uint8_t> rxBuffer;
    char tempBuf[128];
    DWORD bytesRead = 0;
    const std::string flagStr = FRAME_FLAG_STR;

    while (g_bRunning) {
        if (hSerial == INVALID_HANDLE_VALUE) {
            Sleep(50);
            continue;
        }

        BOOL success = ReadFile(hSerial, tempBuf, sizeof(tempBuf), &bytesRead, NULL);
        if (success && bytesRead > 0) {
            rxBuffer.insert(rxBuffer.end(), tempBuf, tempBuf + bytesRead);

            while (rxBuffer.size() >= FRAME_FLAG_LEN + 2) {
                auto it = std::search(rxBuffer.begin(), rxBuffer.end(), flagStr.begin(), flagStr.end());
                if (it == rxBuffer.end()) {
                    if (rxBuffer.size() > FRAME_FLAG_LEN) {
                        rxBuffer.erase(rxBuffer.begin(), rxBuffer.end() - FRAME_FLAG_LEN);
                    }
                    break;
                }

                if (it != rxBuffer.begin()) {
                    rxBuffer.erase(rxBuffer.begin(), it);
                }

                if (rxBuffer.size() < FRAME_FLAG_LEN + 2) break;

                uint16_t bodyLen = (uint8_t)rxBuffer[FRAME_FLAG_LEN] | ((uint8_t)rxBuffer[FRAME_FLAG_LEN + 1] << 8);
                size_t totalExpected = FRAME_FLAG_LEN + 2 + bodyLen;

                if (rxBuffer.size() < totalExpected) break;

                std::vector<uint8_t> stuffedBody(rxBuffer.begin() + FRAME_FLAG_LEN + 2, rxBuffer.begin() + totalExpected);
                rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + totalExpected);

                std::vector<uint8_t> rawBody = BitDestuff(stuffedBody);

                if (rawBody.size() >= 3 + 2 + 1 + RESERVED_BYTES_LEN) {
                    uint16_t dataLen = rawBody[3] | (rawBody[4] << 8);
                    if (rawBody.size() >= 5 + dataLen) {
                        std::string receivedText((char*)&rawBody[5], dataLen);
                        AppendTextToOutput(receivedText);
                    }
                }
            }
        }
        else {
            Sleep(10);
        }
    }
    return 0;
}

void AppendTextToOutput(const std::string& str) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.length(), NULL, 0);
    if (wlen <= 0) return;
    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.length(), &wstr[0], wlen);

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
// ВЫВОД В ОКНО СОСТОЯНИЯ (РАЗДЕЛЕНИЕ ПОЛЕЙ ПРОБЕЛАМИ, ЗНАЧЕНИЙ — ЗАПЯТЫМИ)
// ============================================================================
void AppendFormattedHexByte(uint8_t b, bool isModified, bool addComma) {
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_UNDERLINE | CFM_COLOR;
    cf.crTextColor = RGB(0, 0, 0);
    cf.dwEffects = isModified ? CFE_UNDERLINE : 0;

    std::wstringstream ss;
    ss << std::uppercase << std::hex << std::setw(2) << std::setfill(L'0') << (int)b << L"h";
    if (addComma) ss << L",";

    std::wstring token = ss.str();
    int textLen = GetWindowTextLengthW(hRichStatus);
    SendMessageW(hRichStatus, EM_SETSEL, (WPARAM)textLen, (LPARAM)textLen);
    SendMessageW(hRichStatus, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(hRichStatus, EM_REPLACESEL, FALSE, (LPARAM)token.c_str());
}

void AppendFieldSeparator() {
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_UNDERLINE;
    cf.dwEffects = 0;

    int textLen = GetWindowTextLengthW(hRichStatus);
    SendMessageW(hRichStatus, EM_SETSEL, (WPARAM)textLen, (LPARAM)textLen);
    SendMessageW(hRichStatus, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(hRichStatus, EM_REPLACESEL, FALSE, (LPARAM)L" ");
}

void PrintStatusHeaderOnce() {
    if (g_statusHeaderPrinted) return;
    g_statusHeaderPrinted = true;

    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BOLD | CFM_COLOR;
    cf.dwEffects = CFE_BOLD;
    cf.crTextColor = RGB(10, 60, 160);

    std::wstring header = L"Поля кадра: [ФЛАГ_НАЧАЛА] [ИСТОЧНИК] [НАЗНАЧЕНИЕ] [ТИП] [ДАННЫЕ] [FCS]\r\n"
        L"-----------------------------------------------------------------------------------------\r\n";
    SendMessageW(hRichStatus, EM_SETSEL, 0, 0);
    SendMessageW(hRichStatus, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(hRichStatus, EM_REPLACESEL, FALSE, (LPARAM)header.c_str());
}

void AppendStatusFormatted(const wchar_t* prefix, const std::vector<uint8_t>& frameBody, size_t dataLen, const std::vector<bool>& modifiedMask) {
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BOLD | CFM_COLOR;
    cf.dwEffects = CFE_BOLD;
    cf.crTextColor = RGB(30, 30, 30);

    int textLen = GetWindowTextLengthW(hRichStatus);
    SendMessageW(hRichStatus, EM_SETSEL, (WPARAM)textLen, (LPARAM)textLen);
    SendMessageW(hRichStatus, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(hRichStatus, EM_REPLACESEL, FALSE, (LPARAM)prefix);

    // 1. Поле Флага (8 байт: 45050103), байты разделяются запятыми
    const uint8_t flagBytes[] = { '4', '5', '0', '5', '0', '1', '0', '3' };
    for (int i = 0; i < 8; ++i) {
        AppendFormattedHexByte(flagBytes[i], false, (i < 7));
    }
    AppendFieldSeparator();

    // 2. Служебное поле 1: Адрес источника (1 байт)
    uint8_t src = (frameBody.size() > 0) ? frameBody[0] : 0;
    bool modSrc = (modifiedMask.size() > 0) ? modifiedMask[0] : false;
    AppendFormattedHexByte(src, modSrc, false);
    AppendFieldSeparator();

    // 3. Служебное поле 2: Адрес назначения (1 байт)
    uint8_t dst = (frameBody.size() > 1) ? frameBody[1] : 0;
    bool modDst = (modifiedMask.size() > 1) ? modifiedMask[1] : false;
    AppendFormattedHexByte(dst, modDst, false);
    AppendFieldSeparator();

    // 4. Служебное поле 3: Тип кадра (1 байт)
    uint8_t type = (frameBody.size() > 2) ? frameBody[2] : 0;
    bool modType = (modifiedMask.size() > 2) ? modifiedMask[2] : false;
    AppendFormattedHexByte(type, modType, false);
    AppendFieldSeparator();

    // 5. Поле данных: каждый байт отделяется запятой
    size_t dataOffset = 5;
    for (size_t i = 0; i < dataLen; ++i) {
        size_t idx = dataOffset + i;
        if (idx < frameBody.size()) {
            uint8_t b = frameBody[idx];
            bool m = (idx < modifiedMask.size()) ? modifiedMask[idx] : false;
            AppendFormattedHexByte(b, m, (i + 1 < dataLen)); // Запятая между всеми байтами данных
        }
    }
    AppendFieldSeparator();

    // 6. Служебное поле 4: Контрольная сумма FCS (1 байт)
    size_t fcsIdx = dataOffset + dataLen;
    uint8_t fcs = (fcsIdx < frameBody.size()) ? frameBody[fcsIdx] : 0;
    bool modFcs = (fcsIdx < modifiedMask.size()) ? modifiedMask[fcsIdx] : false;
    AppendFormattedHexByte(fcs, modFcs, false);

    // Перевод строки (зарезервированные 10 байт по заданию НЕ выводятся)
    SendMessageW(hRichStatus, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(hRichStatus, EM_SCROLLCARET, 0, 0);
}

// ============================================================================
// ПЕРЕДАЧА КАДРОВ
// ============================================================================
void SendFramedMessage(const std::string& text) {
    if (hSerial == INVALID_HANDLE_VALUE) {
        MessageBoxW(hMainWnd, L"Сначала выберите COM-порт в окне управления!", L"Предупреждение", MB_ICONWARNING | MB_OK);
        return;
    }

    PrintStatusHeaderOnce();

    size_t totalLen = text.length();
    size_t offset = 0;

    while (offset < totalLen || totalLen == 0) {
        size_t remaining = totalLen - offset;
        size_t chunkLen = (remaining < (size_t)MAX_PAYLOAD_SIZE) ? remaining : (size_t)MAX_PAYLOAD_SIZE;

        // Формирование сырого тела кадра до стаффинга
        std::vector<uint8_t> rawBody;
        rawBody.push_back(0x00); // SrcAddr = 0
        rawBody.push_back(0x00); // DestAddr = 0
        rawBody.push_back(0x00); // PacketType = 0
        rawBody.push_back((uint8_t)(chunkLen & 0xFF));        // Длина данных (мл)
        rawBody.push_back((uint8_t)((chunkLen >> 8) & 0xFF)); // Длина данных (ст)

        for (size_t i = 0; i < chunkLen; ++i) {
            rawBody.push_back((uint8_t)text[offset + i]);
        }

        rawBody.push_back(0x00); // FCS = 0

        // 10 зарезервированных байтов в конце кадра
        for (int i = 0; i < RESERVED_BYTES_LEN; ++i) {
            rawBody.push_back(0x00);
        }

        // Бит-стаффинг
        std::vector<uint8_t> stuffedBody;
        std::vector<bool> modifiedMask;
        BitStuff(rawBody, stuffedBody, modifiedMask);

        // Вывод кадров в окно состояния
        std::vector<bool> noModMask(rawBody.size(), false);
        AppendStatusFormatted(L"До стаффинга:    ", rawBody, chunkLen, noModMask);
        AppendStatusFormatted(L"После стаффинга: ", stuffedBody, chunkLen, modifiedMask);

        // Отправка кадра в порт
        DWORD written = 0;
        WriteFile(hSerial, FRAME_FLAG_STR, FRAME_FLAG_LEN, &written, NULL); // Флаг
        g_txCount += written;

        uint16_t encLen = (uint16_t)stuffedBody.size();
        WriteFile(hSerial, &encLen, 2, &written, NULL);                     // Длина
        g_txCount += written;

        WriteFile(hSerial, stuffedBody.data(), (DWORD)stuffedBody.size(), &written, NULL); // Тело
        g_txCount += written;

        offset += chunkLen;
        if (totalLen == 0) break;
    }
}

// Перехват клавиши Enter: отправка и обязательная очистка поля ввода
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CHAR && wParam == VK_RETURN) {
        int len = GetWindowTextLengthW(hWnd);
        if (len > 0) {
            std::wstring wbuf(len + 1, 0);
            GetWindowTextW(hWnd, &wbuf[0], len + 1);
            wbuf.resize(len);

            int bLen = WideCharToMultiByte(CP_ACP, 0, wbuf.c_str(), -1, NULL, 0, NULL, NULL);
            std::string text(bLen, 0);
            WideCharToMultiByte(CP_ACP, 0, wbuf.c_str(), -1, &text[0], bLen, NULL, NULL);
            text.pop_back();

            text += "\r\n";
            SendFramedMessage(text);

            // Очищаем поле ввода, предотвращая повторную отправку старого текста
            SetWindowTextW(hWnd, L"");
        }
        return 0;
    }
    return CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
}

// ============================================================================
// ГЛАВНОЕ ОКНО
// ============================================================================
LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // 1. Окно управления
        CreateWindowW(L"BUTTON", L"1. Окно управления (Параметры соединения)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            15, 10, 755, 75, hWnd, NULL, NULL, NULL);

        hComboPort = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            30, 38, 350, 200, hWnd, (HMENU)IDC_COMBO_PORT, NULL, NULL);

        std::vector<int> ports = GetAvailableComPorts();
        if (ports.empty()) {
            SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)L"Порты в системе не найдены");
            SendMessageW(hComboPort, CB_SETITEMDATA, 0, (LPARAM)0);
            SendMessageW(hComboPort, CB_SETCURSEL, 0, 0);
            EnableWindow(hComboPort, FALSE);
        }
        else {
            SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)L"-- Выберите доступный COM-порт --");
            SendMessageW(hComboPort, CB_SETITEMDATA, 0, (LPARAM)0);
            for (int p : ports) {
                std::wstring name = L"COM-порт: COM" + std::to_wstring(p);
                int idx = (int)SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)name.c_str());
                SendMessageW(hComboPort, CB_SETITEMDATA, idx, (LPARAM)p);
            }
            SendMessageW(hComboPort, CB_SETCURSEL, 0, 0);
        }

        hComboStopBits = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            400, 38, 350, 200, hWnd, (HMENU)IDC_COMBO_STOPBITS, NULL, NULL);
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 1 стоп-бит");
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 2 стоп-бита");
        SendMessageW(hComboStopBits, CB_SETCURSEL, 0, 0);

        // 2. Окно состояния
        CreateWindowW(L"BUTTON", L"2. Окно состояния (Кадры: До / После бит-стаффинга)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            15, 95, 755, 160, hWnd, NULL, NULL, NULL);

        hRichStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit20W", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            30, 120, 725, 120, hWnd, (HMENU)IDC_RICH_STATUS, NULL, NULL);
        SendMessageW(hRichStatus, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(252, 252, 252));

        // 3. Окно ввода сообщений
        CreateWindowW(L"BUTTON", L"3. Окно ввода сообщений (Нажмите Enter для отправки кадра)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            15, 265, 755, 160, hWnd, NULL, NULL, NULL);

        hEditInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
            30, 290, 725, 120, hWnd, (HMENU)IDC_EDIT_INPUT, NULL, NULL);
        origEditProc = (WNDPROC)SetWindowLongPtrW(hEditInput, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        // 4. Окно вывода сообщений
        CreateWindowW(L"BUTTON", L"4. Окно вывода сообщений (Принятые данные)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            15, 435, 755, 160, hWnd, NULL, NULL, NULL);

        hEditOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            30, 460, 725, 120, hWnd, (HMENU)IDC_EDIT_OUTPUT, NULL, NULL);

        SetFocus(hEditInput);
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
                    OpenAndConfigureSerial(portNumber, GetSelectedStopBits());
                    PrintStatusHeaderOnce();
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

// ============================================================================
// ТОЧКА ВХОДА WinMain
// ============================================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    LoadLibraryW(L"riched20.dll");

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"OSKS_Lab2_Var3_SingleWindow";

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    hMainWnd = CreateWindowExW(
        0, CLASS_NAME,
        L"ОСКС. Лабораторная работа №2 — Вариант 3 (Пакетная передача данных)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 650,
        NULL, NULL, hInstance, NULL
    );

    if (!hMainWnd) return 0;

    ShowWindow(hMainWnd, nCmdShow);
    UpdateWindow(hMainWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}