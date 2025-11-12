#include <windows.h>
#include <shobjidl.h>   // IFileDialog
#include <commctrl.h>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cwctype>

#include "resource.h"

namespace fs = std::filesystem;

// ---------- utf8 helpers + exe dir ----------
static std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len ? len - 1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}
static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}
static fs::path GetExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// ---------- small UI helpers ----------
static void SetText(HWND hDlg, int ctrlId, const std::wstring& s) {
    SetWindowTextW(GetDlgItem(hDlg, ctrlId), s.c_str());
}
static std::wstring GetText(HWND hDlg, int ctrlId) {
    wchar_t buf[4096]{};
    GetWindowTextW(GetDlgItem(hDlg, ctrlId), buf, 4096);
    return std::wstring(buf);
}

// ---------- natural compare (number-aware) ----------
static bool IsSep(wchar_t c) {
    return c == L' ' || c == L'_' || c == L'-' || c == L'.';
}
static bool NaturalLess(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0, na = a.size(), nb = b.size();
    while (i < na && j < nb) {
        while (i < na && IsSep(a[i])) ++i;
        while (j < nb && IsSep(b[j])) ++j;
        if (i >= na || j >= nb) break;

        bool ad = iswdigit(a[i]) != 0;
        bool bd = iswdigit(b[j]) != 0;

        if (ad && bd) {
            unsigned long long va = 0, vb = 0;
            size_t ia = i, jb = j;
            while (ia < na && iswdigit(a[ia])) { va = va*10 + (unsigned)(a[ia]-L'0'); ++ia; }
            while (jb < nb && iswdigit(b[jb])) { vb = vb*10 + (unsigned)(b[jb]-L'0'); ++jb; }
            if (va != vb) return va < vb;
            size_t lena = ia - i, lenb = jb - j;
            if (lena != lenb) return lena < lenb;
            i = ia; j = jb;
            continue;
        }

        wchar_t ca = towlower(a[i]);
        wchar_t cb = towlower(b[j]);
        if (ca != cb) return ca < cb;
        ++i; ++j;
    }
    return (na - i) < (nb - j);
}

// ---------- populate files into combo (only "bf2savefile*", natural order) ----------
static void PopulateFileDropdown(HWND hCombo, const std::wstring& folder) {
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

    std::error_code ec;
    if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec)) return;

    std::vector<std::wstring> files;
    for (auto const& entry : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"bf2savefile", 0) != 0) continue; // prefix filter
            files.push_back(std::move(name));
        }
    }

    std::sort(files.begin(), files.end(), NaturalLess);

    for (const auto& f : files) {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)f.c_str());
    }
    if (!files.empty()) {
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    }
}

// ---------- folder picker ----------
static bool PickFolder(std::wstring& outFolder) {
    IFileDialog* pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (FAILED(hr)) return false;

    DWORD options = 0;
    pfd->GetOptions(&options);
    pfd->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

    hr = pfd->Show(nullptr);
    if (FAILED(hr)) { pfd->Release(); return false; }

    IShellItem* psi = nullptr;
    hr = pfd->GetResult(&psi);
    if (FAILED(hr) || !psi) { if (psi) psi->Release(); pfd->Release(); return false; }

    PWSTR pszPath = nullptr;
    hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    if (SUCCEEDED(hr) && pszPath) {
        outFolder = pszPath;
        CoTaskMemFree(pszPath);
    }
    psi->Release();
    pfd->Release();
    return !outFolder.empty();
}

// ---------- config load/save ----------
static void SaveConfig(const std::wstring& folder, const std::wstring& file, bool pinOnTop) {
    auto configPath = GetExeDir() / L"config.txt";
    std::ofstream out(configPath.string(), std::ios::binary);
    out << "directory=" << ToUtf8(folder) << "\n";
    out << "file=" << ToUtf8(file) << "\n";
    out << "pin=" << (pinOnTop ? "1" : "0") << "\n";
}
static void ApplyPin(HWND hDlg, bool pin) {
    SetWindowPos(
        hDlg,
        pin ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE
    );
    SendMessageW(GetDlgItem(hDlg, IDC_PIN), BM_SETCHECK, pin ? BST_CHECKED : BST_UNCHECKED, 0);
}
static bool LoadConfig(HWND hDlg) {
    auto configPath = GetExeDir() / L"config.txt";
    std::ifstream in(configPath.string(), std::ios::binary);
    if (!in) return false;

    std::string line;
    std::wstring folder, file;
    bool pin = false;

    while (std::getline(in, line)) {
        if (line.rfind("directory=", 0) == 0) folder = FromUtf8(line.substr(10));
        else if (line.rfind("file=", 0) == 0)   file   = FromUtf8(line.substr(5));
        else if (line.rfind("pin=", 0) == 0)    pin    = (line.size() > 4 && line[4] == '1');
    }
    in.close();

    if (!folder.empty()) {
        SetText(hDlg, IDC_EDIT_DIR, folder);
        PopulateFileDropdown(GetDlgItem(hDlg, IDC_COMBO_FILES), folder);
    }
    if (!file.empty()) {
        HWND hCombo = GetDlgItem(hDlg, IDC_COMBO_FILES);
        SendMessageW(hCombo, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)file.c_str());
    }
    ApplyPin(hDlg, pin);
    return true;
}

// ---------- dialog proc ----------
static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);

        wchar_t buf[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, buf);
        SetText(hDlg, IDC_EDIT_DIR, buf);
        SetText(hDlg, IDC_STATUS, L"");
        PopulateFileDropdown(GetDlgItem(hDlg, IDC_COMBO_FILES), buf);

        LoadConfig(hDlg); // applies saved dir/file and pin state if present
        return TRUE;
    }

    // paint "Success!" label green
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;
        if (GetDlgCtrlID(hCtrl) == IDC_STATUS) {
            SetTextColor(hdc, RGB(0, 128, 0));
            SetBkMode(hdc, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_3DFACE);
        }
        break;
    }

    case WM_TIMER: {
        if (wParam == 1) {
            KillTimer(hDlg, 1);
            SetText(hDlg, IDC_STATUS, L""); // hide after delay
        }
        return TRUE;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if (id == IDC_BUTTON_BROWSE) {
            std::wstring chosen;
            if (PickFolder(chosen)) {
                SetText(hDlg, IDC_EDIT_DIR, chosen);
                PopulateFileDropdown(GetDlgItem(hDlg, IDC_COMBO_FILES), chosen);
            }
            return TRUE;
        }

        if (id == IDC_EDIT_DIR && code == EN_CHANGE) {
            std::wstring folder = GetText(hDlg, IDC_EDIT_DIR);
            PopulateFileDropdown(GetDlgItem(hDlg, IDC_COMBO_FILES), folder);
            return TRUE;
        }

        if (id == IDC_PIN && code == BN_CLICKED) {
            BOOL checked = (SendMessageW(GetDlgItem(hDlg, IDC_PIN), BM_GETCHECK, 0, 0) == BST_CHECKED);
            ApplyPin(hDlg, checked);
            return TRUE;
        }

        if (id == IDOK) { // Overwrite
            std::wstring folder = GetText(hDlg, IDC_EDIT_DIR);
            HWND hCombo = GetDlgItem(hDlg, IDC_COMBO_FILES);
            int idx = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);

            std::wstring selectedFile;
            if (idx >= 0) {
                wchar_t name[1024]{};
                SendMessageW(hCombo, CB_GETLBTEXT, idx, (LPARAM)name);
                selectedFile = name;
            }

            fs::path src  = fs::path(folder) / selectedFile;       // from dropdown
            fs::path dest = fs::path(folder) / L"bf2savefile.sav"; // fixed target

            std::error_code ec;
            if (!selectedFile.empty() && fs::exists(src, ec)) {
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
            } else {
                ec = std::make_error_code(std::errc::no_such_file_or_directory);
            }

            BOOL pinChecked = (SendMessageW(GetDlgItem(hDlg, IDC_PIN), BM_GETCHECK, 0, 0) == BST_CHECKED);
            SaveConfig(folder, selectedFile, pinChecked != 0);

            SetText(hDlg, IDC_STATUS, ec ? L"Failed" : L"Success!");
            SetTimer(hDlg, 1, 2500, nullptr);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        EndDialog(hDlg, 0);
        return TRUE;
    }
    return FALSE;
}

// ---------- entry ----------
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, DlgProc, 0);
    CoUninitialize();
    return 0;
}
