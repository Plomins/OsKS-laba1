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

// Подключение современных визуальных стилей Windows
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Идентификаторы элементов управления
#define IDC_COMBO_PORT      101
#define IDC_COMBO_STOPBITS  102
#define IDC_RICH_STATUS     103
#define IDC_EDIT_INPUT      104
#define IDC_EDIT_OUTPUT     105

// Параметры кадра для Варианта 3
#define MAX_FRAME_SIZE      300         // Максимальная длина кадра по ТЗ (300 байтов)
#define FRAME_FLAG_STR      "45050103"  // 8 байтов: 45050 + 1 + 0 + 3
#define FRAME_FLAG_LEN      8
#define RESERVED_BYTES_LEN  10          // 10 зарезервированных байтов в конце кадра
#define SERVICE_FIELDS_LEN  4           // SrcAddr(1) + DestAddr(1) + Type(1) + FCS(1)

// Максимальный объем полезных данных в одном кадре
#define MAX_PAYLOAD_SIZE    (MAX_FRAME_SIZE - FRAME_FLAG_LEN - SERVICE_FIELDS_LEN - 2 - RESERVED_BYTES_LEN)

// Дескрипторы окон и рамок-групп (Сетка 2х2)
HWND hWndMain      = NULL;
HWND hGroupControl = NULL;
HWND hGroupStatus  = NULL;
HWND hGroupInput   = NULL;
HWND hGroupOutput  = NULL;

// Дескрипторы элементов интерфейса
HWND hComboPort     = NULL;
HWND hComboStopBits = NULL;
HWND hRichStatus    = NULL;
HWND hEditInput     = NULL;
HWND hEditOutput    = NULL;

WNDPROC origEditProc = NULL;
HANDLE hSerial = INVALID_HANDLE_VALUE;
HANDLE hReadThread = NULL;

std::atomic<bool> g_bRunning(false);
std::atomic<unsigned long long> g_txCount(0);
bool g_portLocked = false;
int g_lastSentPos = 0; // Граница уже отправленного текста в окне ввода

// Прототипы функций
bool OpenAndConfigureSerial(int portNumber, BYTE stopBits);
void CloseSerial();
DWORD WINAPI SerialReadThread(LPVOID lpParam);
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void AppendTextToOutput(const std::string& str);
BYTE GetSelectedStopBits();
std::vector<int> GetAvailableComPorts();
void SendFramedMessage(const std::string& text);
void ResetStatusWindow();
void DisplayLastFrameStatus(const std::vector<uint8_t>& rawBody, size_t chunkLen, const std::vector<uint8_t>& stuffedBody, const std::vector<bool>& modifiedMask);

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
        MessageBoxW(hWndMain, msg.c_str(), L"Ошибка подключения", MB_ICONWARNING | MB_OK);
        return false;
    }

    SetupComm(hSerial, 4096, 4096);

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(hSerial, &dcb)) {
        CloseSerial();
        MessageBoxW(hWndMain, L"Не удалось получить параметры DCB!", L"Ошибка", MB_ICONERROR | MB_OK);
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
        MessageBoxW(hWndMain, L"Ошибка SetCommState!", L"Ошибка", MB_ICONERROR | MB_OK);
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
    g_lastSentPos = 0; // Сброс границы отправки при новом подключении
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
    if (idx == 1) return ONESTOPBIT;
    if (idx == 2) return TWOSTOPBITS;
    return 0xFF; // Не выбрано
}

// ============================================================================
// ВЫВОД В ОКНО СОСТОЯНИЯ (RICH EDIT)
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

void ResetStatusWindow() {
    SetWindowTextW(hRichStatus, L"");

    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BOLD | CFM_COLOR;
    cf.dwEffects = CFE_BOLD;
    cf.crTextColor = RGB(10, 60, 160);

    std::wstring header = L"Поля кадра: [ФЛАГ] [ИСТОЧНИК] [НАЗНАЧЕНИЕ] [ТИП] [ДАННЫЕ] [FCS]\r\n"
        L"-----------------------------------------------------------------------------------------\r\n";
    SendMessageW(hRichStatus, EM_SETSEL, 0, -1);
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

    // 1. Поле Флага (8 байт: FRAME_FLAG_STR), разделение запятыми
    for (size_t i = 0; i < FRAME_FLAG_LEN; ++i) {
        AppendFormattedHexByte((uint8_t)FRAME_FLAG_STR[i], false, (i + 1 < FRAME_FLAG_LEN));
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
            AppendFormattedHexByte(b, m, (i + 1 < dataLen));
        }
    }
    AppendFieldSeparator();

    // 6. Служебное поле 4: Контрольная сумма FCS (1 байт)
    size_t fcsIdx = dataOffset + dataLen;
    uint8_t fcs = (fcsIdx < frameBody.size()) ? frameBody[fcsIdx] : 0;
    bool modFcs = (fcsIdx < modifiedMask.size()) ? modifiedMask[fcsIdx] : false;
    AppendFormattedHexByte(fcs, modFcs, false);

    // 10 зарезервированных байтов по заданию НЕ выводятся
    SendMessageW(hRichStatus, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(hRichStatus, EM_SCROLLCARET, 0, 0);
}

void DisplayLastFrameStatus(const std::vector<uint8_t>& rawBody, size_t chunkLen, const std::vector<uint8_t>& stuffedBody, const std::vector<bool>& modifiedMask) {
    ResetStatusWindow(); // Перезаписываем статус, чтобы отображался СТРОГО ПОСЛЕДНИЙ переданный кадр
    std::vector<bool> noModMask(rawBody.size(), false);
    AppendStatusFormatted(L"До стаффинга:    ", rawBody, chunkLen, noModMask);
    AppendStatusFormatted(L"После стаффинга: ", stuffedBody, chunkLen, modifiedMask);
}

// ============================================================================
// ПЕРЕДАЧА КАДРОВ
// ============================================================================
void SendFramedMessage(const std::string& text) {
    if (hSerial == INVALID_HANDLE_VALUE) {
        MessageBoxW(hWndMain, L"Сначала выберите параметры COM-порта в окне управления!", L"Предупреждение", MB_ICONWARNING | MB_OK);
        return;
    }

    size_t totalLen = text.length();
    size_t offset = 0;

    while (offset < totalLen || totalLen == 0) {
        size_t remaining = totalLen - offset;
        size_t chunkLen = (remaining < (size_t)MAX_PAYLOAD_SIZE) ? remaining : (size_t)MAX_PAYLOAD_SIZE;

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

        // Отображение только последнего переданного кадра
        DisplayLastFrameStatus(rawBody, chunkLen, stuffedBody, modifiedMask);

        // Отправка кадра в порт
        DWORD written = 0;
        WriteFile(hSerial, FRAME_FLAG_STR, FRAME_FLAG_LEN, &written, NULL);
        g_txCount += written;

        uint16_t encLen = (uint16_t)stuffedBody.size();
        WriteFile(hSerial, &encLen, 2, &written, NULL);
        g_txCount += written;

        WriteFile(hSerial, stuffedBody.data(), (DWORD)stuffedBody.size(), &written, NULL);
        g_txCount += written;

        offset += chunkLen;
        if (totalLen == 0) break;
    }
}

// ============================================================================
// ОКНО ВВОДА: РЕЖИМ ТЕРМИНАЛА (БЕЗ УДАЛЕНИЯ ИСТОРИИ ВВОДА)
// ============================================================================
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // 1. Блокируем стирание (Backspace/Delete) уже отправленного текста
    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_BACK || wParam == VK_DELETE) {
            DWORD start = 0, end = 0;
            SendMessageW(hWnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
            if (start <= (DWORD)g_lastSentPos || end <= (DWORD)g_lastSentPos) {
                return 0; // Запрещено стирать отправленное
            }
        }
    }

    // 2. Блокируем вырезание (Ctrl+X) уже отправленного текста
    if (uMsg == WM_CUT || uMsg == WM_CLEAR) {
        DWORD start = 0, end = 0;
        SendMessageW(hWnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        if (start < (DWORD)g_lastSentPos) {
            return 0;
        }
    }

    // 3. Обработка ввода и нажатия клавиши Enter
    if (uMsg == WM_CHAR) {
        // Если порт ещё не открыт, не разрешаем печатать
        if (hSerial == INVALID_HANDLE_VALUE) {
            return 0;
        }

        // Если каретка оказалась в области старого текста, переносим её в конец
        DWORD start = 0, end = 0;
        SendMessageW(hWnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        if (start < (DWORD)g_lastSentPos) {
            int totalLen = GetWindowTextLengthW(hWnd);
            SendMessageW(hWnd, EM_SETSEL, (WPARAM)totalLen, (LPARAM)totalLen);
        }

        // Отправка по Enter
        if (wParam == VK_RETURN) {
            // Вставляем перевод строки \r\n в окне ввода
            LRESULT res = CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);

            int totalLen = GetWindowTextLengthW(hWnd);
            if (totalLen > g_lastSentPos) {
                std::wstring fullText(totalLen + 1, 0);
                GetWindowTextW(hWnd, &fullText[0], totalLen + 1);
                fullText.resize(totalLen);

                // Забираем ровно ту строку, которую ввели
                std::wstring newText = fullText.substr(g_lastSentPos);

                int bLen = WideCharToMultiByte(CP_ACP, 0, newText.c_str(), -1, NULL, 0, NULL, NULL);
                if (bLen > 1) {
                    std::string sendStr(bLen, 0);
                    WideCharToMultiByte(CP_ACP, 0, newText.c_str(), -1, &sendStr[0], bLen, NULL, NULL);
                    sendStr.pop_back(); // Убираем null-терминатор

                    SendFramedMessage(sendStr);
                }

                // Сдвигаем границу отправки вперед
                g_lastSentPos = totalLen;
            }

            SendMessageW(hWnd, EM_SCROLLCARET, 0, 0);
            return res;
        }
    }

    return CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
}

// ============================================================================
// ГЛАВНОЕ ОКНО ПРИЛОЖЕНИЯ (СЕТКА 2х2)
// ============================================================================
LRESULT CALLBACK WndProcMain(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // 1. Окно управления
        hGroupControl = CreateWindowW(L"BUTTON", L"Окно управления (Параметры COM-порта)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupControl, WM_SETFONT, (WPARAM)hFont, TRUE);

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

        hComboStopBits = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_COMBO_STOPBITS, NULL, NULL);
        SendMessageW(hComboStopBits, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"-- Выберите количество стоп-битов --");
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 1 стоп-бит");
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 2 стоп-бита");
        SendMessageW(hComboStopBits, CB_SETCURSEL, 0, 0);

        // 2. Окно состояния (Кадры)
        hGroupStatus = CreateWindowW(L"BUTTON", L"Окно состояния (Кадры)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        hRichStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit20W", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_RICH_STATUS, NULL, NULL);
        SendMessageW(hRichStatus, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hRichStatus, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(253, 253, 253));

        // Вывод заголовка структуры полей
        ResetStatusWindow();

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
        int topH = 140; // Высота верхнего ряда
        int botH = totalH - topH - (margin * 2) - gap;
        int colW = (totalW - (margin * 2) - gap) / 2;

        if (colW > 120 && botH > 60) {
            // Верхний левый блок: Управление
            MoveWindow(hGroupControl, margin, margin, colW, topH, TRUE);
            int comboW = (colW - 36) / 2;
            MoveWindow(hComboPort, margin + 12, margin + 50, comboW, 250, TRUE);
            MoveWindow(hComboStopBits, margin + 24 + comboW, margin + 50, comboW, 250, TRUE);

            // Верхний правый блок: Состояние (Кадры)
            MoveWindow(hGroupStatus, margin + colW + gap, margin, colW, topH, TRUE);
            MoveWindow(hRichStatus, margin + colW + gap + 12, margin + 22, colW - 24, topH - 34, TRUE);

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
                        ResetStatusWindow();
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

// ============================================================================
// ТОЧКА ВХОДА WinMain
// ============================================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Обязательная загрузка DLL библиотеки RichEdit
    LoadLibraryW(L"riched20.dll");

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

    hWndMain = CreateWindowExW(0, CLASS_NAME,
        L"ОсКС — Лабораторная работа №2 (Пакетная передача данных, Вариант 3)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 600,
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