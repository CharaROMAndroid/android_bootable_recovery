/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "recovery_ui/screen_ui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <aidl/android/hardware/health/BatteryStatus.h>

#include <android-base/chrono_utils.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include <health/utils.h>
#include <healthd/BatteryMonitor.h>

#include "minui/minui.h"
#include "otautil/paths.h"
#include "recovery_ui/device.h"
#include "recovery_ui/ui.h"

enum DirectRenderManager {
    DRM_INNER,
    DRM_OUTER,
};

namespace {
constexpr uint8_t kLightBgR = 0xed;
constexpr uint8_t kLightBgG = 0xf7;
constexpr uint8_t kLightBgB = 0xff;
constexpr uint8_t kLightTextR = 0x0f;
constexpr uint8_t kLightTextG = 0x17;
constexpr uint8_t kLightTextB = 0x2a;
constexpr uint8_t kLightHighlightR = 0xdc;
constexpr uint8_t kLightHighlightG = 0x3a;
constexpr uint8_t kLightHighlightB = 0x4f;
constexpr uint8_t kHeaderLightR = 0x1d;
constexpr uint8_t kHeaderLightG = 0x4e;
constexpr uint8_t kHeaderLightB = 0xd8;
constexpr uint8_t kHeaderDarkR = 0x60;
constexpr uint8_t kHeaderDarkG = 0xa5;
constexpr uint8_t kHeaderDarkB = 0xfa;

void GetRgbIndices(PixelFormat format, int* r, int* g, int* b) {
  if (format == PixelFormat::RGBA) {
    *r = 1;
    *g = 2;
    *b = 3;
  } else if (format == PixelFormat::ARGB || format == PixelFormat::ABGR ||
             format == PixelFormat::BGRA || format == PixelFormat::BGRX) {
    *r = 2;
    *g = 1;
    *b = 0;
  } else {
    *r = 0;
    *g = 1;
    *b = 2;
  }
}

void RecolorSurfaceForLightTheme(GRSurface* surface) {
  if (surface == nullptr || surface->pixel_bytes != 4) return;

  int r = 0;
  int g = 1;
  int b = 2;
  GetRgbIndices(gr_pixel_format(), &r, &g, &b);

  constexpr uint8_t kDarkThreshold = 8;
  constexpr uint8_t kOldLightBgR = 0xd0;
  constexpr uint8_t kOldLightBgG = 0xeb;
  constexpr uint8_t kOldLightBgB = 0xff;
  for (size_t y = 0; y < surface->height; ++y) {
    uint8_t* row = surface->data() + y * surface->row_bytes;
    for (size_t x = 0; x < surface->width; ++x) {
      uint8_t* p = row + x * 4;
      if ((p[r] <= kDarkThreshold && p[g] <= kDarkThreshold && p[b] <= kDarkThreshold) ||
          (p[r] == kOldLightBgR && p[g] == kOldLightBgG && p[b] == kOldLightBgB)) {
        p[r] = kLightBgR;
        p[g] = kLightBgG;
        p[b] = kLightBgB;
      }
    }
  }
}

std::unique_ptr<GRSurface> MakeTintedSurface(const std::unique_ptr<GRSurface>& surface,
                                             uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                                             uint8_t fg_r, uint8_t fg_g, uint8_t fg_b) {
  if (!surface || surface->pixel_bytes != 4) return nullptr;
  auto tinted = surface->Clone();
  if (!tinted) return nullptr;

  int r = 0;
  int g = 1;
  int b = 2;
  GetRgbIndices(gr_pixel_format(), &r, &g, &b);

  constexpr int kLow = 40;
  constexpr int kHigh = 200;
  const int denom = std::max(1, kHigh - kLow);

  for (size_t y = 0; y < tinted->height; ++y) {
    uint8_t* row = tinted->data() + y * tinted->row_bytes;
    for (size_t x = 0; x < tinted->width; ++x) {
      uint8_t* p = row + x * 4;
      int intensity = (static_cast<int>(p[r]) + static_cast<int>(p[g]) +
                       static_cast<int>(p[b])) /
                      3;
      if (intensity <= kLow) {
        p[r] = bg_r;
        p[g] = bg_g;
        p[b] = bg_b;
        continue;
      }
      if (intensity >= kHigh) {
        p[r] = fg_r;
        p[g] = fg_g;
        p[b] = fg_b;
        continue;
      }
      int t = (intensity - kLow);
      p[r] = static_cast<uint8_t>(bg_r + (fg_r - bg_r) * t / denom);
      p[g] = static_cast<uint8_t>(bg_g + (fg_g - bg_g) * t / denom);
      p[b] = static_cast<uint8_t>(bg_b + (fg_b - bg_b) * t / denom);
    }
  }
  return tinted;
}

std::unique_ptr<GRSurface> MakeLightSurface(const std::unique_ptr<GRSurface>& surface) {
  if (!surface) return nullptr;
  auto light = surface->Clone();
  if (!light) return nullptr;
  RecolorSurfaceForLightTheme(light.get());
  return light;
}



std::vector<std::string> WrapTextLines(const std::vector<std::string>& lines, size_t max_cols) {
  std::vector<std::string> out;
  if (max_cols == 0) {
    max_cols = 1;
  }
  for (const auto& line : lines) {
    size_t next_start = 0;
    while (next_start < line.size()) {
      std::string sub = line.substr(next_start, max_cols + 1);
      if (sub.size() <= max_cols) {
        next_start += sub.size();
      } else {
        size_t last_space = sub.find_last_of(" \t\n");
        if (last_space == std::string::npos) {
          sub.resize(max_cols);
          next_start += max_cols;
        } else {
          sub.resize(last_space);
          next_start += last_space + 1;
        }
      }
      out.emplace_back(std::move(sub));
    }
    if (line.empty()) {
      out.emplace_back();
    }
  }
  return out;
}
}  // namespace

// Return the current time as a double (including fractions of a second).
static double now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

namespace {

// ---------------------------------------------------------------------------------------------
// Material-style tonal palette (dark theme on an AMOLED-black background).
// Everything except the neutrals is derived from the accent, so a device only has to set
// ro.recovery.ui.accent_color / ro.recovery.ui.fastbootd_accent_color to re-theme recovery.
// ---------------------------------------------------------------------------------------------
struct Rgb {
  uint8_t r, g, b;
};

constexpr uint32_t kDefaultRecoveryAccent = 0xFE2712;
constexpr uint32_t kDefaultFastbootdAccent = 0xE65100;

constexpr Rgb kBlack{ 0x00, 0x00, 0x00 };
constexpr Rgb kWhite{ 0xFF, 0xFF, 0xFF };
constexpr Rgb kSurfaceContainer{ 0x24, 0x22, 0x28 };  // card / list item container
constexpr Rgb kOnSurface{ 0xE6, 0xE1, 0xE5 };         // primary text
constexpr Rgb kOnSurfaceVariant{ 0xCA, 0xC4, 0xD0 };  // secondary text (title, help)
constexpr Rgb kLogText{ 0xB4, 0xAE, 0xB9 };           // log buffer
constexpr Rgb kOnLightAccent{ 0x1C, 0x1B, 0x1F };     // text on a light accent
constexpr Rgb kError{ 0xF2, 0xB8, 0xB5 };             // M3 dark "error"

// Shape & layout tokens (dp).
constexpr int kMenuHorizontalMarginDp = 16;
constexpr int kCornerRadiusLargeDp = 20;  // outer corners of a group, selected item
constexpr int kCornerRadiusSmallDp = 4;   // inner corners between grouped items
constexpr int kScrollBarWidthDp = 4;

constexpr Rgb FromHex(uint32_t c) {
  return { static_cast<uint8_t>((c >> 16) & 0xFF), static_cast<uint8_t>((c >> 8) & 0xFF),
           static_cast<uint8_t>(c & 0xFF) };
}

// Linear blend: t = 0 returns |a|, t = 1 returns |b|.
Rgb Mix(const Rgb& a, const Rgb& b, float t) {
  auto lerp = [t](uint8_t from, uint8_t to) {
    const long v = std::lround(from + (static_cast<int>(to) - static_cast<int>(from)) * t);
    return static_cast<uint8_t>(std::clamp(v, 0L, 255L));
  };
  return { lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b) };
}

bool IsLight(const Rgb& c) {
  return (299 * c.r + 587 * c.g + 114 * c.b) / 1000 > 150;
}

// Reads "#RRGGBB" / "RRGGBB" from |prop|, falling back to |fallback| when unset or malformed.
uint32_t ReadColorProperty(const std::string& prop, uint32_t fallback) {
  std::string value = android::base::GetProperty(prop, "");
  if (!value.empty() && value[0] == '#') value.erase(0, 1);
  if (value.size() != 6 ||
      !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return isxdigit(ch); })) {
    return fallback;
  }
  return static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 16));
}

}  // namespace

Menu::Menu(size_t initial_selection, const DrawInterface& draw_func)
    : selection_(initial_selection), draw_funcs_(draw_func) {}

int Menu::selection() const {
  return selection_;
}

TextMenu::TextMenu(bool wrappable, size_t max_length,
                   const std::vector<std::string>& headers, const std::vector<std::string>& items,
                   size_t initial_selection, int char_height, const DrawInterface& draw_funcs)
    : Menu(initial_selection, draw_funcs),
      wrappable_(wrappable),
      calibrated_height_(false),
      max_display_items_(items.size()),
      max_item_length_(max_length),
      text_headers_(headers),
      menu_start_(0),
      char_height_(char_height) {

  size_t items_count = items.size();
  for (size_t i = 0; i < items_count; ++i) {
    text_items_.emplace_back(items[i].substr(0, max_item_length_));
  }

  CHECK(!text_items_.empty());
}

const std::vector<std::string>& TextMenu::text_headers() const {
  return text_headers_;
}

std::string TextMenu::TextItem(size_t index) const {
  CHECK_LT(index, text_items_.size());

  return text_items_[index];
}

size_t TextMenu::MenuStart() const {
  return menu_start_;
}

size_t TextMenu::MenuEnd() const {
  return std::min(ItemsCount(), menu_start_ + max_display_items_);
}

size_t TextMenu::ItemsCount() const {
  return text_items_.size();
}

bool TextMenu::ItemsOverflow(std::string* cur_selection_str) const {
  if (ItemsCount() <= max_display_items_) {
    return false;
  }

  *cur_selection_str =
      android::base::StringPrintf("Current item: %d/%zu", selection_ + 1, ItemsCount());
  return true;
}

// TODO(xunchang) modify the function parameters to button up & down.
int TextMenu::Select(int sel) {
  CHECK_LE(ItemsCount(), static_cast<size_t>(std::numeric_limits<int>::max()));
  int count = ItemsCount();

  int min = IsMain() ? 0 : -1; // -1 is back arrow

  if (sel < min) {
    selection_ = wrappable() ? count - 1 : min;
  } else if (sel >= count) {
    selection_ = wrappable() ? min : count - 1;
  } else {
    selection_ = sel;
  }

  if (selection_ >= 0) {
    if (selection_ < menu_start_) {
      menu_start_ = selection_;
    } else if (static_cast<size_t>(selection_) >= MenuEnd()) {
      menu_start_ = selection_ - max_display_items_ + 1;
    }
  }

  return selection_;
}

int TextMenu::SelectVisible(int relative_sel) {
  int sel = relative_sel;
  if (menu_start_ > 0) {
    sel += menu_start_;
  }

  return Select(sel);
}

int TextMenu::Scroll(int updown) {
  if ((updown > 0 && menu_start_ + max_display_items_ < ItemsCount()) ||
      (updown < 0 && menu_start_ > 0)) {
    menu_start_ += updown;

    /* We can receive a kInvokeItem event from a different source than touch,
       like from Power button. For this reason, selection should not get out of
       the screen. Constrain it to the first or last visible item of the list */
    if (selection_ < menu_start_) {
      selection_ = menu_start_;
    } else if (selection_ >= menu_start_ + max_display_items_) {
      selection_ = menu_start_ + max_display_items_ - 1;
    }
  }
  return selection_;
}

int TextMenu::DrawHeader(int x, int y) const {
  int offset = 0;

  draw_funcs_.SetColor(UIElement::HEADER);
  offset += draw_funcs_.DrawWrappedTextLines(x, y + offset, text_headers());

  return offset;
}

// Draws the visible items as one segmented group: every row is its own card separated by a thin
// gap, the group's outer corners use the large radius and the inner corners the small radius.
// The selected row keeps its position's shape, so the group's outline never changes.
int TextMenu::DrawItems(int x, int y, int width, bool long_press) const {
  const int padding = draw_funcs_.MenuItemPadding();
  const int spacing = draw_funcs_.MenuItemSpacing();
  const int item_height = draw_funcs_.MenuItemHeight();
  const int text_x = x + padding;

  int offset = draw_funcs_.MenuItemsTopGap();
  const int item_container_offset = offset;  // top of the first item, used for the scrollbar

  for (size_t i = MenuStart(); i < MenuEnd(); ++i) {
    if (i != MenuStart()) offset += spacing;

    const bool selected = static_cast<int>(i) == selection();
    const bool first = i == MenuStart();
    const bool last = i + 1 == MenuEnd();

    draw_funcs_.SetColor(selected ? (long_press ? UIElement::MENU_SEL_BG_ACTIVE
                                                : UIElement::MENU_SEL_BG)
                                  : UIElement::MENU_BG);
    draw_funcs_.DrawHighlightBar(x, y + offset, width, item_height, first, last);

    draw_funcs_.SetColor(selected ? UIElement::MENU_SEL_FG : UIElement::MENU);
    draw_funcs_.DrawTextLine(text_x,
                             y + offset + (item_height - char_height_ - 2 * padding) / 2,
                             TextItem(i), false /* bold */);
    offset += item_height;
  }

  std::string unused;
  if (ItemsOverflow(&unused)) {
    const int visible = static_cast<int>(max_display_items_);
    const int container_height = visible * (item_height + spacing) - spacing;
    const int steps = static_cast<int>(ItemsCount() - max_display_items_) + 1;
    const int bar_height = std::max(1, container_height / steps);
    const int start_y = y + item_container_offset + bar_height * static_cast<int>(menu_start_);
    draw_funcs_.SetColor(UIElement::SCROLLBAR);
    draw_funcs_.DrawScrollBar(start_y, bar_height);
  }

  return offset;
}

GraphicMenu::GraphicMenu(const GRSurface* graphic_headers,
                         const std::vector<const GRSurface*>& graphic_items,
                         size_t initial_selection, const DrawInterface& draw_funcs)
    : Menu(initial_selection, draw_funcs) {
  graphic_headers_ = graphic_headers->Clone();
  graphic_items_.reserve(graphic_items.size());
  for (const auto& item : graphic_items) {
    graphic_items_.emplace_back(item->Clone());
  }
}

// Define destructor out-of-line so that GRSurface is defined for unique_ptr.
GraphicMenu::~GraphicMenu() = default;

int GraphicMenu::Select(int sel) {
  CHECK_LE(graphic_items_.size(), static_cast<size_t>(std::numeric_limits<int>::max()));
  int count = graphic_items_.size();

  // Wraps the selection at boundary if the menu is not scrollable.
  if (sel < 0) {
    selection_ = count - 1;
  } else if (sel >= count) {
    selection_ = 0;
  } else {
    selection_ = sel;
  }

  return selection_;
}

int GraphicMenu::DrawHeader(int x, int y) const {
  draw_funcs_.SetColor(UIElement::HEADER);
  draw_funcs_.DrawTextIcon(x, y, graphic_headers_.get());
  return graphic_headers_->height;
}

int GraphicMenu::DrawItems(int x, int y, int width, bool long_press) const {
  const int padding = draw_funcs_.MenuItemPadding();
  const int spacing = draw_funcs_.MenuItemSpacing();
  const int text_x = x + padding;
  const size_t count = graphic_items_.size();

  int offset = draw_funcs_.MenuItemsTopGap();

  for (size_t i = 0; i < count; i++) {
    if (i != 0) offset += spacing;

    auto& item = graphic_items_[i];
    const bool selected = static_cast<int>(i) == selection_;
    const int item_height = item->height + padding;

    draw_funcs_.SetColor(selected ? (long_press ? UIElement::MENU_SEL_BG_ACTIVE
                                                : UIElement::MENU_SEL_BG)
                                  : UIElement::MENU_BG);
    draw_funcs_.DrawHighlightBar(x, y + offset, width, item_height, i == 0, i + 1 == count);

    draw_funcs_.SetColor(selected ? UIElement::MENU_SEL_FG : UIElement::MENU);
    draw_funcs_.DrawTextIcon(text_x, y + offset + padding / 2, item.get());
    offset += item_height;
  }

  return offset;
}

size_t GraphicMenu::ItemsCount() const {
  return graphic_items_.size();
}

bool GraphicMenu::Validate(size_t max_width, size_t max_height, const GRSurface* graphic_headers,
                           const std::vector<const GRSurface*>& graphic_items) {
  int offset = 0;
  if (!ValidateGraphicSurface(max_width, max_height, offset, graphic_headers)) {
    return false;
  }
  offset += graphic_headers->height;

  for (const auto& item : graphic_items) {
    if (!ValidateGraphicSurface(max_width, max_height, offset, item)) {
      return false;
    }
    offset += item->height;
  }

  return true;
}

bool GraphicMenu::ValidateGraphicSurface(size_t max_width, size_t max_height, int y,
                                         const GRSurface* surface) {
  if (!surface) {
    fprintf(stderr, "Graphic surface can not be null\n");
    return false;
  }

  if (surface->pixel_bytes != 1 || surface->width != surface->row_bytes) {
    fprintf(stderr, "Invalid graphic surface, pixel bytes: %zu, width: %zu row_bytes: %zu\n",
            surface->pixel_bytes, surface->width, surface->row_bytes);
    return false;
  }

  if (surface->width > max_width || surface->height > max_height - y) {
    fprintf(stderr,
            "Graphic surface doesn't fit into the screen. width: %zu, height: %zu, max_width: %zu,"
            " max_height: %zu, vertical offset: %d\n",
            surface->width, surface->height, max_width, max_height, y);
    return false;
  }

  return true;
}

MenuDrawFunctions::MenuDrawFunctions(const DrawInterface& wrappee)
    : wrappee_(wrappee) {
}

int MenuDrawFunctions::DrawTextLine(int x, int y, const std::string& line, bool bold) const {
  gr_text(gr_menu_font(), x, y + MenuItemPadding(), line.c_str(), bold);
  return 2 * MenuItemPadding() + MenuCharHeight();
}

int MenuDrawFunctions::DrawTextLines(int x, int y, const std::vector<std::string>& lines) const {
  int offset = 0;
  for (const auto& line : lines) {
    offset += DrawTextLine(x, y + offset, line, false);
  }
  return offset;
}

int MenuDrawFunctions::DrawWrappedTextLines(int x, int y, const std::vector<std::string>& lines) const {
  const int padding = MenuItemPadding() / 2;

  // Keep symmetrical margins based on the given offset (i.e. x).
  size_t text_cols = (gr_fb_width() - x * 2) / MenuCharWidth();
  int offset = 0;
  for (const auto& line : lines) {
    size_t next_start = 0;
    while (next_start < line.size()) {
      std::string sub = line.substr(next_start, text_cols + 1);
      if (sub.size() <= text_cols) {
        next_start += sub.size();
      } else {
        // Line too long and must be wrapped to text_cols columns.
        size_t last_space = sub.find_last_of(" \t\n");
        if (last_space == std::string::npos) {
          // No space found, just draw as much as we can.
          sub.resize(text_cols);
          next_start += text_cols;
        } else {
          sub.resize(last_space);
          next_start += last_space + 1;
        }
      }
      offset += DrawTextLine(x, y + offset, sub, false) - (2 * MenuItemPadding() - padding);
    }
  }
  if (!lines.empty()) {
    offset += 2 * MenuItemPadding() - padding;
  }
  return offset;
}

constexpr int kDefaultMarginHeight = 0;
constexpr int kDefaultMarginWidth = 0;
constexpr int kDefaultAnimationFps = 30;

ScreenRecoveryUI::ScreenRecoveryUI()
    : margin_width_(
          android::base::GetIntProperty("ro.recovery.ui.margin_width", kDefaultMarginWidth)),
      margin_height_(
          android::base::GetIntProperty("ro.recovery.ui.margin_height", kDefaultMarginHeight)),
      animation_fps_(
          android::base::GetIntProperty("ro.recovery.ui.animation_fps", kDefaultAnimationFps)),
      density_(static_cast<float>(android::base::GetIntProperty("ro.sf.lcd_density", 160)) / 160.f),
      blank_unblank_on_init_(
          android::base::GetBoolProperty("ro.recovery.ui.blank_unblank_on_init", false)),
      recovery_accent_(ReadColorProperty("ro.recovery.ui.accent_color", kDefaultRecoveryAccent)),
      fastbootd_accent_(
          ReadColorProperty("ro.recovery.ui.fastbootd_accent_color", kDefaultFastbootdAccent)),
      current_icon_(NONE),
      current_frame_(0),
      intro_done_(false),
      progressBarType(EMPTY),
      progressScopeStart(0),
      progressScopeSize(0),
      progress(0),
      pagesIdentical(false),
      screen_dirty_(false),
      text_cols_(0),
      text_rows_(0),
      text_(nullptr),
      text_col_(0),
      text_row_(0),
      show_text(false),
      show_text_ever(false),
      file_viewer_text_(nullptr),
      stage(-1),
      max_stage(-1),
      locale_(""),
      rtl_locale_(false),
      batt_capacity_(0),
      charging_(false),
      is_graphics_available(false) {}

ScreenRecoveryUI::~ScreenRecoveryUI() {
  batt_monitor_thread_stopped_ = true;
  if (batt_monitor_thread_.joinable()) {
    batt_monitor_thread_.join();
  }

  progress_thread_stopped_ = true;
  if (progress_thread_.joinable()) {
    progress_thread_.join();
  }
  // No-op if gr_init() (via Init()) was not called or had failed.
  gr_exit();
}

const GRSurface* ScreenRecoveryUI::GetCurrentFrame() const {
  if (current_icon_ == INSTALLING_UPDATE || current_icon_ == ERASING) {
    return intro_done_ ? loop_frames_[current_frame_].get() : intro_frames_[current_frame_].get();
  }
  return error_icon_.get();
}

const GRSurface* ScreenRecoveryUI::GetCurrentText() const {
  switch (current_icon_) {
    case ERASING:
      return erasing_text_.get();
    case ERROR:
      return error_text_.get();
    case INSTALLING_UPDATE:
      return installing_text_.get();
    case NO_COMMAND:
      return no_command_text_.get();
    case NONE:
      abort();
  }
}

int ScreenRecoveryUI::PixelsFromDp(int dp) const {
  return dp * density_;
}

int ScreenRecoveryUI::MenuHorizontalMargin() const {
  return PixelsFromDp(kMenuHorizontalMarginDp);
}

// Here's the intended layout:

//          | portrait    large        landscape      large
// ---------+-------------------------------------------------
//      gap |
// icon     |                   (200dp)
//      gap |    68dp      68dp             56dp      112dp
// text     |                    (14sp)
//      gap |    32dp      32dp             26dp       52dp
// progress |                     (2dp)
//      gap |

// Note that "baseline" is actually the *top* of each icon (because that's how our drawing routines
// work), so that's the more useful measurement for calling code. We use even top and bottom gaps.

enum Layout { PORTRAIT = 0, PORTRAIT_LARGE = 1, LANDSCAPE = 2, LANDSCAPE_LARGE = 3, LAYOUT_MAX };
enum Dimension { TEXT = 0, ICON = 1, DIMENSION_MAX };
static constexpr int kLayouts[LAYOUT_MAX][DIMENSION_MAX] = {
  { 32, 68 },   // PORTRAIT
  { 32, 68 },   // PORTRAIT_LARGE
  { 26, 56 },   // LANDSCAPE
  { 52, 112 },  // LANDSCAPE_LARGE
};

int ScreenRecoveryUI::GetAnimationBaseline() const {
  return GetTextBaseline() - PixelsFromDp(kLayouts[layout_][ICON]) -
         gr_get_height(loop_frames_[0].get());
}

int ScreenRecoveryUI::GetTextBaseline() const {
  return GetProgressBaseline() - PixelsFromDp(kLayouts[layout_][TEXT]) -
         gr_get_height(installing_text_.get());
}

int ScreenRecoveryUI::GetProgressBaseline() const {
  int elements_sum = gr_get_height(loop_frames_[0].get()) + PixelsFromDp(kLayouts[layout_][ICON]) +
                     gr_get_height(installing_text_.get()) + PixelsFromDp(kLayouts[layout_][TEXT]) +
                     gr_get_height(progress_bar_fill_.get());
  int bottom_gap = (ScreenHeight() - elements_sum) / 2;
  return ScreenHeight() - bottom_gap - gr_get_height(progress_bar_fill_.get());
}

// Clear the screen and draw the currently selected background icon (if any).
// Should only be called with updateMutex locked.
void ScreenRecoveryUI::draw_background_locked() {
  pagesIdentical = false;
  if (theme_ == Theme::LIGHT) {
    gr_color(kLightBgR, kLightBgG, kLightBgB, 255);
  } else {
    gr_color(0, 0, 0, 255);
  }
  gr_clear();
  if (current_icon_ != NONE) {
    if (max_stage != -1) {
      int stage_height = gr_get_height(stage_marker_empty_.get());
      int stage_width = gr_get_width(stage_marker_empty_.get());
      int x = (ScreenWidth() - max_stage * gr_get_width(stage_marker_empty_.get())) / 2;
      int y = ScreenHeight() - stage_height - margin_height_;
      for (int i = 0; i < max_stage; ++i) {
        const auto& stage_surface = (i < stage) ? stage_marker_fill_ : stage_marker_empty_;
        DrawSurface(stage_surface.get(), 0, 0, stage_width, stage_height, x, y);
        x += stage_width;
      }
    }

    const auto& text_surface = GetCurrentText();
    int text_x = (ScreenWidth() - gr_get_width(text_surface)) / 2;
    int text_y = GetTextBaseline();
    if (theme_ == Theme::LIGHT) {
      gr_color(0, 0, 0, 255);
    } else {
      gr_color(255, 255, 255, 255);
    }
    DrawTextIcon(text_x, text_y, text_surface);
  }
}

// Draws the animation and progress bar (if any) on the screen. Does not flip pages. Should only be
// called with updateMutex locked.
void ScreenRecoveryUI::draw_foreground_locked() {
  if (current_icon_ != NONE) {
    const auto& frame = GetCurrentFrame();
    int frame_width = gr_get_width(frame);
    int frame_height = gr_get_height(frame);
    int frame_x = (ScreenWidth() - frame_width) / 2;
    int frame_y = GetAnimationBaseline();
    if (frame_x >= 0 && frame_y >= 0 && (frame_x + frame_width) < ScreenWidth() &&
        (frame_y + frame_height) < ScreenHeight())
      DrawSurface(frame, 0, 0, frame_width, frame_height, frame_x, frame_y);
  }

  if (progressBarType != EMPTY) {
    int width = gr_get_width(progress_bar_empty_.get());
    int height = gr_get_height(progress_bar_empty_.get());

    int progress_x = (ScreenWidth() - width) / 2;
    int progress_y = GetProgressBaseline();

    // Erase behind the progress bar (in case this was a progress-only update)
    if (theme_ == Theme::LIGHT) {
      gr_color(kLightBgR, kLightBgG, kLightBgB, 255);
    } else {
      gr_color(0, 0, 0, 255);
    }
    DrawFill(progress_x, progress_y, width, height);

    if (progressBarType == DETERMINATE) {
      float p = progressScopeStart + progress * progressScopeSize;
      int pos = static_cast<int>(p * width);

      if (rtl_locale_) {
        // Fill the progress bar from right to left.
        if (pos > 0) {
          DrawSurface(progress_bar_fill_.get(), width - pos, 0, pos, height,
                      progress_x + width - pos, progress_y);
        }
        if (pos < width - 1) {
          DrawSurface(progress_bar_empty_.get(), 0, 0, width - pos, height, progress_x, progress_y);
        }
      } else {
        // Fill the progress bar from left to right.
        if (pos > 0) {
          DrawSurface(progress_bar_fill_.get(), 0, 0, pos, height, progress_x, progress_y);
        }
        if (pos < width - 1) {
          DrawSurface(progress_bar_empty_.get(), pos, 0, width - pos, height, progress_x + pos,
                      progress_y);
        }
      }
    }
  }
}

void ScreenRecoveryUI::SetDrawColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) const {
  draw_color_[0] = r;
  draw_color_[1] = g;
  draw_color_[2] = b;
  draw_color_[3] = a;
  gr_color(r, g, b, a);
}

// Palette roles (dark):
//   MENU_BG            surfaceContainer, lightly tinted with the accent
//   MENU_SEL_BG        accent (primary)
//   MENU_SEL_BG_ACTIVE accent + pressed state layer (long-press feedback)
//   MENU_SEL_FG        onPrimary, picked for contrast against the accent
//   HEADER/SCROLLBAR   accent at a lighter tone
//   MENU / INFO / LOG  onSurface / onSurfaceVariant / muted log text
void ScreenRecoveryUI::SetColor(UIElement e) const {
  const char* property = fastbootd_logo_enabled_ ? "ro.recovery.ui.fastbootd_accent_color"
                                                 : "ro.recovery.ui.accent_color";
  if (theme_ == Theme::LIGHT || android::base::GetProperty(property, "").empty()) {

    if (theme_ == Theme::LIGHT) {
      switch (e) {
        case UIElement::BATTERY_LOW:
          SetDrawColor(0xfd, 0x35, 0x35, 255);
          break;
        case UIElement::INFO:
        case UIElement::MENU:
        case UIElement::LOG:
          SetDrawColor(kLightTextR, kLightTextG, kLightTextB, 255);
          break;
        case UIElement::HEADER:
          SetDrawColor(kHeaderLightR, kHeaderLightG, kHeaderLightB, 255);
          break;
        case UIElement::MENU_BG:
          SetDrawColor(0xff, 0xff, 0xff, 230);
          break;
        case UIElement::MENU_SEL_BG:
        case UIElement::SCROLLBAR:
          SetDrawColor(kLightHighlightR, kLightHighlightG, kLightHighlightB, 255);
          break;
        case UIElement::MENU_SEL_BG_ACTIVE:
          SetDrawColor(kLightHighlightR, kLightHighlightG, kLightHighlightB, 255);
          break;
        case UIElement::MENU_SEL_FG:
          SetDrawColor(kLightTextR, kLightTextG, kLightTextB, 255);
          break;
        case UIElement::TEXT_FILL:
          SetDrawColor(255, 255, 255, 160);
          break;
        default:
          SetDrawColor(kLightTextR, kLightTextG, kLightTextB, 255);
          break;
      }
      return;
    }

    switch (e) {
      case UIElement::BATTERY_LOW:
        if (fastbootd_logo_enabled_)
          SetDrawColor(0xfd, 0x35, 0x35, 255);
        else
          SetDrawColor(0xc7, 0x15, 0x85, 255);
        break;
      case UIElement::INFO:
        SetDrawColor(255, 255, 255, 255);
        break;
      case UIElement::HEADER:
        SetDrawColor(kHeaderDarkR, kHeaderDarkG, kHeaderDarkB, 255);
        break;
        case UIElement::MENU:
          SetDrawColor(0xd8, 0xd8, 0xd8, 255);
          break;
        case UIElement::MENU_BG:
          SetDrawColor(0x20, 0x20, 0x20, 220);
          break;
      case UIElement::MENU_SEL_BG:
      case UIElement::SCROLLBAR:
        SetDrawColor(177, 18, 38, 255);
        break;
      case UIElement::MENU_SEL_BG_ACTIVE:
        if (fastbootd_logo_enabled_)
          SetDrawColor(0xe6, 0x51, 0x00, 255);
        else
          SetDrawColor(0x7c, 0x4d, 0xff, 255);
        break;
      case UIElement::MENU_SEL_FG:
        SetDrawColor(0xd8, 0xd8, 0xd8, 255);
        break;
      case UIElement::LOG:
        SetDrawColor(196, 196, 196, 255);
        break;
      case UIElement::TEXT_FILL:
        SetDrawColor(0, 0, 0, 160);
        break;
      default:
        SetDrawColor(255, 255, 255, 255);
        break;
    }
    return;
  }

  const Rgb accent = FromHex(fastbootd_logo_enabled_ ? fastbootd_accent_ : recovery_accent_);
  auto apply = [this](const Rgb& c, uint8_t alpha) { SetDrawColor(c.r, c.g, c.b, alpha); };

  switch (e) {
    case UIElement::BATTERY_LOW:
      apply(kError, 255);
      break;
    case UIElement::INFO:
      apply(kOnSurfaceVariant, 255);
      break;
    case UIElement::HEADER:
      apply(Mix(accent, kWhite, 0.35f), 255);
      break;
    case UIElement::MENU:
      apply(kOnSurface, 255);
      break;
    case UIElement::MENU_BG:
      apply(Mix(kSurfaceContainer, accent, 0.08f), 255);
      break;
    case UIElement::MENU_SEL_BG:
      apply(accent, 255);
      break;
    case UIElement::MENU_SEL_BG_ACTIVE:
      apply(Mix(accent, kWhite, 0.24f), 255);
      break;
    case UIElement::MENU_SEL_FG:
      apply(IsLight(accent) ? kOnLightAccent : kWhite, 255);
      break;
    case UIElement::SCROLLBAR:
      apply(Mix(accent, kWhite, 0.35f), 255);
      break;
    case UIElement::LOG:
      apply(kLogText, 255);
      break;
    case UIElement::TEXT_FILL:
      apply(kBlack, 160);
      break;
    default:
      apply(kWhite, 255);
      break;
  }
}

void ScreenRecoveryUI::SelectAndShowBackgroundText(const std::vector<std::string>& locales_entries,
                                                   size_t sel) {
  SetLocale(locales_entries[sel]);
  std::vector<std::string> text_name = { "erasing_text", "error_text", "installing_text",
                                         "installing_security_text", "no_command_text" };
  std::unordered_map<std::string, std::unique_ptr<GRSurface>> surfaces;
  for (const auto& name : text_name) {
    auto text_image = LoadLocalizedBitmap(name);
    if (!text_image) {
      Print("Failed to load %s\n", name.c_str());
      return;
    }
    surfaces.emplace(name, std::move(text_image));
  }

  std::lock_guard<std::mutex> lg(updateMutex);
  if (theme_ == Theme::LIGHT) {
    gr_color(kLightBgR, kLightBgG, kLightBgB, 255);
  } else {
    gr_color(0, 0, 0, 255);
  }
  gr_clear();

  int text_y = margin_height_;
  int text_x = margin_width_;
  int line_spacing = gr_sys_font()->char_height;  // Put some extra space between images.
  // Write the header and descriptive texts.
  SetColor(UIElement::INFO);
  std::string header = "Show background text image";
  text_y += DrawTextLine(text_x, text_y, header, true);
  std::string locale_selection = android::base::StringPrintf(
      "Current locale: %s, %zu/%zu", locales_entries[sel].c_str(), sel + 1, locales_entries.size());
  // clang-format off
  std::vector<std::string> instruction = {
    locale_selection,
    "Use volume up/down to switch locales and power to exit."
  };
  // clang-format on
  text_y += DrawWrappedTextLines(text_x, text_y, instruction);

  // Iterate through the text images and display them in order for the current locale.
  for (const auto& p : surfaces) {
    text_y += line_spacing;
    SetColor(UIElement::LOG);
    text_y += DrawTextLine(text_x, text_y, p.first, false);
    if (theme_ == Theme::LIGHT) {
      gr_color(0, 0, 0, 255);
    } else {
      gr_color(255, 255, 255, 255);
    }
    gr_texticon(text_x, text_y, p.second.get());
    text_y += gr_get_height(p.second.get());
  }
  // Update the whole screen.
  gr_flip();
}

void ScreenRecoveryUI::CheckBackgroundTextImages() {
  // Load a list of locales embedded in one of the resource files.
  std::vector<std::string> locales_entries = get_locales_in_png("installing_text");
  if (locales_entries.empty()) {
    Print("Failed to load locales from the resource files\n");
    return;
  }
  std::string saved_locale = locale_;
  size_t selected = 0;
  SelectAndShowBackgroundText(locales_entries, selected);

  FlushKeys();
  while (true) {
    InputEvent evt = WaitInputEvent();
    if (evt.type() == EventType::EXTRA) {
      if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) break;
    }
    if (evt.type() == EventType::KEY) {
      if (evt.key() == KEY_POWER || evt.key() == KEY_ENTER) {
        break;
      } else if (evt.key() == KEY_UP || evt.key() == KEY_VOLUMEUP) {
        selected = (selected == 0) ? locales_entries.size() - 1 : selected - 1;
        SelectAndShowBackgroundText(locales_entries, selected);
      } else if (evt.key() == KEY_DOWN || evt.key() == KEY_VOLUMEDOWN) {
        selected = (selected == locales_entries.size() - 1) ? 0 : selected + 1;
        SelectAndShowBackgroundText(locales_entries, selected);
      }
    }
  }

  SetLocale(saved_locale);
}

int ScreenRecoveryUI::ScreenWidth() const {
  return gr_fb_width();
}

int ScreenRecoveryUI::ScreenHeight() const {
  return gr_fb_height();
}

void ScreenRecoveryUI::DrawSurface(const GRSurface* surface, int sx, int sy, int w, int h, int dx,
                                   int dy) const {
  gr_blit(surface, sx, sy, w, h, dx, dy);
}

int ScreenRecoveryUI::DrawHorizontalRule(int y) const {
  gr_fill(0, y + 4, ScreenWidth(), y + 6);
  return 8;
}

// Scanline rasterizer for a rectangle with circular top/bottom corners. Interior spans are filled
// with the current color in one gr_fill() per row; only the pixels the curve actually crosses are
// blended individually, with alpha proportional to their coverage. Clipping is done against the
// whole framebuffer because minui's gr_fill() drops rectangles that are partly off-screen.
void ScreenRecoveryUI::DrawRoundedRect(int x, int y, int width, int height, int radius_top,
                                       int radius_bottom) const {
  if (width <= 0 || height <= 0) return;

  const int max_radius = std::min(width, height) / 2;
  radius_top = std::clamp(radius_top, 0, max_radius);
  radius_bottom = std::clamp(radius_bottom, 0, max_radius);

  const int fb_width = gr_fb_width();
  const int fb_height = gr_fb_height();
  const uint8_t r = draw_color_[0];
  const uint8_t g = draw_color_[1];
  const uint8_t b = draw_color_[2];
  const uint8_t a = draw_color_[3];
  bool color_changed = false;

  auto fill = [&](int x1, int y1, int x2, int y2) {
    x1 = std::max(x1, 0);
    y1 = std::max(y1, 0);
    x2 = std::min(x2, fb_width);
    y2 = std::min(y2, fb_height);
    if (x2 <= x1 || y2 <= y1) return;
    if (color_changed) {
      // An edge pixel was blended with partial alpha; restore the solid color first.
      gr_color(r, g, b, a);
      color_changed = false;
    }
    gr_fill(x1, y1, x2, y2);
  };

  auto blend = [&](int px, int row, double coverage) {
    if (px < 0 || px >= fb_width || coverage <= 0.0) return;
    const int alpha = static_cast<int>(std::lround(a * std::min(coverage, 1.0)));
    if (alpha <= 0) return;
    gr_color(r, g, b, static_cast<uint8_t>(alpha));
    gr_fill(px, row, px + 1, row + 1);
    color_changed = true;
  };

  // Coverage of a pixel whose center is (dx, dy) away from the corner circle's center.
  auto coverage = [](double dx, double dy, int radius) {
    const double d = std::sqrt(std::max(0.0, dx) * std::max(0.0, dx) + dy * dy);
    return std::clamp(radius + 0.5 - d, 0.0, 1.0);
  };

  auto draw_corner_row = [&](int row, int radius, double dy) {
    if (row < 0 || row >= fb_height) return;

    const double cx_left = x + radius;
    const double cx_right = x + width - radius;
    const double r_in = radius - 0.5;
    const double r_out = radius + 0.5;
    const double dx_in = std::sqrt(std::max(0.0, r_in * r_in - dy * dy));
    const double dx_out = std::sqrt(std::max(0.0, r_out * r_out - dy * dy));
    const int mid = x + width / 2;

    // Fully covered span.
    const int solid_left = std::max(x, static_cast<int>(std::ceil(cx_left - dx_in - 0.5)));
    const int solid_right =
        std::min(x + width, static_cast<int>(std::floor(cx_right + dx_in - 0.5)) + 1);
    if (solid_right > solid_left) fill(solid_left, row, solid_right, row + 1);

    // Anti-aliased edge pixels. When there is no solid span (pill ends), split at |mid| so no
    // pixel is blended twice.
    const int left_begin =
        std::max(x, static_cast<int>(std::floor(cx_left - dx_out - 0.5)) + 1);
    const int left_end = std::min(solid_left, mid);
    for (int px = left_begin; px < left_end; ++px) {
      blend(px, row, coverage(cx_left - (px + 0.5), dy, radius));
    }

    const int right_begin = std::max(solid_right, mid);
    const int right_end =
        std::min(x + width, static_cast<int>(std::ceil(cx_right + dx_out - 0.5)));
    for (int px = right_begin; px < right_end; ++px) {
      blend(px, row, coverage((px + 0.5) - cx_right, dy, radius));
    }
  };

  // Straight middle section in a single call.
  fill(x, y + radius_top, x + width, y + height - radius_bottom);

  for (int i = 0; i < radius_top; ++i) {
    draw_corner_row(y + i, radius_top, radius_top - (i + 0.5));
  }
  for (int i = 0; i < radius_bottom; ++i) {
    draw_corner_row(y + height - radius_bottom + i, radius_bottom, i + 0.5);
  }

  if (color_changed) gr_color(r, g, b, a);
}

void ScreenRecoveryUI::DrawHighlightBar(int x, int y, int width, int height) const {
  DrawHighlightBar(x, y, width, height, true, true);
}

void ScreenRecoveryUI::DrawHighlightBar(int x, int y, int width, int height, bool round_top,
                                        bool round_bottom) const {
  const int large = PixelsFromDp(kCornerRadiusLargeDp);
  const int small = PixelsFromDp(kCornerRadiusSmallDp);
  DrawRoundedRect(x, y, width, height, round_top ? large : small, round_bottom ? large : small);
}

void ScreenRecoveryUI::DrawScrollBar(int y, int height) const {
  // Pill-shaped thumb centered in the right-hand gutter next to the cards.
  const int width = std::max(2, PixelsFromDp(kScrollBarWidthDp));
  const int x = ScreenWidth() - margin_width_ - MenuHorizontalMargin() / 2 - width / 2;
  DrawRoundedRect(x, y, width, height, width / 2, width / 2);
}

void ScreenRecoveryUI::DrawFill(int x, int y, int w, int h) const {
  gr_fill(x, y, w, h);
}

void ScreenRecoveryUI::DrawTextIcon(int x, int y, const GRSurface* surface) const {
  gr_texticon(x, y, surface);
}

int ScreenRecoveryUI::DrawTextLine(int x, int y, const std::string& line, bool bold) const {
  gr_text(gr_sys_font(), x, y, line.c_str(), bold);
  return char_height_ + 4;
}

int ScreenRecoveryUI::DrawInfoTextLine(int x, int y, const std::string& line, bool bold) const {
  const GRFont* font = gr_sys_font();
  GRFont info_font{};
  int line_height = char_height_;
  if (info_font_surface_) {
    info_font.texture = info_font_surface_.get();
    info_font.char_width = info_char_width_;
    info_font.char_height = info_char_height_;
    font = &info_font;
    line_height = info_char_height_;
  }
  gr_text(font, x, y, line.c_str(), bold);
  return line_height + 4;
}

int ScreenRecoveryUI::DrawTextLines(int x, int y, const std::vector<std::string>& lines) const {
  int offset = 0;
  for (const auto& line : lines) {
    offset += DrawTextLine(x, y + offset, line, false);
  }
  return offset;
}

int ScreenRecoveryUI::DrawInfoTextLineTight(int x, int y, const std::string& line, bool bold) const {
  const GRFont* font = gr_sys_font();
  GRFont info_font{};
  int line_height = char_height_;
  if (info_font_surface_) {
    info_font.texture = info_font_surface_.get();
    info_font.char_width = info_char_width_;
    info_font.char_height = info_char_height_;
    font = &info_font;
    line_height = info_char_height_;
  }
  if (font == nullptr || font->char_width <= 0) {
    return DrawInfoTextLine(x, y, line, bold);
  }
  int advance = font->char_width;
  int tighten = std::max(1, font->char_width / 8);
  if (advance > tighten) {
    advance -= tighten;
  }
  int cur_x = x;
  for (unsigned char ch : line) {
    char s[2] = { static_cast<char>(ch), 0 };
    gr_text(font, cur_x, y, s, bold);
    cur_x += advance;
  }
  return line_height + 4;
}

int ScreenRecoveryUI::DrawInfoTextLinesTight(int x, int y,
                                             const std::vector<std::string>& lines) const {
  int offset = 0;
  for (const auto& line : lines) {
    offset += DrawInfoTextLineTight(x, y + offset, line, false);
  }
  return offset;
}

int ScreenRecoveryUI::DrawWrappedTextLines(int x, int y,
                                           const std::vector<std::string>& lines) const {
  // Keep symmetrical margins based on the given offset (i.e. x).
  size_t text_cols = (ScreenWidth() - x * 2) / char_width_;
  int offset = 0;
  for (const auto& line : lines) {
    size_t next_start = 0;
    while (next_start < line.size()) {
      std::string sub = line.substr(next_start, text_cols + 1);
      if (sub.size() <= text_cols) {
        next_start += sub.size();
      } else {
        // Line too long and must be wrapped to text_cols columns.
        size_t last_space = sub.find_last_of(" \t\n");
        if (last_space == std::string::npos) {
          // No space found, just draw as much as we can.
          sub.resize(text_cols);
          next_start += text_cols;
        } else {
          sub.resize(last_space);
          next_start += last_space + 1;
        }
      }
      offset += DrawTextLine(x, y + offset, sub, false);
    }
  }
  return offset;
}

void ScreenRecoveryUI::SetTitle(const std::vector<std::string>& lines) {
  title_lines_ = lines;
}

std::vector<std::string> ScreenRecoveryUI::GetMenuHelpMessage() const {
  // clang-format off
  static std::vector<std::string> REGULAR_HELP{
    "Use the volume up/down keys to navigate.",
    "Use the power key to select.",
  };
  static std::vector<std::string> LONG_PRESS_HELP{
    "Any button cycles highlight.",
    "Long-press activates.",
  };
  static const std::vector<std::string> NO_HELP = {};
  // clang-format on
  return HasTouchScreen() ? NO_HELP : HasThreeButtons() ? REGULAR_HELP : LONG_PRESS_HELP;
}

// Redraws everything on the screen. Does not flip pages. Should only be called with updateMutex
// locked.
void ScreenRecoveryUI::draw_screen_locked() {
  if (!show_text) {
    draw_background_locked();
    draw_foreground_locked();
    return;
  }

  if (theme_ == Theme::LIGHT) {
    gr_color(kLightBgR, kLightBgG, kLightBgB, 255);
  } else {
    gr_color(0, 0, 0, 255);
  }
  gr_clear();

  draw_menu_and_text_buffer_locked(GetMenuHelpMessage());
  draw_battery_capacity_locked();
}

// Draws the menu and text buffer on the screen. Should only be called with updateMutex locked.
void ScreenRecoveryUI::draw_menu_and_text_buffer_locked(
    const std::vector<std::string>& help_message) {
  int y = margin_height_;

  if (menu_) {
    GRSurface* logo = nullptr;
    if (fastbootd_logo_enabled_) {
      logo = (theme_ == Theme::LIGHT && fastbootd_logo_light_) ? fastbootd_logo_light_.get()
                                                               : fastbootd_logo_.get();
    } else {
      logo = (theme_ == Theme::LIGHT && chara_logo_light_) ? chara_logo_light_.get()
                                                             : chara_logo_.get();
    }
    auto logo_width = gr_get_width(logo);
    auto logo_height = gr_get_height(logo);
    auto centered_x = ScreenWidth() / 2 - logo_width / 2;
    DrawSurface(logo, 0, 0, logo_width, logo_height, centered_x, y);
    y += logo_height;

    // Cards span the screen minus the margin and gutter; text everywhere aligns with the text
    // inside the cards.
    const int card_x = margin_width_ + MenuHorizontalMargin();
    const int card_width = std::max(0, ScreenWidth() - 2 * card_x);
    const int content_x = card_x + MenuItemPadding();
    const int x = content_x;

    if (!title_lines_.empty()) {
      SetColor(UIElement::INFO);
      y += DrawInfoTextLinesTight(x, y, title_lines_);
      y += std::max(4, MenuItemPadding() / 2);
    }
    back_button_rect_valid_ = false;
    int header_offset = 0;
    const auto* text_headers = menu_->TextHeaders();
    if (text_headers != nullptr && !text_headers->empty()) {
      const int menu_char_width = MenuCharWidth();
      const int text_inset = std::max(6, menu_char_width + menu_char_width / 2);
      const int side_margin = std::max(MenuItemPadding(), text_inset);
      const int button_left = card_x;
      const int button_right = card_x + card_width;
      int header_text_x = content_x;

      GRSurface* back_icon = nullptr;
      GRSurface* back_icon_sel = nullptr;
      if (theme_ == Theme::LIGHT && back_icon_light_) {
        back_icon = back_icon_light_.get();
        back_icon_sel = back_icon_sel_light_ ? back_icon_sel_light_.get() : back_icon;
      } else {
        back_icon = back_icon_.get();
        back_icon_sel = back_icon_sel_.get();
      }
      const int icon_w = gr_get_width(back_icon);
      const int icon_h = gr_get_height(back_icon);
      int icon_bg = std::max(icon_w, icon_h);
      const int header_char_width = header_char_width_ > 0 ? header_char_width_ : menu_char_width;
      const int header_char_height =
          header_char_height_ > 0 ? header_char_height_ : menu_char_height_;
      const int min_gap = std::max(MenuItemPadding(), header_char_width / 2);
      const int min_icon_bg = std::max(icon_w, icon_h);
      if (side_margin > min_gap + min_icon_bg) {
        icon_bg = std::min(icon_bg, side_margin - min_gap);
      }
      header_text_x = std::max(header_text_x, button_left + icon_bg + min_gap);

      const int header_max_width = std::max(0, button_right - header_text_x);
      const size_t header_cols =
          header_char_width > 0 ? static_cast<size_t>(std::max(1, header_max_width / header_char_width))
                                : 1;
      const auto header_lines = WrapTextLines(*text_headers, header_cols);
      const int line_spacing = std::max(2, header_char_height / 3);
      const int line_height = header_char_height + line_spacing;
      const int text_height = header_lines.empty()
                                  ? 0
                                  : line_height * static_cast<int>(header_lines.size()) -
                                        line_spacing;

      int header_height = text_height;
      if (!menu_->IsMain()) {
        header_height = std::max(header_height, icon_bg);
      }

      const int header_top = y;
      const int text_y = header_top + (header_height - text_height) / 2;
      const int icon_bg_y = header_top + (header_height - icon_bg) / 2;

      if (!menu_->IsMain()) {
        const int icon_x = button_left + (icon_bg - icon_w) / 2;
        const int icon_y = icon_bg_y + (icon_bg - icon_h) / 2;
        gr_blit(back_icon_sel && menu_->selection() == -1 ? back_icon_sel : back_icon, 0, 0, icon_w,
                icon_h, icon_x, icon_y);
        back_button_rect_ = { button_left, icon_bg_y, button_left + icon_bg,
                              icon_bg_y + icon_bg };
        back_button_rect_valid_ = true;
      }

      SetColor(UIElement::HEADER);
      for (size_t i = 0; i < header_lines.size(); ++i) {
        gr_text(gr_header_font(), header_text_x, text_y + static_cast<int>(i) * line_height,
                header_lines[i].c_str(), false);
      }

      const int header_bottom_spacing = std::max(MenuItemPadding(), (MenuItemPadding() * 3) / 4);
      header_offset = header_height + header_bottom_spacing;
    } else {
      if (!menu_->IsMain()) {
        GRSurface* back_icon = nullptr;
        GRSurface* back_icon_sel = nullptr;
        if (theme_ == Theme::LIGHT && back_icon_light_) {
          back_icon = back_icon_light_.get();
          back_icon_sel = back_icon_sel_light_ ? back_icon_sel_light_.get() : back_icon;
        } else {
          back_icon = back_icon_.get();
          back_icon_sel = back_icon_sel_.get();
        }
        const int icon_w = gr_get_width(back_icon);
        const int icon_h = gr_get_height(back_icon);
        const int icon_x = (centered_x - margin_width_) / 2 - icon_w / 2 + margin_width_;
        const int icon_y = y - logo_height / 2 - icon_h / 2;
        gr_blit(back_icon_sel && menu_->selection() == -1 ? back_icon_sel : back_icon, 0, 0, icon_w,
                icon_h, icon_x, icon_y);
        back_button_rect_ = { icon_x, icon_y, icon_x + icon_w, icon_y + icon_h };
        back_button_rect_valid_ = true;
      }
      header_offset = menu_->DrawHeader(x, y);
      if (header_offset > 0) {
        header_offset += std::max(MenuItemPadding(), (MenuItemPadding() * 3) / 4);
      }
    }
    y += header_offset;
    menu_start_y_ = y + MenuItemsTopGap();
    int reserved = 0;
    if (!help_message.empty()) {
      reserved = MenuItemsTopGap() + static_cast<int>(help_message.size()) * (char_height_ + 4);
    }
    menu_->SetMenuHeight(std::max(0, ScreenHeight() - margin_height_ - menu_start_y_ - reserved));
    y += menu_->DrawItems(card_x, y, card_width, IsLongPress());
    if (!help_message.empty()) {
      y += MenuItemsTopGap();
      SetColor(UIElement::INFO);
      y += DrawTextLines(content_x, y, help_message);
    }
  }

  // Display from the bottom up, until we hit the top of the screen, the bottom of the menu, or
  // we've displayed the entire text buffer.
  SetColor(UIElement::LOG);
  int row = text_row_;
  size_t count = 0;
  for (int ty = ScreenHeight() - margin_height_ - char_height_; ty >= y && count < text_rows_;
       ty -= char_height_, ++count) {
    DrawTextLine(margin_width_, ty, text_[row], false);
    --row;
    if (row < 0) row = text_rows_ - 1;
  }
}

// Draws the battery capacity on the screen. Should only be called with updateMutex locked.
void ScreenRecoveryUI::draw_battery_capacity_locked() {
  int x;
  int y = margin_height_ + gr_get_height(chara_logo_.get());
  int icon_x, icon_y, icon_h, icon_w;
  int info_char_width = info_char_width_ > 0 ? info_char_width_ : char_width_;
  int info_char_height = info_char_height_ > 0 ? info_char_height_ : char_height_;
  int info_advance = info_char_width;
  int info_tighten = std::max(1, info_char_width / 8);
  if (info_advance > info_tighten) {
    info_advance -= info_tighten;
  }

  if (is_battery_less) return;

  // Battery status
  std::string batt_capacity = std::to_string(batt_capacity_) + '%';

  if (charging_)
    batt_capacity.push_back('+');
  else if (batt_capacity.back() == '+')
    batt_capacity.pop_back();

  if (menu_) {
    // Battery icon
    x = (ScreenWidth() - margin_width_ * 2 - kMenuIndent) - info_char_width;

    SetColor(UIElement::INFO);

    // Top
    icon_x = x + info_char_width / 3;
    icon_y = y;
    icon_w = info_char_width / 3;
    icon_h = info_char_height / 12;
    gr_fill(icon_x, icon_y, icon_x + icon_w, icon_y + icon_h);

    // Main rect
    icon_x = x;
    icon_y = y + icon_h;
    icon_w = info_char_width;
    icon_h = info_char_height - (info_char_height / 12);
    gr_fill(icon_x, icon_y, icon_x + icon_w, icon_y + icon_h);

    // Capacity
    if (batt_capacity_ <= 15) SetColor(UIElement::BATTERY_LOW);
    icon_x = x + info_char_width / 6;
    icon_y = y + info_char_height / 12;
    icon_w = info_char_width - (2 * info_char_width / 6);
    icon_h = info_char_height - (3 * info_char_height / 12);
    int cap_h = icon_h * batt_capacity_ / 100;
    gr_fill(icon_x, icon_y + icon_h - cap_h, icon_x + icon_w, icon_y + icon_h);
    if (theme_ == Theme::LIGHT) {
      gr_color(kLightBgR, kLightBgG, kLightBgB, 255);
    } else {
      gr_color(0, 0, 0, 255);
    }
    gr_fill(icon_x, icon_y, icon_x + icon_w, icon_y + icon_h - cap_h);

    x -= info_char_width;  // Separator

    // Battery text
    SetColor(UIElement::INFO);
    x -= batt_capacity.size() * info_advance;
    DrawInfoTextLineTight(x, icon_y, batt_capacity.c_str(), false);
  }
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with updateMutex locked.
void ScreenRecoveryUI::update_screen_locked() {
  screen_dirty_ = false;
  draw_screen_locked();
  gr_flip();
}

// Updates only the progress bar, if possible, otherwise redraws the screen.
// Should only be called with updateMutex locked.
void ScreenRecoveryUI::update_progress_locked() {
  screen_dirty_ = false;
  if (show_text || !pagesIdentical) {
    draw_screen_locked();  // Must redraw the whole screen
    pagesIdentical = true;
  } else {
    draw_foreground_locked();  // Draw only the progress bar and overlays
  }
  gr_flip();
}

void ScreenRecoveryUI::BattMonitorThreadLoop() {
  using aidl::android::hardware::health::BatteryStatus;
  using android::hardware::health::InitHealthdConfig;

  auto config = std::make_unique<healthd_config>();
  InitHealthdConfig(config.get());

  auto batt_monitor = std::make_unique<android::BatteryMonitor>();
  batt_monitor->init(config.get());

  bool is_first_call = true;

  while (!batt_monitor_thread_stopped_) {
    bool redraw = false;
    {
      std::lock_guard<std::mutex> lg(updateMutex);

      auto charge_status = static_cast<BatteryStatus>(batt_monitor->getChargeStatus());
      // Treat unknown status as on charger.
      bool charging = (charge_status != BatteryStatus::DISCHARGING &&
                       charge_status != BatteryStatus::NOT_CHARGING &&
                       charge_status != BatteryStatus::FULL);
      if (charging_ != charging) {
        charging_ = charging;
        redraw = true;
      }

      android::BatteryProperty prop;
      android::base::Timer t;
      android::status_t status;
      while (t.duration() < 5s) {
        status = batt_monitor->getProperty(android::BATTERY_PROP_CAPACITY, &prop);
        if (status == android::OK || !is_first_call) {
          break;
        }

        LOG(WARNING) << "Trying again for reinitializing battery info";
        if (redraw) update_screen_locked();
        batt_monitor->init(config.get());
        std::this_thread::sleep_for(100ms);
      }
      is_first_call = false;

      // If we can't read battery percentage, it may be a device without battery. In this
      // situation, use 100 as a fake battery percentage.
      if (status != android::OK) {
        prop.valueInt64 = 100;
      }

      int32_t batt_capacity = static_cast<int32_t>(prop.valueInt64);
      if (batt_capacity_ != batt_capacity) {
        batt_capacity_ = batt_capacity;
        redraw = true;
      }

      if (redraw) update_screen_locked();
    }
    std::this_thread::sleep_for(5s);
  }
}

void ScreenRecoveryUI::ProgressThreadLoop() {
  double interval = 1.0 / animation_fps_;
  while (!progress_thread_stopped_) {
    double start = now();
    bool redraw = false;
    {
      std::lock_guard<std::mutex> lg(updateMutex);

      // update the installation animation, if active
      // skip this if we have a text overlay (too expensive to update)
      if ((current_icon_ == INSTALLING_UPDATE || current_icon_ == ERASING) && !show_text) {
        if (!intro_done_) {
          if (current_frame_ == intro_frames_.size() - 1) {
            intro_done_ = true;
            current_frame_ = 0;
          } else {
            ++current_frame_;
          }
        } else {
          current_frame_ = (current_frame_ + 1) % loop_frames_.size();
        }

        redraw = true;
      }

      // move the progress bar forward on timed intervals, if configured
      int duration = progressScopeDuration;
      if (progressBarType == DETERMINATE && duration > 0) {
        double elapsed = now() - progressScopeTime;
        float p = 1.0 * elapsed / duration;
        if (p > 1.0) p = 1.0;
        if (p > progress) {
          progress = p;
          redraw = true;
        }
      }

      if (screen_dirty_) redraw = true;

      if (redraw) update_progress_locked();
    }

    double end = now();
    // minimum of 20ms delay between frames
    double delay = interval - (end - start);
    if (delay < 0.02) delay = 0.02;
    usleep(static_cast<useconds_t>(delay * 1000000));
  }
}

std::unique_ptr<GRSurface> ScreenRecoveryUI::LoadBitmap(const std::string& filename) {
  GRSurface* surface;
  if (auto result = res_create_display_surface(filename.c_str(), &surface); result < 0) {
    LOG(ERROR) << "Failed to load bitmap " << filename << " (error " << result << ")";
    return nullptr;
  }
  return std::unique_ptr<GRSurface>(surface);
}

std::unique_ptr<GRSurface> ScreenRecoveryUI::LoadLocalizedBitmap(const std::string& filename) {
  GRSurface* surface;
  auto result = res_create_localized_alpha_surface(filename.c_str(), locale_.c_str(), &surface);
  if (result == 0) {
    return std::unique_ptr<GRSurface>(surface);
  }

  result = res_create_localized_alpha_surface(filename.c_str(), DEFAULT_LOCALE, &surface);
  if (result == 0) {
    return std::unique_ptr<GRSurface>(surface);
  }

  return nullptr;
}

static char** Alloc2d(size_t rows, size_t cols) {
  char** result = new char*[rows];
  for (size_t i = 0; i < rows; ++i) {
    result[i] = new char[cols];
    memset(result[i], 0, cols);
  }
  return result;
}

// Choose the right background string to display during update.
void ScreenRecoveryUI::SetSystemUpdateText(bool security_update) {
  if (security_update) {
    installing_text_ = LoadLocalizedBitmap("installing_security_text");
  } else {
    installing_text_ = LoadLocalizedBitmap("installing_text");
  }
  Redraw();
}

bool ScreenRecoveryUI::InitTextParams() {
  // gr_init() would return successfully on font initialization failure.
  if (gr_sys_font() == nullptr) {
    return false;
  }
  gr_font_size(gr_sys_font(), &char_width_, &char_height_);
  gr_font_size(gr_menu_font(), &menu_char_width_, &menu_char_height_);
  if (gr_header_font() == nullptr ||
      gr_font_size(gr_header_font(), &header_char_width_, &header_char_height_) != 0) {
    header_char_width_ = menu_char_width_;
    header_char_height_ = menu_char_height_;
  }
  info_char_width_ = char_width_;
  info_char_height_ = char_height_;
  info_font_surface_.reset();
  const bool prefer_large_info_font = (char_width_ >= 18 || char_height_ >= 32);
  const char* preferred = prefer_large_info_font ? "font_info_18" : "font_info_12";
  const char* fallback = prefer_large_info_font ? "font_info_12" : "font_info_18";
  GRSurface* info_surface = nullptr;
  if (res_create_alpha_surface(preferred, &info_surface) != 0) {
    if (res_create_alpha_surface(fallback, &info_surface) != 0) {
      info_surface = nullptr;
    }
  }
  if (info_surface) {
    info_font_surface_.reset(info_surface);
    info_char_width_ = info_surface->width / 96;
    info_char_height_ = info_surface->height / 2;
  }
  text_rows_ = (ScreenHeight() - margin_height_ * 2) / char_height_;
  text_cols_ = (ScreenWidth() - margin_width_ * 2) / char_width_;
  return true;
}

bool ScreenRecoveryUI::LoadWipeDataMenuText() {
  // Ignores the errors since the member variables will stay as nullptr.
  cancel_wipe_data_text_ = LoadLocalizedBitmap("cancel_wipe_data_text");
  factory_data_reset_text_ = LoadLocalizedBitmap("factory_data_reset_text");
  try_again_text_ = LoadLocalizedBitmap("try_again_text");
  wipe_data_confirmation_text_ = LoadLocalizedBitmap("wipe_data_confirmation_text");
  wipe_data_menu_header_text_ = LoadLocalizedBitmap("wipe_data_menu_header_text");
  return true;
}

static bool InitGraphics() {
  // Default timeout is 5 seconds, same as init wait for file
  // 10ms increments
  const unsigned timeout =
      android::base::GetIntProperty("ro.recovery.ui.graphics_timeout_ms", 5000) / 10;
  for (auto retry = timeout; retry > 0; --retry) {
    if (gr_init() == 0) {
      if (retry < timeout) {
        // Log message like init wait for file completion log for consistency.
        LOG(WARNING) << "wait for 'graphics' took " << ((timeout - retry) * 10) << "ms";
      }
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  // Log message like init wait for file timeout log for consistency.
  LOG(ERROR) << "timeout wait for 'graphics' took " << (timeout * 10) << "ms";
  return false;
}

bool ScreenRecoveryUI::Init(const std::string& locale) {
  RecoveryUI::Init(locale);

  if (!InitGraphics()) {
    return false;
  }
  is_graphics_available = true;

  if (!InitTextParams()) {
    return false;
  }
  menu_draw_funcs_ = std::make_unique<MenuDrawFunctions>(*this);

  if (blank_unblank_on_init_) {
    gr_fb_blank(true);
    gr_fb_blank(false);
  }

  // Are we portrait or landscape?
  layout_ = (gr_fb_width() > gr_fb_height()) ? LANDSCAPE : PORTRAIT;
  // Are we the large variant of our base layout?
  if (gr_fb_height() > PixelsFromDp(800)) ++layout_;

  text_ = Alloc2d(text_rows_, text_cols_ + 1);
  file_viewer_text_ = Alloc2d(text_rows_, text_cols_ + 1);

  text_col_ = text_row_ = 0;

  // Set up the locale info.
  SetLocale(locale);

  error_icon_ = LoadBitmap("icon_error");

  progress_bar_empty_ = LoadBitmap("progress_empty");
  progress_bar_fill_ = LoadBitmap("progress_fill");
  stage_marker_empty_ = LoadBitmap("stage_empty");
  stage_marker_fill_ = LoadBitmap("stage_fill");

  erasing_text_ = LoadLocalizedBitmap("erasing_text");
  no_command_text_ = LoadLocalizedBitmap("no_command_text");
  error_text_ = LoadLocalizedBitmap("error_text");

  back_icon_ = LoadBitmap("ic_back");
  back_icon_sel_ = LoadBitmap("ic_back_sel");
  if (android::base::GetBoolProperty("ro.boot.dynamic_partitions", false) ||
      android::base::GetBoolProperty("ro.fastbootd.available", true)) {
    chara_logo_ = LoadBitmap("logo_image_switch");
    fastbootd_logo_ = LoadBitmap("fastbootd");
  } else {
    chara_logo_ = LoadBitmap("logo_image");
  }
  const char* chara_logo_light_name =
      (android::base::GetBoolProperty("ro.boot.dynamic_partitions", false) ||
       android::base::GetBoolProperty("ro.fastbootd.available", true))
          ? "logo_image_switch_light"
          : "logo_image_light";
  chara_logo_light_ = LoadBitmap(chara_logo_light_name);
  if (!chara_logo_light_) {
    chara_logo_light_ = MakeLightSurface(chara_logo_);
  }
  fastbootd_logo_light_ = LoadBitmap("fastbootd_light");
  if (!fastbootd_logo_light_) {
    fastbootd_logo_light_ = MakeLightSurface(fastbootd_logo_);
  }
  back_icon_light_ = LoadBitmap("ic_back_light");
  back_icon_sel_light_ = LoadBitmap("ic_back_sel_light");
  if (!back_icon_light_) {
    back_icon_light_ = MakeTintedSurface(back_icon_, kLightBgR, kLightBgG, kLightBgB, 0x00, 0x00,
                                         0x00);
  }
  if (!back_icon_sel_light_) {
    back_icon_sel_light_ = MakeTintedSurface(back_icon_sel_, kLightHighlightR, kLightHighlightG,
                                             kLightHighlightB, 0x00, 0x00, 0x00);
  }

  // Background text for "installing_update" could be "installing update" or
  // "installing security update". It will be set after Init() according to the commands in BCB.
  installing_text_.reset();

  LoadWipeDataMenuText();

  LoadAnimation();

  is_battery_less = android::base::GetBoolProperty("ro.recovery.batteryless", false);
  if (!is_battery_less)
    // Keep the battery capacity updated.
    batt_monitor_thread_ = std::thread(&ScreenRecoveryUI::BattMonitorThreadLoop, this);

  // Keep the progress bar updated, even when the process is otherwise busy.
  progress_thread_ = std::thread(&ScreenRecoveryUI::ProgressThreadLoop, this);

  // set the callback for hall sensor event
  (void)ev_sync_sw_state([this](auto&& a, auto&& b) { return this->SetSwCallback(a, b);});

  return true;
}

std::string ScreenRecoveryUI::GetLocale() const {
  return locale_;
}

void ScreenRecoveryUI::LoadAnimation() {
  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(Paths::Get().resource_dir().c_str()),
                                                closedir);
  dirent* de;
  std::vector<std::string> intro_frame_names;
  std::vector<std::string> loop_frame_names;

  while ((de = readdir(dir.get())) != nullptr) {
    int value, num_chars;
    if (sscanf(de->d_name, "intro%d%n.png", &value, &num_chars) == 1) {
      intro_frame_names.emplace_back(de->d_name, num_chars);
    } else if (sscanf(de->d_name, "loop%d%n.png", &value, &num_chars) == 1) {
      loop_frame_names.emplace_back(de->d_name, num_chars);
    }
  }

  size_t intro_frames = intro_frame_names.size();
  size_t loop_frames = loop_frame_names.size();

  // It's okay to not have an intro.
  if (intro_frames == 0) intro_done_ = true;
  // But you must have an animation.
  if (loop_frames == 0) abort();

  std::sort(intro_frame_names.begin(), intro_frame_names.end());
  std::sort(loop_frame_names.begin(), loop_frame_names.end());

  intro_frames_.clear();
  intro_frames_.reserve(intro_frames);
  for (const auto& frame_name : intro_frame_names) {
    intro_frames_.emplace_back(LoadBitmap(frame_name));
  }

  loop_frames_.clear();
  loop_frames_.reserve(loop_frames);
  for (const auto& frame_name : loop_frame_names) {
    loop_frames_.emplace_back(LoadBitmap(frame_name));
  }
}

void ScreenRecoveryUI::SetBackground(Icon icon) {
  std::lock_guard<std::mutex> lg(updateMutex);

  current_icon_ = icon;
  update_screen_locked();
}

void ScreenRecoveryUI::SetTheme(Theme theme) {
  std::lock_guard<std::mutex> lg(updateMutex);
  theme_ = theme;
  update_screen_locked();
}

void ScreenRecoveryUI::SetProgressType(ProgressType type) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (progressBarType != type) {
    progressBarType = type;
  }
  progressScopeStart = 0;
  progressScopeSize = 0;
  progress = 0;
  update_progress_locked();
}

void ScreenRecoveryUI::ShowProgress(float portion, float seconds) {
  std::lock_guard<std::mutex> lg(updateMutex);
  progressBarType = DETERMINATE;
  progressScopeStart += progressScopeSize;
  progressScopeSize = portion;
  progressScopeTime = now();
  progressScopeDuration = seconds;
  progress = 0;
  update_progress_locked();
}

void ScreenRecoveryUI::SetProgress(float fraction) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;
  if (progressBarType == DETERMINATE && fraction > progress) {
    // Skip updates that aren't visibly different.
    int width = gr_get_width(progress_bar_empty_.get());
    float scale = width * progressScopeSize;
    if ((int)(progress * scale) != (int)(fraction * scale)) {
      progress = fraction;
      screen_dirty_ = true;
    }
  }
}

void ScreenRecoveryUI::SetStage(int current, int max) {
  std::lock_guard<std::mutex> lg(updateMutex);
  stage = current;
  max_stage = max;
}

void ScreenRecoveryUI::PrintV(const char* fmt, bool copy_to_stdout, va_list ap) {
  std::string str;
  android::base::StringAppendV(&str, fmt, ap);

  if (copy_to_stdout) {
    fputs(str.c_str(), stdout);
  }

  std::lock_guard<std::mutex> lg(updateMutex);
  if (text_rows_ > 0 && text_cols_ > 0) {
    for (const char* ptr = str.c_str(); *ptr != '\0'; ++ptr) {
      if (*ptr == '\n' || text_col_ >= text_cols_) {
        text_[text_row_][text_col_] = '\0';
        text_col_ = 0;
        text_row_ = (text_row_ + 1) % text_rows_;
      }
      if (*ptr != '\n') text_[text_row_][text_col_++] = *ptr;
    }
    text_[text_row_][text_col_] = '\0';
    update_screen_locked();
  }
}

void ScreenRecoveryUI::Print(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  PrintV(fmt, true, ap);
  va_end(ap);
}

void ScreenRecoveryUI::PrintOnScreenOnly(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  PrintV(fmt, false, ap);
  va_end(ap);
}

void ScreenRecoveryUI::PutChar(char ch) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (ch != '\n') text_[text_row_][text_col_++] = ch;
  if (ch == '\n' || text_col_ >= text_cols_) {
    text_col_ = 0;
    ++text_row_;
  }
}

void ScreenRecoveryUI::ClearText() {
  std::lock_guard<std::mutex> lg(updateMutex);
  text_col_ = 0;
  text_row_ = 0;
  for (size_t i = 0; i < text_rows_; ++i) {
    memset(text_[i], 0, text_cols_ + 1);
  }
}

void ScreenRecoveryUI::ShowFile(FILE* fp) {
  std::vector<off_t> offsets;
  offsets.push_back(ftello(fp));
  ClearText();

  struct stat sb;
  fstat(fileno(fp), &sb);

  bool show_prompt = false;
  while (true) {
    if (show_prompt) {
      PrintOnScreenOnly("--(%d%% of %d bytes)--",
                        static_cast<int>(100 * (double(ftello(fp)) / double(sb.st_size))),
                        static_cast<int>(sb.st_size));
      Redraw();
      while (show_prompt) {
        show_prompt = false;
        InputEvent evt = WaitInputEvent();
        if (evt.type() == EventType::EXTRA) {
          if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) {
            return;
          }
        }
        if (evt.type() != EventType::KEY) {
          show_prompt = true;
          continue;
        }
        if (evt.key() == KEY_ENTER || evt.key() == KEY_BACKSPACE ||
            evt.key() == KEY_BACK || evt.key() == KEY_HOMEPAGE ||
            evt.key() == KEY_ESC || evt.key() == KEY_LEFTMETA || evt.key() == KEY_RIGHTMETA) {
          return;
        } else if (evt.key() == KEY_POWER) {
          off_t page_bytes = static_cast<off_t>(text_rows_ * text_cols_);
          off_t start = 0;
          if (sb.st_size > page_bytes) {
            start = sb.st_size - page_bytes;
          }
          off_t prev = 0;
          if (start > page_bytes) {
            prev = start - page_bytes;
          }
          fseek(fp, start, SEEK_SET);
          offsets.clear();
          offsets.push_back(prev);
          if (start != prev) {
            offsets.push_back(start);
          }
        } else if (evt.key() == KEY_UP || evt.key() == KEY_VOLUMEUP || evt.key() == KEY_SCROLLUP ||
                   evt.key() == KEY_PAGEUP) {
          if (offsets.size() <= 1) {
            show_prompt = true;
          } else {
            offsets.pop_back();
            fseek(fp, offsets.back(), SEEK_SET);
          }
        } else {
          if (feof(fp)) {
            return;
          }
          offsets.push_back(ftello(fp));
        }
      }
      ClearText();
    }

    int ch = getc(fp);
    if (ch == EOF) {
      while (text_row_ < text_rows_ - 1) PutChar('\n');
      show_prompt = true;
    } else {
      PutChar(ch);
      if (text_col_ == 0 && text_row_ >= text_rows_ - 1) {
        show_prompt = true;
      }
    }
  }
}

void ScreenRecoveryUI::ShowFile(const std::string& filename) {
  std::unique_ptr<FILE, decltype(&fclose)> fp(fopen(filename.c_str(), "re"), fclose);
  if (!fp) {
    Print("  Unable to open %s: %s\n", filename.c_str(), strerror(errno));
    return;
  }

  char** old_text = text_;
  size_t old_text_col = text_col_;
  size_t old_text_row = text_row_;

  // Swap in the alternate screen and clear it.
  text_ = file_viewer_text_;
  ClearText();

  ShowFile(fp.get());

  text_ = old_text;
  text_col_ = old_text_col;
  text_row_ = old_text_row;
}

std::unique_ptr<Menu> ScreenRecoveryUI::CreateMenu(
    const GRSurface* graphic_header, const std::vector<const GRSurface*>& graphic_items,
    const std::vector<std::string>& text_headers, const std::vector<std::string>& text_items,
    size_t initial_selection) const {
  // horizontal unusable area: margin width + card gutter + text inset, on both sides
  const int content_width =
      ScreenWidth() - 2 * (margin_width_ + MenuHorizontalMargin() + MenuItemPadding());
  size_t max_width = static_cast<size_t>(std::max(0, content_width));
  // vertical unusable area: margin height + title lines + helper message + high light bar.
  // It is safe to reserve more space.
  size_t max_height = ScreenHeight() - margin_height_ - char_height_ * (title_lines_.size() + 3);
  if (GraphicMenu::Validate(max_width, max_height, graphic_header, graphic_items)) {
    return std::make_unique<GraphicMenu>(graphic_header, graphic_items, initial_selection, *this);
  }

  fprintf(stderr, "Failed to initialize graphic menu, falling back to use the text menu.\n");

  return CreateMenu(text_headers, text_items, initial_selection);
}

std::unique_ptr<Menu> ScreenRecoveryUI::CreateMenu(const std::vector<std::string>& text_headers,
                                                   const std::vector<std::string>& text_items,
                                                   size_t initial_selection) const {
  int menu_char_width = MenuCharWidth();
  int menu_char_height = MenuCharHeight();
  // Text lives inside the cards, so subtract the gutter and the text inset on both sides.
  int content_width =
      ScreenWidth() - 2 * (margin_width_ + MenuHorizontalMargin() + MenuItemPadding());
  int menu_cols = std::max(1, content_width / menu_char_width);
  bool wrap_selection = !HasThreeButtons() && !HasTouchScreen();
  return std::make_unique<TextMenu>(wrap_selection, menu_cols, text_headers, text_items,
                                    initial_selection, menu_char_height, *menu_draw_funcs_);
}

int ScreenRecoveryUI::SelectMenu(int sel) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (menu_) {
    int old_sel = menu_->selection();
    sel = menu_->Select(sel);

    if (sel != old_sel) {
      update_screen_locked();
    }
  }
  return sel;
}

int ScreenRecoveryUI::SelectMenu(const Point& p) {
  Point point;

  const auto scaleX = static_cast<double>(p.x()) / ScreenWidth();
  const auto scaleY = static_cast<double>(p.y()) / ScreenHeight();

  // Correct position for touch rotation
  switch (gr_touch_rotation()) {
    case GRRotation::NONE:
      point.x(ScreenWidth() * scaleX);
      point.y(ScreenHeight() * scaleY);
      break;
    case GRRotation::RIGHT:
      point.x(ScreenWidth() * scaleY);
      point.y(ScreenHeight() - (ScreenHeight() * scaleX));
      break;
    case GRRotation::DOWN:
      point.x(ScreenWidth() - (ScreenWidth() * scaleX));
      point.y(ScreenHeight() - (ScreenHeight() * scaleY));
      break;
    case GRRotation::LEFT:
      point.x(ScreenWidth() - (ScreenWidth() * scaleY));
      point.y(ScreenHeight() * scaleX);
      break;
  }

  // Correct position for overscan
  point.x(point.x() - gr_overscan_offset_x());
  point.y(point.y() - gr_overscan_offset_y());

  int new_sel = Device::kNoAction;
  std::lock_guard<std::mutex> lg(updateMutex);
  if (menu_) {
    if (!menu_->IsMain()) {
      if (back_button_rect_valid_ && point.x() >= back_button_rect_.left &&
          point.x() <= back_button_rect_.right && point.y() >= back_button_rect_.top &&
          point.y() <= back_button_rect_.bottom) {
        return Device::kGoBack;
      }
    }

    if (point.y() >= menu_start_y_ &&
        point.y() < menu_start_y_ + menu_->ItemsCount() * (MenuItemHeight() + MenuItemSpacing())) {
      int old_sel = menu_->selection();
      int relative_sel = (point.y() - menu_start_y_) / (MenuItemHeight() + MenuItemSpacing());
      new_sel = menu_->SelectVisible(relative_sel);
      if (new_sel != -1 && new_sel != old_sel) {
        update_screen_locked();
      }
    }
  }
  return new_sel;
}

int ScreenRecoveryUI::ScrollMenu(int updown) {
  std::lock_guard<std::mutex> lg(updateMutex);
  int sel = Device::kNoAction;
  if (menu_) {
    sel = menu_->Scroll(updown);
    update_screen_locked();
  }
  return sel;
}

size_t ScreenRecoveryUI::ShowMenu(std::unique_ptr<Menu>&& menu, bool menu_only,
                                  const std::function<int(int, bool)>& key_handler,
                                  bool refreshable) {
  // Throw away keys pressed previously, so user doesn't accidentally trigger menu items.
  FlushKeys();

  // If there is a key interrupt in progress, return KeyError::INTERRUPTED without starting the
  // menu.
  if (IsKeyInterrupted()) return static_cast<size_t>(KeyError::INTERRUPTED);

  CHECK(menu != nullptr);

  // Starts and displays the menu
  menu_ = std::move(menu);
  Redraw();

  int selected = menu_->selection();
  int chosen_item = -1;
  while (chosen_item < 0) {
    InputEvent evt = WaitInputEvent();
    if (evt.type() == EventType::EXTRA) {
      if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) {
        // WaitKey() was interrupted.
        return static_cast<size_t>(KeyError::INTERRUPTED);
      }
      if (evt.key() == static_cast<int>(KeyError::TIMED_OUT)) {  // WaitKey() timed out.
        if (WasTextEverVisible()) {
          continue;
        } else {
          LOG(INFO) << "Timed out waiting for key input; rebooting.";
          menu_.reset();
          Redraw();
          return static_cast<size_t>(KeyError::TIMED_OUT);
        }
      }
    }

    int action = Device::kNoAction;
    if (evt.type() == EventType::TOUCH) {
      int touch_sel = SelectMenu(evt.pos());
      if (touch_sel < 0) {
        action = touch_sel;
      } else {
        action = Device::kInvokeItem;
        selected = touch_sel;
      }
    } else {
      bool visible = IsTextVisible();
      action = key_handler(evt.key(), visible);
    }

    if (action < 0) {
      switch (action) {
        case Device::kHighlightUp:
          selected = SelectMenu(--selected);
          break;
        case Device::kHighlightDown:
          selected = SelectMenu(++selected);
          break;
        case Device::kHighlightFirst:
          selected = SelectMenu(0);
          break;
        case Device::kHighlightLast:
          selected = SelectMenu(menu_->ItemsCount() - 1);
          break;
        case Device::kScrollUp:
          selected = ScrollMenu(-1);
          break;
        case Device::kScrollDown:
          selected = ScrollMenu(1);
          break;
        case Device::kInvokeItem:
          if (selected < 0) {
            chosen_item = Device::kGoBack;
          } else {
            chosen_item = selected;
          }
          break;
        case Device::kNoAction:
          break;
        case Device::kGoBack:
          chosen_item = Device::kGoBack;
          break;
        case Device::kGoHome:
          chosen_item = Device::kGoHome;
          break;
        case Device::kDoSideload:
          chosen_item = Device::kDoSideload;
          break;
        case Device::kRefresh:
          if (refreshable) {
            chosen_item = Device::kRefresh;
          }
          break;
      }
    } else if (!menu_only) {
      chosen_item = action;
    }

    if (chosen_item == Device::kGoBack || chosen_item == Device::kGoHome ||
        chosen_item == Device::kDoSideload || chosen_item == Device::kRefresh) {
      break;
    }
  }

  menu_.reset();

  return chosen_item;
}

size_t ScreenRecoveryUI::ShowMenu(const std::vector<std::string>& headers,
                                  const std::vector<std::string>& items, size_t initial_selection,
                                  bool menu_only,
                                  const std::function<int(int, bool)>& key_handler,
                                  bool refreshable) {
  auto menu = CreateMenu(headers, items, initial_selection);
  if (menu == nullptr) {
    return initial_selection;
  }

  return ShowMenu(std::move(menu), menu_only, key_handler, refreshable);
}

size_t ScreenRecoveryUI::ShowPromptWipeDataMenu(const std::vector<std::string>& backup_headers,
                                                const std::vector<std::string>& backup_items,
                                                const std::function<int(int, bool)>& key_handler) {
  auto wipe_data_menu = CreateMenu(backup_headers, backup_items, 0);
  if (wipe_data_menu == nullptr) {
    return 0;
  }

  return ShowMenu(std::move(wipe_data_menu), true, key_handler);
}

size_t ScreenRecoveryUI::ShowPromptWipeDataConfirmationMenu(
    const std::vector<std::string>& backup_headers, const std::vector<std::string>& backup_items,
    const std::function<int(int, bool)>& key_handler) {
  auto confirmation_menu =
      CreateMenu(wipe_data_confirmation_text_.get(),
                 { cancel_wipe_data_text_.get(), factory_data_reset_text_.get() }, backup_headers,
                 backup_items, 0);
  if (confirmation_menu == nullptr) {
    return 0;
  }

  return ShowMenu(std::move(confirmation_menu), true, key_handler);
}

bool ScreenRecoveryUI::IsTextVisible() {
  std::lock_guard<std::mutex> lg(updateMutex);
  int visible = show_text;
  return visible;
}

bool ScreenRecoveryUI::WasTextEverVisible() {
  std::lock_guard<std::mutex> lg(updateMutex);
  int ever_visible = show_text_ever;
  return ever_visible;
}

void ScreenRecoveryUI::ShowText(bool visible) {
  std::lock_guard<std::mutex> lg(updateMutex);
  show_text = visible;
  if (show_text) show_text_ever = true;
  update_screen_locked();
}

void ScreenRecoveryUI::Redraw() {
  std::lock_guard<std::mutex> lg(updateMutex);
  update_screen_locked();
}

void ScreenRecoveryUI::KeyLongPress(int) {
  // Redraw so that if we're in the menu, the highlight
  // will change color to indicate a successful long press.
  Redraw();
}

void ScreenRecoveryUI::SetLocale(const std::string& new_locale) {
  locale_ = new_locale;
  rtl_locale_ = false;

  if (!new_locale.empty()) {
    size_t separator = new_locale.find('-');
    // lang has the language prefix prior to the separator, or full string if none exists.
    std::string lang = new_locale.substr(0, separator);

    // A bit cheesy: keep an explicit list of supported RTL languages.
    if (lang == "ar" ||  // Arabic
        lang == "fa" ||  // Persian (Farsi)
        lang == "he" ||  // Hebrew (new language code)
        lang == "iw" ||  // Hebrew (old language code)
        lang == "ur") {  // Urdu
      rtl_locale_ = true;
    }
  }
}

int ScreenRecoveryUI::SetSwCallback(int code, int value) {
  if (!is_graphics_available) { return -1; }
  if (code > SW_MAX) { return -1; }
  if (code != SW_LID) { return 0; }

  /* detect dual display */
  if (!gr_has_multiple_connectors()) { return -1; }

  /* turn off all screen */
  gr_fb_blank(true, DirectRenderManager::DRM_INNER);
  gr_fb_blank(true, DirectRenderManager::DRM_OUTER);
  gr_color(0, 0, 0, 255);
  gr_clear();

  /* turn on the screen */
  gr_fb_blank(false, value);
  gr_flip();

  /* set the retation */
  std::string rotation_str;
  if (value == DirectRenderManager::DRM_OUTER) {
    rotation_str =
      android::base::GetProperty("ro.minui.second_rotation", "ROTATION_NONE");
  } else {
    rotation_str =
      android::base::GetProperty("ro.minui.default_rotation", "ROTATION_NONE");
  }

  if (rotation_str == "ROTATION_RIGHT") {
    gr_rotate(GRRotation::RIGHT);
  } else if (rotation_str == "ROTATION_DOWN") {
    gr_rotate(GRRotation::DOWN);
  } else if (rotation_str == "ROTATION_LEFT") {
    gr_rotate(GRRotation::LEFT);
  } else {  // "ROTATION_NONE" or unknown string
    gr_rotate(GRRotation::NONE);
  }
  Redraw();

  return 0;
}
