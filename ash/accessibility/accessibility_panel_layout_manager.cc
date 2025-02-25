// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/accessibility/accessibility_panel_layout_manager.h"

#include "ash/root_window_controller.h"
#include "ash/shell.h"
#include "ash/wm/work_area_insets.h"
#include "base/logging.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/wm/core/window_util.h"
#include "ui/wm/public/activation_client.h"

namespace ash {

AccessibilityPanelLayoutManager::AccessibilityPanelLayoutManager() {
  display::Screen::GetScreen()->AddObserver(this);
  Shell::Get()->activation_client()->AddObserver(this);
  Shell::Get()->AddShellObserver(this);
}

AccessibilityPanelLayoutManager::~AccessibilityPanelLayoutManager() {
  Shell::Get()->RemoveShellObserver(this);
  Shell::Get()->activation_client()->RemoveObserver(this);
  display::Screen::GetScreen()->RemoveObserver(this);
}

void AccessibilityPanelLayoutManager::SetPanelBounds(
    const gfx::Rect& bounds,
    mojom::AccessibilityPanelState state) {
  if (!panel_window_)
    return;

  panel_bounds_ = bounds;
  panel_state_ = state;
  UpdateWindowBounds();
  UpdateWorkAreaForPanelHeight();
}

void AccessibilityPanelLayoutManager::OnWindowAddedToLayout(
    aura::Window* child) {
  panel_window_ = child;
  // Defer setting the window bounds until the extension is loaded and Chrome
  // shows the widget.
}

void AccessibilityPanelLayoutManager::OnWindowRemovedFromLayout(
    aura::Window* child) {
  // NOTE: In browser_tests a second ChromeVoxPanel can be created while the
  // first one is closing due to races between loading the extension and
  // closing the widget. We only track the latest panel.
  if (child == panel_window_)
    panel_window_ = nullptr;

  UpdateWorkAreaForPanelHeight();
}

void AccessibilityPanelLayoutManager::OnChildWindowVisibilityChanged(
    aura::Window* child,
    bool visible) {
  if (child == panel_window_ && visible) {
    UpdateWindowBounds();
    UpdateWorkAreaForPanelHeight();
  }
}

void AccessibilityPanelLayoutManager::SetChildBounds(
    aura::Window* child,
    const gfx::Rect& requested_bounds) {
  SetChildBoundsDirect(child, requested_bounds);
}

void AccessibilityPanelLayoutManager::OnDisplayMetricsChanged(
    const display::Display& display,
    uint32_t changed_metrics) {
  UpdateWindowBounds();
}

void AccessibilityPanelLayoutManager::OnWindowActivated(
    ActivationReason reason,
    aura::Window* gained_active,
    aura::Window* lost_active) {
  UpdateWindowBounds();
}

void AccessibilityPanelLayoutManager::OnFullscreenStateChanged(
    bool is_fullscreen,
    aura::Window* container) {
  UpdateWindowBounds();
}

void AccessibilityPanelLayoutManager::UpdateWindowBounds() {
  if (!panel_window_)
    return;

  aura::Window* root_window = panel_window_->GetRootWindow();
  RootWindowController* root_controller =
      RootWindowController::ForWindow(root_window);

  gfx::Rect bounds = panel_bounds_;

  // The panel can make itself fill the screen (including covering the shelf).
  if (panel_state_ == mojom::AccessibilityPanelState::FULLSCREEN) {
    bounds = root_window->bounds();
  } else if (panel_state_ == mojom::AccessibilityPanelState::FULL_WIDTH) {
    bounds.set_x(0);
    bounds.set_width(root_window->bounds().width());
  }

  // Make sure the accessibility panel is always below the Docked Magnifier
  // viewport so it shows up and gets magnified.
  int magnifier_height =
      root_controller->work_area_insets()->docked_magnifier_height();
  if (bounds.y() < magnifier_height)
    bounds.Offset(0, magnifier_height);
  // Make sure the accessibility panel doesn't go offscreen when the Docked
  // Magnifier is on.
  int screen_height = root_window->bounds().height();
  int available_height = screen_height - magnifier_height;
  if (bounds.height() > available_height)
    bounds.set_height(available_height);

  panel_window_->SetBounds(bounds);
}

void AccessibilityPanelLayoutManager::UpdateWorkAreaForPanelHeight() {
  bool has_height = panel_window_ && panel_window_->bounds().y() == 0 &&
                    panel_state_ == mojom::AccessibilityPanelState::FULL_WIDTH;
  Shell::GetPrimaryRootWindowController()
      ->work_area_insets()
      ->SetAccessibilityPanelHeight(
          has_height ? panel_window_->bounds().height() : 0);
}

}  // namespace ash
