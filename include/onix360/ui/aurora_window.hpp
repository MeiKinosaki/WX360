#pragma once
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "onix360/storage/data_directories.hpp"
#include "onix360/storage/game_library.hpp"
#include "onix360/storage/content_library.hpp"
#include "onix360/state/state_slots.hpp"
#include "onix360/ui/slot_visual_state.hpp"
#include "onix360/ui/clock_display.hpp"
#include "onix360/version.hpp"
#include "onix360/xex/xex_image_loader.hpp"

namespace onix360::ui {
class AuroraWindow {
public:
 int run(HINSTANCE instance, int show, std::filesystem::path rom_root = {}) {
  rom_root_ = std::move(rom_root);
  if (rom_root_.empty()) rom_root_ = storage::DataDirectories::default_root() / "roms";
  games_ = storage::GameLibrary::scan(rom_root_);
  content_entries_ = storage::ContentLibrary::scan(rom_root_);
  state_slots_ = state::StateSlots(rom_root_.parent_path() / "saves" / "states");
  refresh_slots();

  WNDCLASSW wc{};
  wc.lpfnWndProc = window_proc;
  wc.hInstance = instance;
  wc.lpszClassName = L"Onix360AuroraWindow";
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.hbrBackground = nullptr;
  RegisterClassW(&wc);
  instance_ = instance;
  std::wstring window_title = L"Onix360 ";
  window_title += version::display;
  window_title += L" | Aurora-style Dashboard";
  HWND window = CreateWindowExW(0, wc.lpszClassName, window_title.c_str(),
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760,
      nullptr, nullptr, instance, this);
  if (!window) return 1;
  ShowWindow(window, show);
  UpdateWindow(window);
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
   TranslateMessage(&msg);
   DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
 }

private:
 static constexpr UINT_PTR kClockTimerId = 0x3601;
 static LRESULT CALLBACK window_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  AuroraWindow* self = reinterpret_cast<AuroraWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (m == WM_NCCREATE) {
   auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
   self = static_cast<AuroraWindow*>(cs->lpCreateParams);
   SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (!self) return DefWindowProcW(h, m, w, l);
  switch (m) {
   case WM_CREATE:
    self->update_clock();
    SetTimer(h, kClockTimerId, 1000, nullptr);
    return 0;
   case WM_TIMER:
    if (w == kClockTimerId) {
     self->update_clock();
     InvalidateRect(h, nullptr, FALSE);
     return 0;
    }
    break;
   case WM_PAINT: self->paint(h); return 0;
   case WM_ERASEBKGND: return 1;
   case WM_DESTROY:
    KillTimer(h, kClockTimerId);
    PostQuitMessage(0);
    return 0;
   case WM_KEYDOWN:
    if (w == VK_ESCAPE) DestroyWindow(h);
    else if (w == VK_DOWN || w == VK_RIGHT) {
     self->selected_nav_ = (self->selected_nav_ + 1) % 5;
     InvalidateRect(h, nullptr, FALSE);
    } else if (w == VK_UP || w == VK_LEFT) {
     self->selected_nav_ = (self->selected_nav_ + 4) % 5;
     InvalidateRect(h, nullptr, FALSE);
    } else if (w == VK_DELETE && self->selected_nav_ == 3 && self->selected_slot_ >= 0) {
     std::string error;
     if (self->state_slots_.erase(static_cast<unsigned>(self->selected_slot_), &error)) {
      self->slot_message_ = L"Slot apagado.";
      self->refresh_slots();
     } else {
      self->slot_message_ = std::wstring(error.begin(), error.end());
     }
     InvalidateRect(h, nullptr, FALSE);
    }
    return 0;
   case WM_LBUTTONDOWN: {
    const int x = static_cast<short>(LOWORD(l));
    const int y = static_cast<short>(HIWORD(l));
    if (x < 210 && y >= 105 && y < 345) {
     const int index = (y - 105) / 48;
     if (index >= 0 && index < 5) {
      self->selected_nav_ = index;
      InvalidateRect(h, nullptr, FALSE);
     }
    } else if (self->selected_nav_ == 1) {
     for (std::size_t i = 0; i < self->games_.size() && i < 8; ++i) {
      const int row_y = 280 + static_cast<int>(i) * 34;
      if (y < row_y - 4 || y >= row_y + 28) continue;
      const auto& game = self->games_[i];
      if (game.format == "XEX") {
       xex::FullXexImage image;
       const auto status = xex::XexImageLoader::read_complete(game.path, image);
       if (status == xex::LoadError::None) {
        self->xex_message_ = L"XEX lido e metadados validados; nenhum jogo foi iniciado. Entry point: 0x" +
            hex_value(image.header.entry_point) + L", imagem: " +
            std::to_wstring(image.format.image_size) + L" bytes.";
       } else {
        self->xex_message_ = L"Falha ao ler/validar XEX. Codigo: " +
            std::to_wstring(static_cast<unsigned>(status));
       }
      } else {
       self->xex_message_ = L"ISO detectado; abertura ISO ainda nao conectada.";
      }
      InvalidateRect(h, nullptr, FALSE);
      break;
     }
    } else if (self->selected_nav_ == 3) {
     for (int i = 0; i < 10; ++i) {
      const int col = i % 5, row = i / 5;
      const int sx = 250 + col * 170, sy = 300 + row * 105;
      if (x >= sx && x < sx + 150 && y >= sy && y < sy + 78) {
       self->selected_slot_ = i;
       self->inspect_slot(static_cast<unsigned>(i));
       InvalidateRect(h, nullptr, FALSE);
       break;
      }
     }
    }
    return 0;
   }
  }
  return DefWindowProcW(h, m, w, l);
 }

 void update_clock() {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  const auto formatted = format_clock({now.wYear, now.wMonth, now.wDay,
      now.wHour, now.wMinute, now.wSecond});
  clock_time_ = formatted.time;
  clock_date_ = formatted.date;
 }

 static std::wstring hex_value(std::uint32_t value) {
  static constexpr wchar_t digits[] = L"0123456789ABCDEF";
  std::wstring result(8, L'0');
  for (int i = 7; i >= 0; --i) {
   result[static_cast<std::size_t>(i)] = digits[value & 0x0Fu];
   value >>= 4;
  }
  return result;
 }

 void refresh_slots() {
  for (unsigned i = 0; i < state::StateSlots::kSlotCount; ++i) {
   const auto status = slot_visual_state(state_slots_, i);
   slot_labels_[i] = status == SlotVisualState::Empty ? L"VAZIO" :
       status == SlotVisualState::Valid ? L"VALIDO" : L"INVALIDO";
  }
 }

 void inspect_slot(unsigned slot) {
  state::Snapshot snapshot;
  std::string error;
  if (state_slots_.load(slot, snapshot, &error)) {
   slot_message_ = L"Snapshot valido | PC: 0x" +
       hex_value(static_cast<std::uint32_t>(snapshot.cpu.pc)) +
       L" | RAM: " + std::to_wstring(snapshot.ram.size()) + L" bytes.";
  } else {
   slot_message_ = L"Slot nao carregavel: " + std::wstring(error.begin(), error.end());
  }
 }

 void draw_text(HDC dc, int x, int y, const wchar_t* value, COLORREF color) {
  SetTextColor(dc, color);
  TextOutW(dc, x, y, value, lstrlenW(value));
 }

 void panel(HDC dc, int x, int y, int width, int height, COLORREF color) {
  RECT box{x, y, x + width, y + height};
  HBRUSH brush = CreateSolidBrush(color);
  FillRect(dc, &box, brush);
  DeleteObject(brush);
 }

 void paint(HWND h) {
  PAINTSTRUCT ps{};
  HDC dc = BeginPaint(h, &ps);
  RECT r{};
  GetClientRect(h, &r);
  panel(dc, 0, 0, r.right, r.bottom, RGB(15, 22, 35));
  panel(dc, 0, 0, 210, r.bottom, RGB(10, 16, 27));
  SetBkMode(dc, TRANSPARENT);
  HFONT title = CreateFontW(30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  HFONT normal = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  HFONT small = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  HGDIOBJ old = SelectObject(dc, title);
  draw_text(dc, 28, 28, L"Onix360", RGB(235, 244, 255));
  SelectObject(dc, normal);
  const wchar_t* nav[] = {L"INICIO", L"BIBLIOTECA", L"ARQUIVOS", L"SAVE STATES", L"CONFIGURACOES"};
  for (int i = 0; i < 5; ++i) {
   if (i == selected_nav_) panel(dc, 12, 105 + i * 48, 186, 40, RGB(0, 116, 153));
   draw_text(dc, 28, 112 + i * 48, nav[i], RGB(235, 244, 255));
  }
  const wchar_t* headings[] = {L"INICIO", L"BIBLIOTECA DE JOGOS", L"ARQUIVOS DE JOGOS", L"SAVE STATES", L"CONFIGURACOES"};
  draw_text(dc, 250, 35, headings[selected_nav_], RGB(255, 255, 255));
  SelectObject(dc, small);
  draw_text(dc, (std::max)(260, static_cast<int>(r.right) - 190), 20, clock_time_.c_str(), RGB(105, 225, 245));
  draw_text(dc, (std::max)(260, static_cast<int>(r.right) - 190), 43, clock_date_.c_str(), RGB(170, 195, 215));
  SelectObject(dc, normal);
  draw_text(dc, 252, 78, L"Dashboard inspirado no Aurora | Onix360", RGB(130, 205, 225));
  panel(dc, 250, 125, r.right - 280, 135, RGB(23, 43, 62));

  if (selected_nav_ == 0) {
   draw_text(dc, 278, 155, L"BEM-VINDO AO Onix360", RGB(255, 255, 255));
   draw_text(dc, 280, 205, L"Emulador experimental Xbox 360 para Windows x64", RGB(174, 197, 215));
   const wchar_t* cards[] = {L"BIBLIOTECA", L"FORMATOS", L"SAVE STATES"};
   const wchar_t* details[] = {L"Gerencie sua colecao", L"ISO e XEX em desenvolvimento", L"10 slots CPU/RAM"};
   for (int i = 0; i < 3; ++i) {
    const int x = 250 + i * 250;
    panel(dc, x, 300, 225, 170, RGB(27, 39, 56));
    panel(dc, x, 300, 225, 7, RGB(0, 166, 204));
    draw_text(dc, x + 18, 340, cards[i], RGB(240, 247, 255));
    draw_text(dc, x + 18, 385, details[i], RGB(140, 165, 188));
   }
  } else if (selected_nav_ == 1) {
   draw_text(dc, 278, 155, L"SUA BIBLIOTECA", RGB(255, 255, 255));
   draw_text(dc, 280, 205, L"Arquivos ISO/XEX encontrados (clique em um XEX para ler):", RGB(174, 197, 215));
   if (games_.empty()) {
    draw_text(dc, 252, 300, L"Nenhum ISO ou XEX encontrado na pasta Onix360-data/roms.", RGB(190, 205, 220));
   } else {
    const int visible = std::min<int>(static_cast<int>(games_.size()), 8);
    for (int i = 0; i < visible; ++i) {
     const auto label = games_[static_cast<std::size_t>(i)].path.filename().wstring();
     draw_text(dc, 252, 280 + i * 34, label.c_str(), RGB(220, 232, 245));
    }
    if (games_.size() > 8)
     draw_text(dc, 252, 570, L"Mais jogos encontrados. Lista limitada a 8 itens nesta tela.", RGB(140, 165, 188));
   }
   if (!xex_message_.empty()) draw_text(dc, 252, 610, xex_message_.c_str(), RGB(120, 220, 190));
   draw_text(dc, 252, r.bottom - 68, rom_root_.wstring().c_str(), RGB(110, 135, 158));
  } else if (selected_nav_ == 2) {
   draw_text(dc, 278, 155, L"FORMATOS CONTENT DETECTADOS", RGB(255, 255, 255));
   draw_text(dc, 280, 205, L"Pastas GOD/XBLA/DLC/TU e candidatos STFS identificados:", RGB(174, 197, 215));
   if (content_entries_.empty()) {
    draw_text(dc, 252, 300, L"Nenhum item Content/GOD encontrado em Onix360-data/roms.", RGB(190, 205, 220));
   } else {
    const int visible = std::min<int>(static_cast<int>(content_entries_.size()), 7);
    for (int i = 0; i < visible; ++i) {
     const auto& entry = content_entries_[static_cast<std::size_t>(i)];
     auto label = std::wstring(entry.format.begin(), entry.format.end()) + L" | " + entry.path.filename().wstring();
     if (entry.stfs_header_valid) {
      label += L" | Title ID: " + std::to_wstring(entry.title_id);
      if (!entry.title_name.empty()) label += L" | " + std::wstring(entry.title_name.begin(), entry.title_name.end());
      if (entry.stfs_file_table_valid) label += L" | " + std::to_wstring(entry.stfs_file_count) + L" arquivos";
     }
     draw_text(dc, 252, 280 + i * 34, label.c_str(), RGB(220, 232, 245));
    }
   }
   draw_text(dc, 252, r.bottom - 68, L"Identificacao de pastas/arquivos apenas; parser STFS e execucao ainda pendentes.", RGB(140, 165, 188));
  } else if (selected_nav_ == 3) {
   draw_text(dc, 278, 155, L"10 SLOTS DISPONIVEIS NO GERENCIADOR", RGB(255, 255, 255));
   draw_text(dc, 280, 205, L"Save State atual: CPU Xenon e RAM.", RGB(174, 197, 215));
   for (int i = 0; i < 10; ++i) {
    const int col = i % 5, row = i / 5, x = 250 + col * 170, y = 300 + row * 105;
    panel(dc, x, y, 150, 78, RGB(27, 39, 56));
    draw_text(dc, x + 14, y + 18, (std::wstring(L"SLOT ") + std::to_wstring(i + 1)).c_str(), RGB(240, 247, 255));
    draw_text(dc, x + 14, y + 46, slot_labels_[static_cast<std::size_t>(i)].c_str(), RGB(140, 165, 188));
   }
   if (!slot_message_.empty()) draw_text(dc, 252, 540, slot_message_.c_str(), RGB(120, 220, 190));
   draw_text(dc, 252, 570, L"Selecione um slot para verificar checksum e snapshot; Delete apaga o slot.", RGB(140, 165, 188));
  } else {
   draw_text(dc, 278, 155, L"COMPATIBILIDADE E DESEMPENHO", RGB(255, 255, 255));
   draw_text(dc, 280, 205, L"Baseline x86-64 / SSE2; AVX2 nao e obrigatorio.", RGB(174, 197, 215));
   draw_text(dc, 252, 300, L"Perfil de memoria, renderer grafico e configuracoes por jogo ainda pendentes.", RGB(190, 205, 220));
   draw_text(dc, 252, 340, L"Meta: PCs Windows 10/11 com ate 4 GB RAM (sem garantia de jogos completos).", RGB(140, 165, 188));
  }
  draw_text(dc, 252, r.bottom - 38, L"Onix360 v0.7b | CPU Xenon em desenvolvimento | ESC para sair", RGB(110, 135, 158));
  SelectObject(dc, old);
  DeleteObject(title);
  DeleteObject(normal);
  DeleteObject(small);
  EndPaint(h, &ps);
 }

 int selected_nav_ = 0;
 int selected_slot_ = -1;
 std::array<std::wstring, state::StateSlots::kSlotCount> slot_labels_{};
 std::wstring slot_message_;
 std::wstring xex_message_;
 std::wstring clock_time_;
 std::wstring clock_date_;
 std::filesystem::path rom_root_;
 std::vector<storage::GameFile> games_;
 std::vector<storage::ContentEntry> content_entries_;
 state::StateSlots state_slots_{std::filesystem::path{}};
 HINSTANCE instance_{};
};
}
#endif
