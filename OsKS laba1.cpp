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

// Подключение современных стилей оформления Windows
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Идентификаторы элементов интерфейса
#define IDC_COMBO_PORT      101
#define IDC_COMBO_STOPBITS  102
#define IDC_STATIC_STATUS   103
#define IDC_RICH_STATUS     104
#define IDC_EDIT_INPUT      105
#define IDC_EDIT_OUTPUT     106

#define IDT_STATUS_TIMER    201

// ============================================================================
// ПАРАМЕТРЫ СТРУКТУРЫ КАДРА (СТРОГО 300 БАЙТ, СЛУЖЕБНЫЕ ПОЛЯ: ADDR, TYPE, FCS)
// ============================================================================  


// Структура кадра со служебными полями Addr, Type и FCS
#pragma pack(push, 1)
struct DataFrame {
    char    flag[ 8];           // Флаг начала кадра ("450501-3")[cite: 1, 2]
    uint8_t addr;                           // Служебное поле 1: Адрес узла[cite: 1]
    uint8_t packetType;                     // Служебное поле 2: Тип пакета[cite: 1]
    uint8_t data[279];         // Данные + Padding (279 байт)[cite: 1]
    uint8_t fcs;                            // Служебное поле 3: Контрольная сумма FCS[cite: 1]
    uint8_t reserved[10];   // 10 зарезервированных байтов[cite: 1]
};
#pragma pack(pop)

// Дескрипторы окон интерфейса (сетка 2х2)
HWND hWndMain = NULL;
HWND hGroupControl = NULL;
HWND hGroupStatus = NULL;
HWND hGroupInput = NULL;
HWND hGroupOutput = NULL;

HWND hComboPort = NULL;
HWND hComboStopBits = NULL;
HWND hStaticStatus = NULL;
HWND hRichStatus = NULL;
HWND hEditInput = NULL;
HWND hEditOutput = NULL;
WNDPROC origEditProc = NULL;

HANDLE hSerial = INVALID_HANDLE_VALUE;
HANDLE hReadThread = NULL;

std::atomic<bool> g_bRunning(false);
std::atomic<unsigned long long> g_txCount(0);    // Счётчик переданных байт (строго кратно 300)
std::atomic<unsigned long long> g_frameCount(0); // Счётчик отправленных кадров
bool g_portLocked = false;
int  g_unsentStartPos = 0;

// Прототипы функций
bool OpenAndConfigureSerial(int portNumber, BYTE stopBits);
void CloseSerial();
DWORD WINAPI SerialReadThread(LPVOID lpParam);
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void CheckAndAutoSend(HWND hWnd);
void AppendTextToOutput(const std::string& str);
BYTE GetSelectedStopBits();
std::vector<int> GetAvailableComPorts();
void SendFramedMessage(const std::string& text);
void PrintStatusHeaderOnce();
void AppendStatusFormatted(const wchar_t* prefix, const std::vector<uint8_t>& frameBody, size_t dataLen, const std::vector<bool>& modifiedMask);

// ============================================================================
// БИТ-СТАФФИНГ ДЛЯ ЗАГОЛОВКА "450501-3" (63 БИТА)
// ============================================================================
const bool g_pattern63[63] = {
    // '4' (0x34) = 00110100b
    0, 0, 1, 1, 0, 1, 0, 0,
    // '5' (0x35) = 00110101b
    0, 0, 1, 1, 0, 1, 0, 1,
    // '0' (0x30) = 00110000b
    0, 0, 1, 1, 0, 0, 0, 0,
    // '5' (0x35) = 00110101b
    0, 0, 1, 1, 0, 1, 0, 1,
    // '0' (0x30) = 00110000b
    0, 0, 1, 1, 0, 0, 0, 0,
    // '1' (0x31) = 00110001b
    0, 0, 1, 1, 0, 0, 0, 1,
    // '-' (0x2D) = 00101101b
    0, 0, 1, 0, 1, 1, 0, 1,
    // '3' (0x33, первые 7 бит) = 0011001b (8-й бит равен 1)
    0, 0, 1, 1, 0, 0, 1
};

void BitStuff(const std::vector<uint8_t>& inData, std::vector<uint8_t>& outData, std::vector<bool>& modifiedBytes) {
    outData.clear();
    modifiedBytes.clear();

    std::vector<bool> inBits;
    for (uint8_t b : inData) {
        for (int i = 7; i >= 0; --i) inBits.push_back((b >> i) & 1);
    }

    std::vector<bool> outBits;
    std::vector<bool> bitModifiedMask;

    size_t i = 0;
    while (i < inBits.size()) {
        bool match = false;
        if (i + 63 <= inBits.size()) {
            match = true;
            for (int k = 0; k < 63; ++k) {
                if (inBits[i + k] != g_pattern63[k]) {
                    match = false;
                    break;
                }
            }
        }

        if (match) {
            for (int k = 0; k < 63; ++k) {
                outBits.push_back(inBits[i + k]);
                bitModifiedMask.push_back(false);
            }
            // Вставка бита '0' разрушает 64-й бит '1' символа '3'
            outBits.push_back(false);
            bitModifiedMask.push_back(true);
            i += 63;
        }
        else {
            outBits.push_back(inBits[i]);
            bitModifiedMask.push_back(false);
            i++;
        }
    }

    while (outBits.size() % 8 != 0) {
        outBits.push_back(false);
        bitModifiedMask.push_back(false);
    }

    for (size_t byteIdx = 0; byteIdx < outBits.size(); byteIdx += 8) {
        uint8_t b = 0;
        bool byteMod = false;
        for (int bitIdx = 0; bitIdx < 8; ++bitIdx) {
            if (outBits[byteIdx + bitIdx]) b |= (1 << (7 - bitIdx));
            if (bitModifiedMask[byteIdx + bitIdx]) byteMod = true;
        }
        outData.push_back(b);
        modifiedBytes.push_back(byteMod);
    }
}

std::vector<uint8_t> BitDestuff(const std::vector<uint8_t>& inData) {
    std::vector<bool> inBits;
    for (uint8_t b : inData) {
        for (int i = 7; i >= 0; --i) inBits.push_back((b >> i) & 1);
    }

    std::vector<bool> outBits;
    size_t i = 0;
    while (i < inBits.size()) {
        bool match = false;
        if (i + 63 <= inBits.size()) {
            match = true;
            for (int k = 0; k < 63; ++k) {
                if (inBits[i + k] != g_pattern63[k]) {
                    match = false;
                    break;
                }
            }
        }

        if (match) {
            for (int k = 0; k < 63; ++k) outBits.push_back(inBits[i + k]);
            i += 63;
            if (i < inBits.size()) i++;
        }
        else {
            outBits.push_back(inBits[i]);
            i++;
        }
    }

    std::vector<uint8_t> outBytes;
    for (size_t byteIdx = 0; byteIdx + 8 <= outBits.size(); byteIdx += 8) {
        uint8_t b = 0;
        for (int bitIdx = 0; bitIdx < 8; ++bitIdx) {
            if (outBits[byteIdx + bitIdx]) b |= (1 << (7 - bitIdx));
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
            nameLen = 256; dataSize = 256;
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
    hSerial = CreateFileW(portName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

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

// Приём входящих пакетов с проверкой FCS
DWORD WINAPI SerialReadThread(LPVOID lpParam) {
    std::vector<uint8_t> rxBuffer;
    char tempBuf[128];
    DWORD bytesRead = 0;
    const std::string flagStr = "450501-3"  ;

    while (g_bRunning) {
        if (hSerial == INVALID_HANDLE_VALUE) {
            Sleep(50);
            continue;
        }

        BOOL success = ReadFile(hSerial, tempBuf, sizeof(tempBuf), &bytesRead, NULL);
        if (success && bytesRead > 0) {
            rxBuffer.insert(rxBuffer.end(), tempBuf, tempBuf + bytesRead);

            // Ищем кадр строго фиксированного размера 300 байт
            while (rxBuffer.size() >= 300) {
                auto it = std::search(rxBuffer.begin(), rxBuffer.end(), flagStr.begin(), flagStr.end());
                if (it == rxBuffer.end()) {
                    if (rxBuffer.size() >  8) {
                        rxBuffer.erase(rxBuffer.begin(), rxBuffer.end() -  8);
                    }
                    break;
                }

                if (it != rxBuffer.begin()) rxBuffer.erase(rxBuffer.begin(), it);
                if (rxBuffer.size() < 300) break;

                std::vector<uint8_t> stuffedBody(rxBuffer.begin() +  8, rxBuffer.begin() + 300);
                rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + 300);

                // Дестаффинг
                std::vector<uint8_t> rawBody = BitDestuff(stuffedBody);

                // Структура тела: [Addr: 0] [Type: 1] [Data: 2..280] [FCS: 281] [Reserved: 10]
                if (rawBody.size() >= 282 ) {
                    size_t fcsIdx = 281;
                    uint8_t receivedFcs = rawBody[fcsIdx];
                    const char* pData = (const char*)&rawBody[2];

                    // Считываем полезный текст до первого нулевого байта паддинга
                    size_t textLen = 0;
                    while (textLen < 279&& pData[textLen] != '\0') {
                        textLen++;
                    }

                    // Проверяем контрольную сумму FCS (XOR по полезным символам)
                    uint8_t calculatedFcs = 0;
                    for (size_t i = 0; i < textLen; ++i) {
                        calculatedFcs ^= (uint8_t)pData[i];
                    }

                    if (textLen > 0 && calculatedFcs == receivedFcs) {
                        std::string receivedText(pData, textLen);
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
// ВЫВОД В ОКНО СОСТОЯНИЯ (RICHEDIT)
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
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BOLD | CFM_COLOR;
    cf.dwEffects = CFE_BOLD;
    cf.crTextColor = RGB(10, 60, 160);

    // Строго 3 служебных поля кадра: Адрес, Тип и FCS в конце
    std::wstring header = L"Поля: [Заголовок] [Адрес] [Тип] [Данные] [FCS]\r\n"
        L"-----------------------------------------------------------------\r\n";
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

    // 1. Флаг начала (8 байт: "450501-3")[cite: 1, 2]
    const uint8_t flagBytes[] = { '4', '5', '0', '5', '0', '1', '-', '3' };
    for (int i = 0; i < 8; ++i) {
        AppendFormattedHexByte(flagBytes[i], false, (i < 7));
    }
    AppendFieldSeparator();

    // 2. Служебный флаг 1: Адрес (Addr)[cite: 1]
    uint8_t addr = (frameBody.size() > 0) ? frameBody[0] : 0;
    bool modAddr = (modifiedMask.size() > 0) ? modifiedMask[0] : false;
    AppendFormattedHexByte(addr, modAddr, false);
    AppendFieldSeparator();

    // 3. Служебный флаг 2: Тип кадра (PacketType)[cite: 1]
    uint8_t type = (frameBody.size() > 1) ? frameBody[1] : 0;
    bool modType = (modifiedMask.size() > 1) ? modifiedMask[1] : false;
    AppendFormattedHexByte(type, modType, false);
    AppendFieldSeparator();

    // 4. Поле данных (фактически набранный текст, модифицированные байты подчёркиваются)
    size_t dataOffset = 2; // Addr(1) + PacketType(1) = 2
    for (size_t i = 0; i < dataLen; ++i) {
        size_t idx = dataOffset + i;
        if (idx < frameBody.size()) {
            uint8_t b = frameBody[idx];
            bool m = (idx < modifiedMask.size()) ? modifiedMask[idx] : false;
            AppendFormattedHexByte(b, m, (i + 1 < dataLen));
        }
    }
    AppendFieldSeparator();

    // 5. Служебный флаг 3: Контрольная сумма FCS (находится перед 10 байтами резерва)[cite: 1]
    size_t fcsIdx = 281;
    uint8_t fcsVal = (fcsIdx < frameBody.size()) ? frameBody[fcsIdx] : 0;
    bool modFcs = (fcsIdx < modifiedMask.size()) ? modifiedMask[fcsIdx] : false;
    AppendFormattedHexByte(fcsVal, modFcs, false);

    // 10 зарезервированных байтов в конце кадра НЕ выводятся в RichEdit[cite: 2]
    SendMessageW(hRichStatus, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(hRichStatus, WM_HSCROLL, SB_LEFT, 0);
}

// ============================================================================
// ПАКЕТНАЯ ПЕРЕДАЧА СТРОГО ПО 300 БАЙТ
// ============================================================================
void SendFramedMessage(const std::string& text) {
    if (hSerial == INVALID_HANDLE_VALUE) return;

    size_t totalLen = text.length();
    size_t offset = 0;

    while (offset < totalLen || totalLen == 0) {
        size_t remaining = totalLen - offset;
        size_t chunkLen = (remaining < (size_t)279) ? remaining : (size_t)279;

        // Расчёт контрольной суммы FCS (побайтовый XOR полезного текста)
        uint8_t fcsCalc = 0;
        for (size_t i = 0; i < chunkLen; ++i) {
            fcsCalc ^= (uint8_t)text[offset + i];
        }

        // Тело кадра (292 байта): Addr(1) + Type(1) + Data(279) + FCS(1) + Reserved(10)[cite: 1]
        std::vector<uint8_t> rawBody;
        rawBody.push_back(0x00); // Флаг 1: Addr = 00h[cite: 1]
        rawBody.push_back(0x01); // Флаг 2: PacketType = 01h (Информационный кадр)[cite: 1]

        // Полезные данные
        for (size_t i = 0; i < chunkLen; ++i) {
            rawBody.push_back((uint8_t)text[offset + i]);
        }

        // Автодополнение нулями (Padding) до 279 байт
        for (size_t i = chunkLen; i < (size_t)279; ++i) {
            rawBody.push_back(0x00);
        }

        // Контрольная сумма FCS (1 байт)[cite: 1]
        rawBody.push_back(fcsCalc);

        // 10 зарезервированных байтов[cite: 1]
        for (int i = 0; i < 10; ++i) {
            rawBody.push_back(0x00);
        }

        // Бит-стаффинг
        std::vector<uint8_t> stuffedBody;
        std::vector<bool> modifiedMask;
        BitStuff(rawBody, stuffedBody, modifiedMask);

        // Фиксация тела кадра ровно на 292 байта
        if (stuffedBody.size() < 292) {
            stuffedBody.resize(292, 0x00);
            modifiedMask.resize(292, false);
        }
        else if (stuffedBody.size() > 292) {
            stuffedBody.resize(292);
            modifiedMask.resize(292);
        }

        // Вывод последнего кадра в RichEdit
        SetWindowTextW(hRichStatus, L"");
        PrintStatusHeaderOnce();

        std::vector<bool> noModMask(rawBody.size(), false);
        AppendStatusFormatted(L"До стаффинга:    ", rawBody, chunkLen, noModMask);
        AppendStatusFormatted(L"После стаффинга: ", stuffedBody, chunkLen, modifiedMask);

        // Полный кадр: Флаг (8 байт) + Закодированное тело (292 байта) = РОВНО 300 БАЙТ[cite: 1]
        std::vector<uint8_t> fullPacket;
        fullPacket.insert(fullPacket.end(), 450501-3, 450501-3 +  8);
        fullPacket.insert(fullPacket.end(), stuffedBody.begin(), stuffedBody.end());

        // Отправка в COM-порт
        DWORD written = 0;
        WriteFile(hSerial, fullPacket.data(), (DWORD)fullPacket.size(), &written, NULL);

        // Счётчик всегда увеличивается строго на 300 байт[cite: 1]
        g_txCount += 300;
        g_frameCount++;

        offset += chunkLen;
        if (totalLen == 0) break;
    }
}

// Автоотправка при наборе 279 полезных байт
void CheckAndAutoSend(HWND hWnd) {
    if (hSerial == INVALID_HANDLE_VALUE) return;

    int totalLen = GetWindowTextLengthW(hWnd);
    if (totalLen < g_unsentStartPos) {
        g_unsentStartPos = totalLen;
        return;
    }

    int unsentWLen = totalLen - g_unsentStartPos;
    if (unsentWLen <= 0) return;

    std::vector<wchar_t> wbuf(totalLen + 1, 0);
    GetWindowTextW(hWnd, wbuf.data(), totalLen + 1);

    const wchar_t* pUnsent = wbuf.data() + g_unsentStartPos;
    int bLen = WideCharToMultiByte(CP_ACP, 0, pUnsent, unsentWLen, NULL, 0, NULL, NULL);
    if (bLen < 279) return;

    std::string unsentStr(bLen, 0);
    WideCharToMultiByte(CP_ACP, 0, pUnsent, unsentWLen, &unsentStr[0], bLen, NULL, NULL);

    while (unsentStr.length() >= 279) {
        std::string chunk = unsentStr.substr(0, 279);
        SendFramedMessage(chunk);

        int chunkWLen = MultiByteToWideChar(CP_ACP, 0, chunk.c_str(), (int)chunk.length(), NULL, 0);
        g_unsentStartPos += chunkWLen;

        unsentStr = unsentStr.substr(279);
    }
}

// Перехват ввода: Enter выполняет только перенос строки, отправка — автоматически
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CHAR && wParam == VK_RETURN) {
        CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
        return 0; // Запрет отправки по Enter
    }
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        return CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
    }

    LRESULT res = CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);

    if (uMsg == WM_CHAR || uMsg == WM_PASTE || (uMsg == WM_KEYDOWN && wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))) {
        CheckAndAutoSend(hWnd);
    }

    return res;
}

// ============================================================================
// ГЛАВНОЕ ОКНО ПРИЛОЖЕНИЯ
// ============================================================================
LRESULT CALLBACK WndProcMain(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // 1. Окно управления
        hGroupControl = CreateWindowW(L"BUTTON", L"1. Окно управления (Параметры COM-порта)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupControl, WM_SETFONT, (WPARAM)hFont, TRUE);

        hComboPort = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_COMBO_PORT, NULL, NULL);
        SendMessageW(hComboPort, WM_SETFONT, (WPARAM)hFont, TRUE);

        std::vector<int> ports = GetAvailableComPorts();
        if (ports.empty()) {
            SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)L"COM-порты не найдены");
            SendMessageW(hComboPort, CB_SETITEMDATA, 0, 0);
            SendMessageW(hComboPort, CB_SETCURSEL, 0, 0);
            EnableWindow(hComboPort, FALSE);
        }
        else {
            SendMessageW(hComboPort, CB_ADDSTRING, 0, (LPARAM)L"-- Выберите COM-порт --");
            SendMessageW(hComboPort, CB_SETITEMDATA, 0, 0);
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
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 1 стоп-бит");
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 2 стоп-бита");
        SendMessageW(hComboStopBits, CB_SETCURSEL, 0, 0);

        // 2. Окно состояния
        hGroupStatus = CreateWindowW(L"BUTTON", L"2. Окно состояния (Кадры и статистика)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        hStaticStatus = CreateWindowW(L"STATIC", L"Статус: Выберите COM-порт в окне управления...",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_STATUS, NULL, NULL);
        SendMessageW(hStaticStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        hRichStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit20W", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
            0, 0, 0, 0, hWnd, (HMENU)IDC_RICH_STATUS, NULL, NULL);
        SendMessageW(hRichStatus, EM_SETTARGETDEVICE, 0, 0);
        SendMessageW(hRichStatus, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(252, 252, 252));

        SetTimer(hWnd, IDT_STATUS_TIMER, 200, NULL);

        // 3. Окно ввода сообщений
        hGroupInput = CreateWindowW(L"BUTTON", L"3. Окно ввода сообщений",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupInput, WM_SETFONT, (WPARAM)hFont, TRUE);

        hEditInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_INPUT, NULL, NULL);
        SendMessageW(hEditInput, WM_SETFONT, (WPARAM)hFont, TRUE);
        origEditProc = (WNDPROC)SetWindowLongPtrW(hEditInput, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        // 4. Окно вывода сообщений
        hGroupOutput = CreateWindowW(L"BUTTON", L"4. Окно вывода сообщений (Принятые данные)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupOutput, WM_SETFONT, (WPARAM)hFont, TRUE);

        hEditOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_OUTPUT, NULL, NULL);
        SendMessageW(hEditOutput, WM_SETFONT, (WPARAM)hFont, TRUE);

        SetFocus(hEditInput);
        break;
    }

    case WM_SIZE: {
        int totalW = LOWORD(lParam);
        int totalH = HIWORD(lParam);
        int margin = 12;
        int gap = 12;
        int topH = 175;
        int botH = totalH - topH - (margin * 2) - gap;
        int colW = (totalW - (margin * 2) - gap) / 2;

        if (colW > 150 && botH > 60) {
            MoveWindow(hGroupControl, margin, margin, colW, topH, TRUE);
            int comboW = (colW - 36) / 2;
            MoveWindow(hComboPort, margin + 12, margin + 50, comboW, 250, TRUE);
            MoveWindow(hComboStopBits, margin + 24 + comboW, margin + 50, comboW, 250, TRUE);

            MoveWindow(hGroupStatus, margin + colW + gap, margin, colW, topH, TRUE);
            MoveWindow(hStaticStatus, margin + colW + gap + 15, margin + 22, colW - 30, 18, TRUE);
            MoveWindow(hRichStatus, margin + colW + gap + 10, margin + 44, colW - 20, topH - 54, TRUE);

            MoveWindow(hGroupInput, margin, margin + topH + gap, colW, botH, TRUE);
            MoveWindow(hEditInput, margin + 10, margin + topH + gap + 25, colW - 20, botH - 35, TRUE);

            MoveWindow(hGroupOutput, margin + colW + gap, margin + topH + gap, colW, botH, TRUE);
            MoveWindow(hEditOutput, margin + colW + gap + 10, margin + topH + gap + 25, colW - 20, botH - 35, TRUE);
        }
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
                    BYTE stopBits = GetSelectedStopBits();
                    if (OpenAndConfigureSerial(portNumber, stopBits)) {
                        SetWindowTextW(hRichStatus, L"");
                        PrintStatusHeaderOnce();
                    }
                    SetFocus(hEditInput);
                }
            }
        }

        if (wmId == IDC_COMBO_STOPBITS && wmEvent == CBN_SELCHANGE) {
            BYTE stopBits = GetSelectedStopBits();
            if (hSerial != INVALID_HANDLE_VALUE) {
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

    case WM_TIMER: {
        if (wParam == IDT_STATUS_TIMER) {
            std::wstring statusText;
            if (hSerial != INVALID_HANDLE_VALUE) {
                statusText = L"Передано байт: " +
                    std::to_wstring(g_txCount.load()) +
                    L" | Кадров: " +
                    std::to_wstring(g_frameCount.load());
            }
            else {
                statusText = L"Выберите COM-порт в окне управления.";
            }
            SetWindowTextW(hStaticStatus, statusText.c_str());
        }
        break;
    }

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

// ============================================================================
// ТОЧКА ВХОДА WinMain
// ============================================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    LoadLibraryW(L"riched20.dll");

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"OSKS_Lab2_Var3_FinalFCS";

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProcMain;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClassW(&wc);

    hWndMain = CreateWindowExW(
        0, CLASS_NAME,
        L"ОсКС — Лабораторная работа №2 (Пакетная передача данных, Вариант 3)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 680,
        NULL, NULL, hInstance, NULL
    );

    if (!hWndMain) return 0;

    ShowWindow(hWndMain, nCmdShow);
    UpdateWindow(hWndMain);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
