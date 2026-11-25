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
#include <cstdlib>
#include <ctime>
#include <random>

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
#define IDC_CHECK_ERROR     107

#define IDT_STATUS_TIMER    201

// ============================================================================
// ОПРЕДЕЛЕНИЕ ПАРАМЕТРОВ КАНАЛЬНОГО КОДИРОВАНИЯ И РАСЧЁТ ДЛИНЫ ПОЛЯ FCS
// (Обязательные расчёты для отчёта по лабораторной работе №3, Вариант 3)
// ============================================================================
// 1. Вариант 3:
//    - Тип помехоустойчивого кода: Циклический код.
//    - Вероятность ошибки в поле данных: 30%.
//    - Требование к коду: обнаружение и исправление одиночных ошибок.
//
// 2. Расчёт длины поля FCS (в битах и байтах):
//    - Максимальная длина поля данных кадра: L_data = 279 байт.
//    - Число информационных бит: k = 279 * 8 = 2232 бита.
//    - Для обнаружения и исправления всех одиночных ошибок в кодовом слове
//      длины n = k + r должно выполняться неравенство Хэмминга:
//          2^r >= n + 1 = k + r + 1
//          2^r >= 2232 + r + 1 = 2233 + r
//    - Проверим минимальные значения проверочных бит r:
//          При r = 11: 2^11 = 2048 < 2233 + 11 = 2244 (не подходит)
//          При r = 12: 2^12 = 4096 >= 2233 + 12 = 2245 (выполняется!)
//    - Минимальная необходимая длина FCS в битах: r = 12 бит.
//    - Длина кодового слова циклического кода: n = k + r = 2232 + 12 = 2244 бита.
//    - Округление длины поля FCS до целого числа байт (так как минимальной
//      единицей передачи через COM-порт является 1 байт = 8 бит):
//          L_FCS = ceil(12 / 8) = 2 байта (16 бит).
//    - В структуре кадра из ЛР №2 было предусмотрено поле fcs (1 байт) и
//      reserved (10 байт). Расширяем поле fcs до 2 байт за счёт использования
//      1 байта из зарезервированных (остаётся 9 байт резерва).
//      Общий размер кадра остаётся строго равным 300 байтам:
//      8 (Flag) + 1 (Addr) + 1 (Type) + 279 (Data) + 2 (FCS) + 9 (Reserved) = 300 байт.
//
// 3. Образующий полином циклического кода G(x) степени r = 12:
//    Используем примитивный полином степени 12 над полем GF(2):
//          G(x) = x^12 + x^6 + x^4 + x + 1
//    Период полинома равен 2^12 - 1 = 4095 >= n (2244), что гарантирует
//    различные ненулевые синдромы для всех одиночных ошибок.
// ============================================================================

constexpr size_t DATA_BYTES_LEN = 279;
constexpr size_t FCS_BYTES_LEN = 2;
constexpr size_t RESERVED_BYTES_LEN = 9;
constexpr size_t FRAME_BODY_LEN = 1 + 1 + DATA_BYTES_LEN + FCS_BYTES_LEN + RESERVED_BYTES_LEN; // 292 байта
constexpr size_t FULL_FRAME_LEN = 8 + FRAME_BODY_LEN; // 300 байт

constexpr size_t CYCLIC_K_BITS = DATA_BYTES_LEN * 8; // 2232 информационных бита
constexpr size_t CYCLIC_R_BITS = 12;                 // 12 проверочных бит
constexpr size_t CYCLIC_N_BITS = CYCLIC_K_BITS + CYCLIC_R_BITS; // 2244 бита
constexpr int    ERROR_PROBABILITY = 30;                 // Вероятность ошибки 30% (Вариант 3)

// Коэффициенты образующего полинома G(x) = x^12 + x^6 + x^4 + x + 1 (от старшей степени x^12 к x^0)
const uint8_t g_generatorPoly[CYCLIC_R_BITS + 1] = {
    1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1
};

// Структура кадра со служебными полями Addr, Type и 2-байтовым полем FCS
#pragma pack(push, 1)
struct DataFrame {
    char    flag[8];                       // Флаг начала кадра ("450501-3")
    uint8_t timestamp;                          // Служебное поле 1: Адрес узла
    uint8_t packetType;                    // Служебное поле 2: Тип пакета
    uint8_t data[DATA_BYTES_LEN];          // Данные + Padding (279 байт)
    uint8_t fcs[FCS_BYTES_LEN];            // Служебное поле 3: FCS (2 байта = 12 бит кода + 4 бита дополнения)
    uint8_t reserved[RESERVED_BYTES_LEN];  // 9 зарезервированных байтов
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
HWND hCheckError = NULL;
HWND hStaticStatus = NULL;
HWND hRichStatus = NULL;
HWND hEditInput = NULL;
HWND hEditOutput = NULL;
WNDPROC origEditProc = NULL;

HFONT hGuiFont = NULL;
HFONT hMonoFont = NULL; // Моноширинный шрифт для вывода кадров в окне состояния

HANDLE hSerial = INVALID_HANDLE_VALUE;
HANDLE hReadThread = NULL;

std::atomic<bool> g_bRunning(false);
std::atomic<bool> g_bErrorEmulation(false);      // Флаг разрешения эмуляции ошибок
std::atomic<unsigned long long> g_txCount(0);    // Счётчик переданных байт (строго кратно 300)
std::atomic<unsigned long long> g_frameCount(0); // Счётчик отправленных кадров
std::atomic<unsigned long long> g_errorCount(0); // Счётчик кадров с внесённой ошибкой
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
// ЦИКЛИЧЕСКИЙ КОД (АЛГОРИТМ «КАК НА БУМАГЕ»: ДЕЛЕНИЕ УГОЛКОМ ПО МОДУЛЮ 2)
// ============================================================================

// Деление многочлена (битового вектора) на образующий полином G(x) столбиком по модулю 2
std::vector<uint8_t> DividePolynomialModulo2(std::vector<uint8_t> dividend) {
    const size_t gLen = CYCLIC_R_BITS + 1; // 13 бит
    for (size_t i = 0; i + gLen <= dividend.size(); ++i) {
        if (dividend[i] == 1) { // Если старший разряд текущего остатка равен 1
            for (size_t j = 0; j < gLen; ++j) {
                dividend[i + j] ^= g_generatorPoly[j]; // Вычитание по модулю 2 (XOR)
            }
        }
    }
    // Остаток от деления находится в последних r = 12 разрядах
    return std::vector<uint8_t>(dividend.end() - CYCLIC_R_BITS, dividend.end());
}

// Вычисление 2-байтового поля FCS для поля данных (279 байт = 2232 бита)
void CalculateCyclicFCS(const std::vector<uint8_t>& dataBytes, uint8_t& outFcs0, uint8_t& outFcs1) {
    // 1. Переводим поле данных (279 байт) в битовую последовательность M(x) (2232 бита)
    std::vector<uint8_t> bits;
    bits.reserve(CYCLIC_N_BITS);
    for (size_t i = 0; i < DATA_BYTES_LEN; ++i) {
        uint8_t b = (i < dataBytes.size()) ? dataBytes[i] : 0;
        for (int bit = 7; bit >= 0; --bit) {
            bits.push_back((b >> bit) & 1);
        }
    }   

    // 2. Умножаем M(x) на x^r: дописываем в конец r = 12 нулевых бит
    for (size_t i = 0; i < CYCLIC_R_BITS; ++i) {
        bits.push_back(0);
    }

    // 3. Делим M(x)*x^r на G(x) уголком и получаем 12-битный остаток R(x)
    std::vector<uint8_t> rem = DividePolynomialModulo2(bits);

    // 4. Упаковываем 12 бит остатка R(x) в 2 байта поля FCS (округление до байтов)
    outFcs0 = 0;
    outFcs1 = 0;
    for (int i = 0; i < 8; ++i) {
        if (rem[i]) outFcs0 |= (1 << (7 - i));
    }
    for (int i = 0; i < 4; ++i) {
        if (rem[8 + i]) outFcs1 |= (1 << (7 - i));
    }
}

// Декодирование циклического кода на приёмной стороне с обнаружением и исправлением одиночной ошибки
void CyclicDecodeAndCorrect(std::vector<uint8_t>& dataBytes, uint8_t fcs0, uint8_t fcs1) {
    // 1. Формируем принятое кодовое слово V(x) длины n = 2244 бита: 2232 бита данных + 12 бит FCS
    std::vector<uint8_t> codeword;
    codeword.reserve(CYCLIC_N_BITS);
    for (size_t i = 0; i < DATA_BYTES_LEN; ++i) {
        uint8_t b = dataBytes[i];
        for (int bit = 7; bit >= 0; --bit) {
            codeword.push_back((b >> bit) & 1);
        }
    }
    for (int i = 0; i < 8; ++i) {
        codeword.push_back((fcs0 >> (7 - i)) & 1);
    }
    for (int i = 0; i < 4; ++i) {
        codeword.push_back((fcs1 >> (7 - i)) & 1);
    }

    // 2. Вычисляем синдром S(x) как остаток от деления V(x) на G(x)
    std::vector<uint8_t> syndrome = DividePolynomialModulo2(codeword);

    // Проверяем, равен ли синдром нулю
    bool hasError = false;
    for (uint8_t bit : syndrome) {
        if (bit != 0) {
            hasError = true;
            break;
        }
    }
    if (!hasError) return; // Ошибок нет

    // 3. Если S(x) != 0, ищем позицию одиночной ошибки «как на бумаге»:
    //    одиночная ошибка в позиции pos даёт полином ошибки E(x) = x^(n - 1 - pos).
    //    Последовательно вычисляем остаток от деления x^m на G(x) сдвигом и сложением по модулю 2.
    uint8_t reg[CYCLIC_R_BITS + 1] = { 0 };
    reg[CYCLIC_R_BITS] = 1; // Остаток для ошибки в самом младшем разряде (x^0)

    for (int pos = (int)CYCLIC_N_BITS - 1; pos >= 0; --pos) {
        // Сравниваем текущий остаток E(x) mod G(x) с полученным синдромом S(x)
        bool match = true;
        for (size_t j = 0; j < CYCLIC_R_BITS; ++j) {
            if (reg[1 + j] != syndrome[j]) {
                match = false;
                break;
            }
        }

        if (match) {
            // Одиночная ошибка локализована в разряде pos — инвертируем искажённый бит
            codeword[pos] ^= 1;
            if ((size_t)pos < CYCLIC_K_BITS) {
                size_t byteIdx = (size_t)pos / 8;
                int bitIdx = 7 - ((int)pos % 8);
                dataBytes[byteIdx] ^= (1 << bitIdx);
            }
            break;
        }

        // Переход к следующему разряду: умножение на x по модулю G(x)
        for (size_t j = 0; j < CYCLIC_R_BITS; ++j) {
            reg[j] = reg[j + 1];
        }
        reg[CYCLIC_R_BITS] = 0;

        if (reg[0] == 1) {
            for (size_t j = 0; j <= CYCLIC_R_BITS; ++j) {
                reg[j] ^= g_generatorPoly[j];
            }
        }
    }
}

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
// ПОДПРОГРАММА ЭМУЛЯЦИИ ОШИБОК (ВАРИАНТ 3 — ВЕРОЯТНОСТЬ 30%)
// ============================================================================
// Случайное искажение одного случайного бита в одном случайном байте
// в поле данных каждого кадра непосредственно перед передачей.
void EmulateDataError(std::vector<uint8_t>& frameBody, size_t dataLen, std::vector<bool>& errorModifiedMask) {
    errorModifiedMask.assign(frameBody.size(), false);

    if (!g_bErrorEmulation.load() || dataLen == 0) {
        return;
    }

    static std::mt19937 rng((unsigned int)std::time(NULL) ^ (unsigned int)GetTickCount());
    std::uniform_int_distribution<int> probDist(0, 99);

    // Строго 30% вероятность ошибки для каждого кадра
    if (probDist(rng) < ERROR_PROBABILITY) {
        size_t dataOffset = 2; // Смещение поля данных: Addr(1) + Type(1) = 2

        // Случайный выбор байта по всей длине поля данных (от 0 до dataLen - 1)
        std::uniform_int_distribution<size_t> byteDist(0, dataLen - 1);
        // Случайный выбор бита внутри выбранного байта (от 0 до 7)
        std::uniform_int_distribution<int> bitDist(0, 7);

        size_t targetByteIdx = dataOffset + byteDist(rng);
        int randomBitIdx = bitDist(rng);

        if (targetByteIdx < frameBody.size()) {
            frameBody[targetByteIdx] ^= (uint8_t)(1 << randomBitIdx);
            errorModifiedMask[targetByteIdx] = true;
            g_errorCount++;
        }
    }
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

// Приём входящих пакетов с дестаффингом и декодированием циклическим кодом
DWORD WINAPI SerialReadThread(LPVOID lpParam) {
    std::vector<uint8_t> rxBuffer;
    char tempBuf[128];
    DWORD bytesRead = 0;
    const std::string flagStr = "450501-3";

    while (g_bRunning) {
        if (hSerial == INVALID_HANDLE_VALUE) {
            Sleep(50);
            continue;
        }

        BOOL success = ReadFile(hSerial, tempBuf, sizeof(tempBuf), &bytesRead, NULL);
        if (success && bytesRead > 0) {
            rxBuffer.insert(rxBuffer.end(), tempBuf, tempBuf + bytesRead);

            // Ищем кадр строго фиксированного размера 300 байт
            while (rxBuffer.size() >= FULL_FRAME_LEN) {
                auto it = std::search(rxBuffer.begin(), rxBuffer.end(), flagStr.begin(), flagStr.end());
                if (it == rxBuffer.end()) {
                    if (rxBuffer.size() > 8) {
                        rxBuffer.erase(rxBuffer.begin(), rxBuffer.end() - 8);
                    }
                    break;
                }

                if (it != rxBuffer.begin()) rxBuffer.erase(rxBuffer.begin(), it);
                if (rxBuffer.size() < FULL_FRAME_LEN) break;

                std::vector<uint8_t> stuffedBody(rxBuffer.begin() + 8, rxBuffer.begin() + FULL_FRAME_LEN);
                rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + FULL_FRAME_LEN);

                // 1. Дестаффинг принятого тела кадра
                std::vector<uint8_t> rawBody = BitDestuff(stuffedBody);

                // Структура тела: [Addr: 0] [Type: 1] [Data: 2..280] [FCS: 281..282] [Reserved: 283..291]
                if (rawBody.size() >= 2 + DATA_BYTES_LEN + FCS_BYTES_LEN) {
                    std::vector<uint8_t> dataField(rawBody.begin() + 2, rawBody.begin() + 2 + DATA_BYTES_LEN);
                    uint8_t receivedFcs0 = rawBody[281];
                    uint8_t receivedFcs1 = rawBody[282];

                    // 2. Проверка и исправление одиночной ошибки циклическим кодом
                    CyclicDecodeAndCorrect(dataField, receivedFcs0, receivedFcs1);

                    // 3. Считываем полезный текст до первого нулевого байта паддинга
                    const char* pData = reinterpret_cast<const char*>(dataField.data());
                    size_t textLen = 0;
                    while (textLen < DATA_BYTES_LEN && pData[textLen] != '\0') {
                        textLen++;
                    }

                    if (textLen > 0) {
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
// ВЫВОД В ОКНО СОСТОЯНИЯ (RICHEDIT С МОНОШИРИННЫМ ШРИФТОМ)
// ============================================================================
void AppendFormattedHexByte(uint8_t b, bool isModified, bool addComma) {
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_UNDERLINE | CFM_COLOR | CFM_FACE | CFM_BOLD | CFM_SIZE;
    cf.crTextColor = RGB(0, 0, 0);
    cf.dwEffects = isModified ? CFE_UNDERLINE : 0;
    cf.yHeight = 165; // Фиксированный компактный размер моноширинного шрифта (8.25 pt)
    wcscpy_s(cf.szFaceName, L"Consolas");

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
    cf.dwMask = CFM_UNDERLINE | CFM_FACE | CFM_BOLD | CFM_SIZE;
    cf.dwEffects = 0;
    cf.yHeight = 165;
    wcscpy_s(cf.szFaceName, L"Consolas");

    int textLen = GetWindowTextLengthW(hRichStatus);
    SendMessageW(hRichStatus, EM_SETSEL, (WPARAM)textLen, (LPARAM)textLen);
    SendMessageW(hRichStatus, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(hRichStatus, EM_REPLACESEL, FALSE, (LPARAM)L" ");
}

void PrintStatusHeaderOnce() {
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BOLD | CFM_COLOR | CFM_FACE | CFM_SIZE;
    cf.dwEffects = CFE_BOLD;
    cf.crTextColor = RGB(10, 60, 160);
    cf.yHeight = 165;
    wcscpy_s(cf.szFaceName, L"Consolas");

    // Названия полей кадра (с учётом 2-байтового поля FCS)
    std::wstring header = L"Поля: [Заголовок] [Время] [Тип] [Данные] [FCS (2 байта)]\r\n"
        L"------------------------------------------------------------------------\r\n";
    SendMessageW(hRichStatus, EM_SETSEL, 0, 0);
    SendMessageW(hRichStatus, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(hRichStatus, EM_REPLACESEL, FALSE, (LPARAM)header.c_str());
}

void AppendStatusFormatted(const wchar_t* prefix, const std::vector<uint8_t>& frameBody, size_t dataLen, const std::vector<bool>& modifiedMask) {
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BOLD | CFM_COLOR | CFM_UNDERLINE | CFM_FACE | CFM_SIZE;
    cf.dwEffects = CFE_BOLD;
    cf.crTextColor = RGB(30, 30, 30);
    cf.yHeight = 165;
    wcscpy_s(cf.szFaceName, L"Consolas");

    int textLen = GetWindowTextLengthW(hRichStatus);
    SendMessageW(hRichStatus, EM_SETSEL, (WPARAM)textLen, (LPARAM)textLen);
    SendMessageW(hRichStatus, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(hRichStatus, EM_REPLACESEL, FALSE, (LPARAM)prefix);

    // 1. Флаг начала (8 байт: "450501-3")
    const uint8_t flagBytes[] = { '4', '5', '0', '5', '0', '1', '-', '3' };
    for (int i = 0; i < 8; ++i) {
        AppendFormattedHexByte(flagBytes[i], false, (i < 7));
    }
    AppendFieldSeparator();

    // 2. Служебное поле 1: Адрес (Addr)
    uint8_t timestamp = (frameBody.size() > 0) ? frameBody[0] : 0;
    bool modtimestamp = (modifiedMask.size() > 0) ? modifiedMask[0] : false;
    AppendFormattedHexByte(timestamp, modtimestamp, false);
    AppendFieldSeparator();

    // 3. Служебное поле 2: Тип кадра (PacketType)
    uint8_t type = (frameBody.size() > 1) ? frameBody[1] : 0;
    bool modType = (modifiedMask.size() > 1) ? modifiedMask[1] : false;
    AppendFormattedHexByte(type, modType, false);
    AppendFieldSeparator();

    // 4. Поле данных (модифицированные байты подчёркиваются)
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

    
    size_t fcsIdx0 = 281;
    size_t fcsIdx1 = 282;
    uint8_t fcsVal0 = (fcsIdx0 < frameBody.size()) ? frameBody[fcsIdx0] : 0;
    uint8_t fcsVal1 = (fcsIdx1 < frameBody.size()) ? frameBody[fcsIdx1] : 0;
    bool modFcs0 = (fcsIdx0 < modifiedMask.size()) ? modifiedMask[fcsIdx0] : false;
    bool modFcs1 = (fcsIdx1 < modifiedMask.size()) ? modifiedMask[fcsIdx1] : false;
    AppendFormattedHexByte(fcsVal0, modFcs0, true);
    AppendFormattedHexByte(fcsVal1, modFcs1, false);

    // 9 зарезервированных байтов в конце кадра не выводятся в RichEdit
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
        size_t chunkLen = (remaining < DATA_BYTES_LEN) ? remaining : DATA_BYTES_LEN;

        // Формируем 279-байтовое поле данных (полезный текст + дополнение нулями)
        std::vector<uint8_t> dataField(DATA_BYTES_LEN, 0x00);
        for (size_t i = 0; i < chunkLen; ++i) {
            dataField[i] = (uint8_t)text[offset + i];
        }

        // Расчёт 2-байтового поля FCS циклическим кодом (применяется только к полю данных)
        uint8_t fcs0 = 0, fcs1 = 0;
        CalculateCyclicFCS(dataField, fcs0, fcs1);

        // Тело кадра (292 байта): Addr(1) + Type(1) + Data(279) + FCS(2) + Reserved(9)
        std::vector<uint8_t> rawBody;
        rawBody.reserve(FRAME_BODY_LEN);
        rawBody.push_back(0x00); // Поле 1: Addr = 00h
        rawBody.push_back(0x01); // Поле 2: PacketType = 01h (Информационный кадр)

        // Поле данных (279 байт)
        rawBody.insert(rawBody.end(), dataField.begin(), dataField.end());

        // Поле FCS (2 байта)
        rawBody.push_back(fcs0);
        rawBody.push_back(fcs1);

        // 9 зарезервированных байтов
        for (size_t i = 0; i < RESERVED_BYTES_LEN; ++i) {
            rawBody.push_back(0x00);
        }

        // Бит-стаффинг применяется к кадру с уже заполненным полем FCS (п. 4 задания)
        std::vector<uint8_t> stuffedBody;
        std::vector<bool> stuffModifiedMask;
        BitStuff(rawBody, stuffedBody, stuffModifiedMask);

        // Фиксация тела кадра ровно на 292 байта
        if (stuffedBody.size() < FRAME_BODY_LEN) {
            stuffedBody.resize(FRAME_BODY_LEN, 0x00);
            stuffModifiedMask.resize(FRAME_BODY_LEN, false);
        }
        else if (stuffedBody.size() > FRAME_BODY_LEN) {
            stuffedBody.resize(FRAME_BODY_LEN);
            stuffModifiedMask.resize(FRAME_BODY_LEN);
        }

        // Эмуляция одиночной ошибки в поле данных непосредственно перед передачей (п. 2 и п. 7)
        std::vector<uint8_t> transmittedBody = stuffedBody;
        std::vector<bool> errorModifiedMask;
        EmulateDataError(transmittedBody, chunkLen, errorModifiedMask);

        // Вывод последнего переданного кадра в окно состояния (3 строки с моноширинным выравниванием)
        SetWindowTextW(hRichStatus, L"");
        PrintStatusHeaderOnce();

        std::vector<bool> noModMask(rawBody.size(), false);
        AppendStatusFormatted(L"До стаффинга:    ", rawBody, chunkLen, noModMask);
        AppendStatusFormatted(L"После стаффинга: ", stuffedBody, chunkLen, stuffModifiedMask);
        AppendStatusFormatted(L"После ошибки:    ", transmittedBody, chunkLen, errorModifiedMask);

        // Полный кадр: Флаг (8 байт) + Тело кадра после эмуляции ошибки (292 байта) = РОВНО 300 БАЙТ
        std::vector<uint8_t> fullPacket;
        fullPacket.reserve(FULL_FRAME_LEN);
        fullPacket.insert(fullPacket.end(), "450501-3", "450501-3" + 8);
        fullPacket.insert(fullPacket.end(), transmittedBody.begin(), transmittedBody.end());

        // Отправка в COM-порт
        DWORD written = 0;
        WriteFile(hSerial, fullPacket.data(), (DWORD)fullPacket.size(), &written, NULL);

        // Счётчик всегда увеличивается строго на 300 байт
        g_txCount += FULL_FRAME_LEN;
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
    if (bLen < (int)DATA_BYTES_LEN) return;

    std::string unsentStr(bLen, 0);
    WideCharToMultiByte(CP_ACP, 0, pUnsent, unsentWLen, &unsentStr[0], bLen, NULL, NULL);

    while (unsentStr.length() >= DATA_BYTES_LEN) {
        std::string chunk = unsentStr.substr(0, DATA_BYTES_LEN);
        SendFramedMessage(chunk);

        int chunkWLen = MultiByteToWideChar(CP_ACP, 0, chunk.c_str(), (int)chunk.length(), NULL, 0);
        g_unsentStartPos += chunkWLen;

        unsentStr = unsentStr.substr(DATA_BYTES_LEN);
    }
}

// Окно ввода: поведение полностью сохранено из ЛР1/ЛР2
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {

        // 1. Любое стирание запрещено
    case WM_KEYDOWN:
        if (wParam == VK_BACK || wParam == VK_DELETE) return 0;
        break;
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
    case EM_UNDO:
        return 0;

        // 2. Вставка: только при открытом порте и всегда в конец текста
    case WM_PASTE: {
        if (hSerial == INVALID_HANDLE_VALUE) return 0;
        int len = GetWindowTextLengthW(hWnd);
        SendMessageW(hWnd, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        LRESULT res = CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
        CheckAndAutoSend(hWnd);
        return res;
    }

                 // 3. Ввод символов
    case WM_CHAR: {
        if (hSerial == INVALID_HANDLE_VALUE) return 0; // порт не выбран: печатать нельзя
        if (wParam == VK_BACK) return 0;               // символ Backspace

        // Печатаемый символ или Enter: каретка в конец, выделение схлопывается
        if (wParam >= 32 || wParam == VK_RETURN) {
            int len = GetWindowTextLengthW(hWnd);
            SendMessageW(hWnd, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        }

        if (wParam == VK_RETURN) {
            CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
            return 0;
        }

        LRESULT res = CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
        CheckAndAutoSend(hWnd);
        return res;
    }
    }
    return CallWindowProc(origEditProc, hWnd, uMsg, wParam, lParam);
}

// ============================================================================
// ГЛАВНОЕ ОКНО ПРИЛОЖЕНИЯ
// ============================================================================
LRESULT CALLBACK WndProcMain(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        hGuiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // Создаём моноширинный шрифт специально для вывода кадров в окне состояния
        hMonoFont = CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas"
        );

        // 1. Окно управления
        hGroupControl = CreateWindowW(L"BUTTON", L"1. Окно управления (Параметры COM-порта и ошибок)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupControl, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

        hComboPort = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_COMBO_PORT, NULL, NULL);
        SendMessageW(hComboPort, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

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
        SendMessageW(hComboStopBits, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 1 стоп-бит");
        SendMessageW(hComboStopBits, CB_ADDSTRING, 0, (LPARAM)L"Стоп-биты: 2 стоп-бита");
        SendMessageW(hComboStopBits, CB_SETCURSEL, 0, 0);

        // Элемент управления разрешением-запрещением эмуляции ошибок (п. 7 задания)
        hCheckError = CreateWindowW(L"BUTTON", L"Разрешить эмуляцию ошибок (вероятность 30%)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hWnd, (HMENU)IDC_CHECK_ERROR, NULL, NULL);
        SendMessageW(hCheckError, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

        // 2. Окно состояния
        hGroupStatus = CreateWindowW(L"BUTTON", L"2. Окно состояния (Кадры и статистика)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupStatus, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

        hStaticStatus = CreateWindowW(L"STATIC", L"Статус: Выберите COM-порт в окне управления...",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_STATIC_STATUS, NULL, NULL);
        SendMessageW(hStaticStatus, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

        hRichStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit20W", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_RICH_STATUS, NULL, NULL);
        SendMessageW(hRichStatus, WM_SETFONT, (WPARAM)(hMonoFont ? hMonoFont : hGuiFont), TRUE);
        SendMessageW(hRichStatus, EM_SETTARGETDEVICE, 0, 1); // Горизонтальная прокрутка без переноса строк
        SendMessageW(hRichStatus, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(252, 252, 252));

        SetTimer(hWnd, IDT_STATUS_TIMER, 200, NULL);

        // 3. Окно ввода сообщений
        hGroupInput = CreateWindowW(L"BUTTON", L"3. Окно ввода сообщений",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupInput, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

        hEditInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_INPUT, NULL, NULL);
        SendMessageW(hEditInput, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
        SendMessageW(hEditInput, EM_SETLIMITTEXT, 0, 0); // Снимаем стандартный лимит в 30 000 символов
        origEditProc = (WNDPROC)SetWindowLongPtrW(hEditInput, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        // 4. Окно вывода сообщений
        hGroupOutput = CreateWindowW(L"BUTTON", L"4. Окно вывода сообщений (Принятые данные)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        SendMessageW(hGroupOutput, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

        hEditOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_OUTPUT, NULL, NULL);
        SendMessageW(hEditOutput, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
        SendMessageW(hEditOutput, EM_SETLIMITTEXT, 0, 0); // Снимаем стандартный лимит в 30 000 символов

        SetFocus(hEditInput);
        break;
    }

    case WM_SIZE: {
        int totalW = LOWORD(lParam);
        int totalH = HIWORD(lParam);
        int margin = 12;
        int gap = 12;
        int topH = 185;
        int botH = totalH - topH - (margin * 2) - gap;
        int colW = (totalW - (margin * 2) - gap) / 2;

        if (colW > 150 && botH > 60) {
            MoveWindow(hGroupControl, margin, margin, colW, topH, TRUE);
            int comboW = (colW - 36) / 2;
            MoveWindow(hComboPort, margin + 12, margin + 40, comboW, 250, TRUE);
            MoveWindow(hComboStopBits, margin + 24 + comboW, margin + 40, comboW, 250, TRUE);
            MoveWindow(hCheckError, margin + 12, margin + 85, colW - 24, 24, TRUE);

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

        if (wmId == IDC_CHECK_ERROR && wmEvent == BN_CLICKED) {
            LRESULT checked = SendMessageW(hCheckError, BM_GETCHECK, 0, 0);
            g_bErrorEmulation = (checked == BST_CHECKED);
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
                    std::to_wstring(g_frameCount.load()) +
                    L" | Искажено: " +
                    std::to_wstring(g_errorCount.load()) +
                    (g_bErrorEmulation.load() ? L" (30% ВКЛ)" : L" (ВЫКЛ)");
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
        if (hMonoFont) {
            DeleteObject(hMonoFont);
            hMonoFont = NULL;
        }
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
    std::srand((unsigned int)std::time(NULL));
    LoadLibraryW(L"riched20.dll");

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"OSKS_Lab3_Var3_CyclicCode";

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProcMain;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClassW(&wc);

    hWndMain = CreateWindowExW(
        0, CLASS_NAME,
        L"ОсКС — Лабораторная работа №3 (Канальное кодирование, Вариант 3)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 700,
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
