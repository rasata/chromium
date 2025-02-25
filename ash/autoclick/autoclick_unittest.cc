// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/accessibility/accessibility_controller.h"
#include "ash/autoclick/autoclick_controller.h"
#include "ash/shelf/shelf.h"
#include "ash/shell.h"
#include "ash/system/accessibility/autoclick_menu_bubble_controller.h"
#include "ash/system/accessibility/autoclick_menu_view.h"
#include "ash/test/ash_test_base.h"
#include "base/run_loop.h"
#include "base/test/scoped_task_environment.h"
#include "ui/accessibility/accessibility_switches.h"
#include "ui/aura/test/test_window_delegate.h"
#include "ui/aura/window.h"
#include "ui/aura/window_event_dispatcher.h"
#include "ui/display/manager/display_manager.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/event_handler.h"
#include "ui/events/event_utils.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/test/event_generator.h"

namespace ash {

class MouseEventCapturer : public ui::EventHandler {
 public:
  MouseEventCapturer() { Reset(); }
  ~MouseEventCapturer() override = default;

  void Reset() { events_.clear(); }

  void OnMouseEvent(ui::MouseEvent* event) override {
    bool save_event = false;
    bool stop_event = false;
    // Filter out extraneous mouse events like mouse entered, exited,
    // capture changed, etc.
    ui::EventType type = event->type();
    if (type == ui::ET_MOUSE_PRESSED || type == ui::ET_MOUSE_RELEASED) {
      // Only track left and right mouse button events, ensuring that we get
      // left-click, right-click and double-click.
      if (!(event->flags() & ui::EF_LEFT_MOUSE_BUTTON) &&
          (!(event->flags() & ui::EF_RIGHT_MOUSE_BUTTON)))
        return;
      save_event = true;
      // Stop event propagation so we don't click on random stuff that
      // might break test assumptions.
      stop_event = true;
    } else if (type == ui::ET_MOUSE_DRAGGED) {
      save_event = true;
      stop_event = false;
    }
    if (save_event) {
      events_.push_back(ui::MouseEvent(event->type(), event->location(),
                                       event->root_location(),
                                       ui::EventTimeForNow(), event->flags(),
                                       event->changed_button_flags()));
    }
    if (stop_event)
      event->StopPropagation();

    // If there is a possibility that we're in an infinite loop, we should
    // exit early with a sensible error rather than letting the test time out.
    ASSERT_LT(events_.size(), 100u);
  }

  const std::vector<ui::MouseEvent>& captured_events() const { return events_; }

 private:
  std::vector<ui::MouseEvent> events_;

  DISALLOW_COPY_AND_ASSIGN(MouseEventCapturer);
};

class AutoclickTest : public AshTestBase {
 public:
  AutoclickTest() {
    DestroyScopedTaskEnvironment();
    scoped_task_environment_ =
        std::make_unique<base::test::ScopedTaskEnvironment>(
            base::test::ScopedTaskEnvironment::MainThreadType::UI_MOCK_TIME);
  }
  ~AutoclickTest() override = default;

  void SetUp() override {
    base::CommandLine::ForCurrentProcess()->AppendSwitch(
        switches::kEnableExperimentalAccessibilityAutoclick);
    AshTestBase::SetUp();
    Shell::Get()->AddPreTargetHandler(&mouse_event_capturer_);
    GetAutoclickController()->SetAutoclickDelay(base::TimeDelta());

    // Move mouse to deterministic location at the start of each test.
    GetEventGenerator()->MoveMouseTo(100, 100);

    // Make sure the display is initialized so we don't fail the test due to any
    // input events caused from creating the display.
    Shell::Get()->display_manager()->UpdateDisplays();
    base::RunLoop().RunUntilIdle();
  }

  void TearDown() override {
    Shell::Get()->RemovePreTargetHandler(&mouse_event_capturer_);
    AshTestBase::TearDown();
  }

  void MoveMouseWithFlagsTo(int x, int y, ui::EventFlags flags) {
    GetEventGenerator()->set_flags(flags);
    GetEventGenerator()->MoveMouseTo(x, y);
    GetEventGenerator()->set_flags(ui::EF_NONE);
  }

  const std::vector<ui::MouseEvent>& WaitForMouseEvents() {
    ClearMouseEvents();
    base::RunLoop().RunUntilIdle();
    return GetMouseEvents();
  }

  void FastForwardBy(int milliseconds) {
    scoped_task_environment_->FastForwardBy(
        base::TimeDelta::FromMilliseconds(milliseconds));
  }

  AutoclickController* GetAutoclickController() {
    return Shell::Get()->autoclick_controller();
  }

  // Calculates and returns full delay from the animation delay, after setting
  // that delay on the autoclick controller.
  int UpdateAnimationDelayAndGetFullDelay(float animation_delay) {
    float ratio =
        GetAutoclickController()->GetStartGestureDelayRatioForTesting();
    int full_delay = ceil(1.0 / ratio) * animation_delay;
    GetAutoclickController()->SetAutoclickDelay(
        base::TimeDelta::FromMilliseconds(full_delay));
    return full_delay;
  }

  AutoclickMenuView* GetAutoclickMenuView() {
    return GetAutoclickController()
        ->GetMenuBubbleControllerForTesting()
        ->menu_view_;
  }

  views::View* GetMenuButton(AutoclickMenuView::ButtonId view_id) {
    AutoclickMenuView* menu_view = GetAutoclickMenuView();
    if (!menu_view)
      return nullptr;
    return menu_view->GetViewByID(static_cast<int>(view_id));
  }

  void ClearMouseEvents() { mouse_event_capturer_.Reset(); }

  const std::vector<ui::MouseEvent>& GetMouseEvents() {
    return mouse_event_capturer_.captured_events();
  }

 private:
  MouseEventCapturer mouse_event_capturer_;
  std::unique_ptr<base::test::ScopedTaskEnvironment> scoped_task_environment_;

  DISALLOW_COPY_AND_ASSIGN(AutoclickTest);
};

TEST_F(AutoclickTest, ToggleEnabled) {
  std::vector<ui::MouseEvent> events;

  // We should not see any events initially.
  EXPECT_FALSE(GetAutoclickController()->IsEnabled());
  events = WaitForMouseEvents();
  EXPECT_EQ(0u, events.size());

  // Enable autoclick, and we should see a mouse pressed and
  // a mouse released event, simulating a click.
  GetAutoclickController()->SetEnabled(true);
  GetEventGenerator()->MoveMouseTo(0, 0);
  EXPECT_TRUE(GetAutoclickController()->IsEnabled());
  events = WaitForMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ(ui::ET_MOUSE_PRESSED, events[0].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[0].flags());
  EXPECT_EQ(ui::ET_MOUSE_RELEASED, events[1].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[1].flags());

  // We should not get any more clicks until we move the mouse.
  events = WaitForMouseEvents();
  EXPECT_EQ(0u, events.size());
  GetEventGenerator()->MoveMouseTo(30, 30);
  events = WaitForMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ(ui::ET_MOUSE_PRESSED, events[0].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[0].flags());
  EXPECT_EQ(ui::ET_MOUSE_RELEASED, events[1].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[1].flags());

  // Disable autoclick, and we should see the original behaviour.
  GetAutoclickController()->SetEnabled(false);
  EXPECT_FALSE(GetAutoclickController()->IsEnabled());
  events = WaitForMouseEvents();
  EXPECT_EQ(0u, events.size());
}

TEST_F(AutoclickTest, MouseMovement) {
  std::vector<ui::MouseEvent> events;
  GetAutoclickController()->SetEnabled(true);

  gfx::Point p1(0, 0);
  gfx::Point p2(20, 20);
  gfx::Point p3(40, 40);

  // Move mouse to p1.
  GetEventGenerator()->MoveMouseTo(p1);
  events = WaitForMouseEvents();
  EXPECT_EQ(2u, events.size());
  EXPECT_EQ(p1.ToString(), events[0].root_location().ToString());
  EXPECT_EQ(p1.ToString(), events[1].root_location().ToString());

  // Move mouse to multiple locations and finally arrive at p3.
  GetEventGenerator()->MoveMouseTo(p2);
  GetEventGenerator()->MoveMouseTo(p1);
  GetEventGenerator()->MoveMouseTo(p3);
  events = WaitForMouseEvents();
  EXPECT_EQ(2u, events.size());
  EXPECT_EQ(p3.ToString(), events[0].root_location().ToString());
  EXPECT_EQ(p3.ToString(), events[1].root_location().ToString());
}

TEST_F(AutoclickTest, MovementThreshold) {
  UpdateDisplay("1280x1024,800x600");
  base::RunLoop().RunUntilIdle();
  aura::Window::Windows root_windows = Shell::GetAllRootWindows();
  EXPECT_EQ(2u, root_windows.size());

  // Try at a couple different thresholds.
  for (int movement_threshold = 10; movement_threshold < 50;
       movement_threshold += 10) {
    GetAutoclickController()->SetMovementThreshold(movement_threshold);

    // Run test for the secondary display too to test fix for crbug.com/449870.
    for (auto* root_window : root_windows) {
      gfx::Point center = root_window->GetBoundsInScreen().CenterPoint();

      GetAutoclickController()->SetEnabled(true);
      GetEventGenerator()->MoveMouseTo(center);
      EXPECT_EQ(2u, WaitForMouseEvents().size());

      // Small mouse movements should not trigger an autoclick, i.e. movements
      // within the radius of the movement_threshold.
      GetEventGenerator()->MoveMouseTo(
          center + gfx::Vector2d(std::sqrt(movement_threshold) - 1,
                                 std::sqrt(movement_threshold) - 1));
      EXPECT_EQ(0u, WaitForMouseEvents().size());
      GetEventGenerator()->MoveMouseTo(
          center + gfx::Vector2d(movement_threshold - 1, 0));
      EXPECT_EQ(0u, WaitForMouseEvents().size());
      GetEventGenerator()->MoveMouseTo(
          center + gfx::Vector2d(0, -movement_threshold + 1));
      EXPECT_EQ(0u, WaitForMouseEvents().size());
      GetEventGenerator()->MoveMouseTo(center);
      EXPECT_EQ(0u, WaitForMouseEvents().size());

      // A larger mouse movement should trigger an autoclick.
      GetEventGenerator()->MoveMouseTo(
          center +
          gfx::Vector2d(movement_threshold + 1, movement_threshold + 1));
      EXPECT_EQ(2u, WaitForMouseEvents().size());
    }
  }

  // Reset to default threshold.
  GetAutoclickController()->SetMovementThreshold(20);
}

TEST_F(AutoclickTest, MovementWithinThresholdWhileTimerRunning) {
  GetAutoclickController()->SetEnabled(true);
  GetAutoclickController()->SetMovementThreshold(20);
  int animation_delay = 5;
  int full_delay = UpdateAnimationDelayAndGetFullDelay(animation_delay);

  GetEventGenerator()->MoveMouseTo(100, 100);
  FastForwardBy(animation_delay + 1);

  // Move the mouse within the threshold. It shouldn't change the eventual
  // target of the event, or cancel the click.
  GetEventGenerator()->MoveMouseTo(110, 110);

  ClearMouseEvents();
  FastForwardBy(full_delay);
  std::vector<ui::MouseEvent> events = GetMouseEvents();

  EXPECT_EQ(2u, events.size());
  EXPECT_EQ(gfx::Point(100, 100), events[0].location());
  EXPECT_EQ(ui::ET_MOUSE_PRESSED, events[0].type());
  EXPECT_EQ(ui::EF_LEFT_MOUSE_BUTTON, events[0].flags());
  EXPECT_EQ(gfx::Point(100, 100), events[1].location());
  EXPECT_EQ(ui::ET_MOUSE_RELEASED, events[1].type());
  EXPECT_EQ(ui::EF_LEFT_MOUSE_BUTTON, events[1].flags());

  // Reset delay.
  GetAutoclickController()->SetAutoclickDelay(base::TimeDelta());
}

TEST_F(AutoclickTest, SingleKeyModifier) {
  GetAutoclickController()->SetEnabled(true);
  MoveMouseWithFlagsTo(20, 20, ui::EF_SHIFT_DOWN);
  std::vector<ui::MouseEvent> events = WaitForMouseEvents();
  EXPECT_EQ(2u, events.size());
  EXPECT_EQ(ui::EF_SHIFT_DOWN, events[0].flags() & ui::EF_SHIFT_DOWN);
  EXPECT_EQ(ui::EF_SHIFT_DOWN, events[1].flags() & ui::EF_SHIFT_DOWN);
}

TEST_F(AutoclickTest, MultipleKeyModifiers) {
  GetAutoclickController()->SetEnabled(true);
  ui::EventFlags modifier_flags = static_cast<ui::EventFlags>(
      ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN | ui::EF_SHIFT_DOWN);
  MoveMouseWithFlagsTo(30, 30, modifier_flags);
  std::vector<ui::MouseEvent> events = WaitForMouseEvents();
  EXPECT_EQ(2u, events.size());
  EXPECT_EQ(modifier_flags, events[0].flags() & modifier_flags);
  EXPECT_EQ(modifier_flags, events[1].flags() & modifier_flags);
}

TEST_F(AutoclickTest, KeyModifiersReleased) {
  GetAutoclickController()->SetEnabled(true);

  ui::EventFlags modifier_flags = static_cast<ui::EventFlags>(
      ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN | ui::EF_SHIFT_DOWN);
  MoveMouseWithFlagsTo(12, 12, modifier_flags);

  // Simulate releasing key modifiers by sending key released events.
  GetEventGenerator()->ReleaseKey(
      ui::VKEY_CONTROL,
      static_cast<ui::EventFlags>(ui::EF_ALT_DOWN | ui::EF_SHIFT_DOWN));
  GetEventGenerator()->ReleaseKey(ui::VKEY_SHIFT, ui::EF_ALT_DOWN);

  std::vector<ui::MouseEvent> events;
  events = WaitForMouseEvents();
  EXPECT_EQ(2u, events.size());
  EXPECT_EQ(0, events[0].flags() & ui::EF_CONTROL_DOWN);
  EXPECT_EQ(0, events[0].flags() & ui::EF_SHIFT_DOWN);
  EXPECT_EQ(ui::EF_ALT_DOWN, events[0].flags() & ui::EF_ALT_DOWN);
}

TEST_F(AutoclickTest, UserInputCancelsAutoclick) {
  GetAutoclickController()->SetEnabled(true);
  std::vector<ui::MouseEvent> events;

  // Pressing a normal key should cancel the autoclick.
  GetEventGenerator()->MoveMouseTo(200, 200);
  GetEventGenerator()->PressKey(ui::VKEY_K, ui::EF_NONE);
  GetEventGenerator()->ReleaseKey(ui::VKEY_K, ui::EF_NONE);
  events = WaitForMouseEvents();
  EXPECT_EQ(0u, events.size());

  // Pressing a modifier key should NOT cancel the autoclick.
  GetEventGenerator()->MoveMouseTo(100, 100);
  GetEventGenerator()->PressKey(ui::VKEY_SHIFT, ui::EF_SHIFT_DOWN);
  GetEventGenerator()->ReleaseKey(ui::VKEY_SHIFT, ui::EF_NONE);
  events = WaitForMouseEvents();
  EXPECT_EQ(2u, events.size());

  // Performing a gesture should cancel the autoclick.
  GetEventGenerator()->MoveMouseTo(200, 200);
  GetEventGenerator()->GestureTapDownAndUp(gfx::Point(100, 100));
  events = WaitForMouseEvents();
  EXPECT_EQ(0u, events.size());

  // Test another gesture.
  GetEventGenerator()->MoveMouseTo(100, 100);
  GetEventGenerator()->GestureScrollSequence(
      gfx::Point(100, 100), gfx::Point(200, 200),
      base::TimeDelta::FromMilliseconds(200), 3);
  events = WaitForMouseEvents();
  EXPECT_EQ(0u, events.size());

  // Test scroll events.
  GetEventGenerator()->MoveMouseTo(200, 200);
  GetEventGenerator()->ScrollSequence(gfx::Point(100, 100),
                                      base::TimeDelta::FromMilliseconds(200), 0,
                                      100, 3, 2);
  events = WaitForMouseEvents();
  EXPECT_EQ(0u, events.size());

  // However, just starting a scroll doesn't cancel. If you tap a touchpad on
  // an Eve chromebook, for example, it can send an ET_SCROLL_FLING_CANCEL
  // event, which shouldn't actually cancel autoclick.
  GetEventGenerator()->MoveMouseTo(100, 100);
  GetEventGenerator()->ScrollSequence(gfx::Point(100, 100), base::TimeDelta(),
                                      0, 0, 0, 1);
  events = WaitForMouseEvents();
  EXPECT_EQ(2u, events.size());
}

TEST_F(AutoclickTest, SynthesizedMouseMovesIgnored) {
  GetAutoclickController()->SetEnabled(true);
  std::vector<ui::MouseEvent> events;
  GetEventGenerator()->MoveMouseTo(100, 100);
  events = WaitForMouseEvents();
  EXPECT_EQ(2u, events.size());

  // Show a window and make sure the new window is under the cursor. As a
  // result, synthesized mouse events will be dispatched to the window, but it
  // should not trigger an autoclick.
  aura::test::EventCountDelegate delegate;
  std::unique_ptr<aura::Window> window(CreateTestWindowInShellWithDelegate(
      &delegate, 123, gfx::Rect(50, 50, 100, 100)));
  window->Show();
  events = WaitForMouseEvents();
  EXPECT_EQ(0u, events.size());
  EXPECT_EQ("1 1 0", delegate.GetMouseMotionCountsAndReset());
}

TEST_F(AutoclickTest, AutoclickChangeEventTypes) {
  GetAutoclickController()->SetEnabled(true);
  GetAutoclickController()->set_revert_to_left_click(false);
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kRightClick);
  std::vector<ui::MouseEvent> events;

  GetEventGenerator()->MoveMouseTo(30, 30);
  events = WaitForMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ(ui::ET_MOUSE_PRESSED, events[0].type());
  EXPECT_TRUE(ui::EF_RIGHT_MOUSE_BUTTON & events[0].flags());
  EXPECT_EQ(ui::ET_MOUSE_RELEASED, events[1].type());
  EXPECT_TRUE(ui::EF_RIGHT_MOUSE_BUTTON & events[1].flags());

  // Changing the event type cancels the event
  GetEventGenerator()->MoveMouseTo(60, 60);
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kLeftClick);
  events = WaitForMouseEvents();
  EXPECT_EQ(0u, events.size());

  // Changing the event type to the same thing does not cancel the event.
  // kLeftClick type does not produce a double-click.
  GetEventGenerator()->MoveMouseTo(90, 90);
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kLeftClick);
  events = WaitForMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ(ui::ET_MOUSE_PRESSED, events[0].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[0].flags());
  EXPECT_FALSE(ui::EF_IS_DOUBLE_CLICK & events[0].flags());
  EXPECT_EQ(ui::ET_MOUSE_RELEASED, events[1].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[1].flags());
  EXPECT_FALSE(ui::EF_IS_DOUBLE_CLICK & events[1].flags());

  // Double-click works as expected.
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kDoubleClick);
  GetEventGenerator()->MoveMouseTo(120, 120);
  events = WaitForMouseEvents();
  ASSERT_EQ(4u, events.size());
  EXPECT_EQ(ui::ET_MOUSE_PRESSED, events[0].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[0].flags());
  EXPECT_FALSE(ui::EF_IS_DOUBLE_CLICK & events[0].flags());
  EXPECT_EQ(ui::ET_MOUSE_RELEASED, events[1].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[1].flags());
  EXPECT_FALSE(ui::EF_IS_DOUBLE_CLICK & events[1].flags());
  EXPECT_EQ(ui::ET_MOUSE_PRESSED, events[2].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[2].flags());
  EXPECT_TRUE(ui::EF_IS_DOUBLE_CLICK & events[2].flags());
  EXPECT_EQ(ui::ET_MOUSE_RELEASED, events[3].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[3].flags());
  EXPECT_TRUE(ui::EF_IS_DOUBLE_CLICK & events[3].flags());

  // Pause / no action does not cause events to be generated even when the
  // mouse moves.
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kNoAction);
  GetEventGenerator()->MoveMouseTo(120, 120);
  events = WaitForMouseEvents();
  EXPECT_EQ(0u, events.size());
}

TEST_F(AutoclickTest, AutoclickDragAndDropEvents) {
  GetAutoclickController()->SetEnabled(true);
  GetAutoclickController()->set_revert_to_left_click(false);
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kDragAndDrop);
  std::vector<ui::MouseEvent> events;

  GetEventGenerator()->MoveMouseTo(30, 30);
  events = WaitForMouseEvents();
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ(ui::ET_MOUSE_PRESSED, events[0].type());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[0].flags());

  ClearMouseEvents();
  GetEventGenerator()->MoveMouseTo(60, 60);
  events = GetMouseEvents();
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ(ui::ET_MOUSE_DRAGGED, events[0].type());

  // Another move creates a drag
  ClearMouseEvents();
  GetEventGenerator()->MoveMouseTo(90, 90);
  events = GetMouseEvents();
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ(ui::ET_MOUSE_DRAGGED, events[0].type());

  // Waiting in place creates the released event.
  events = WaitForMouseEvents();
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ(ui::ET_MOUSE_RELEASED, events[0].type());
}

TEST_F(AutoclickTest, AutoclickRevertsToLeftClick) {
  GetAutoclickController()->SetEnabled(true);
  GetAutoclickController()->set_revert_to_left_click(true);
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kRightClick);
  std::vector<ui::MouseEvent> events;

  GetEventGenerator()->MoveMouseTo(30, 30);
  events = WaitForMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_TRUE(ui::EF_RIGHT_MOUSE_BUTTON & events[0].flags());
  EXPECT_TRUE(ui::EF_RIGHT_MOUSE_BUTTON & events[1].flags());

  // Another event is now left-click; we've reverted to left click.
  GetEventGenerator()->MoveMouseTo(90, 90);
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kLeftClick);
  events = WaitForMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[0].flags());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[1].flags());

  // The next event is also a left click.
  GetEventGenerator()->MoveMouseTo(120, 120);
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kLeftClick);
  events = WaitForMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[0].flags());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[1].flags());

  // Changing revert to false doesn't change that we are on left click at
  // present.
  GetAutoclickController()->set_revert_to_left_click(false);
  GetEventGenerator()->MoveMouseTo(150, 150);
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kLeftClick);
  events = WaitForMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[0].flags());
  EXPECT_TRUE(ui::EF_LEFT_MOUSE_BUTTON & events[1].flags());

  // But we should no longer revert to left click if the type is something else.
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kRightClick);
  GetEventGenerator()->MoveMouseTo(180, 180);
  events = WaitForMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_TRUE(ui::EF_RIGHT_MOUSE_BUTTON & events[0].flags());
  EXPECT_TRUE(ui::EF_RIGHT_MOUSE_BUTTON & events[1].flags());

  // Should still be right click.
  GetEventGenerator()->MoveMouseTo(210, 210);
  events = WaitForMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_TRUE(ui::EF_RIGHT_MOUSE_BUTTON & events[0].flags());
  EXPECT_TRUE(ui::EF_RIGHT_MOUSE_BUTTON & events[1].flags());
}

TEST_F(AutoclickTest, WaitsToDrawAnimationAfterDwellBegins) {
  int animation_delay = 5;
  int full_delay = UpdateAnimationDelayAndGetFullDelay(animation_delay);
  GetAutoclickController()->SetEnabled(true);
  std::vector<ui::MouseEvent> events;

  // Start a dwell at (210, 210).
  GetEventGenerator()->MoveMouseTo(210, 210);

  // The center should change to (205, 205) if the adjustment is made before
  // the animation starts.
  FastForwardBy(animation_delay - 1);
  GetEventGenerator()->MoveMouseTo(205, 205);

  // Now wait the full delay to ensure the click has happened, then check
  // the result.
  FastForwardBy(full_delay);
  events = GetMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ(gfx::Point(205, 205), events[0].location());

  // Start another dwell at (100, 100).
  ClearMouseEvents();
  GetEventGenerator()->MoveMouseTo(100, 100);

  // Move the mouse a little to (105, 105), which should become the center.
  FastForwardBy(animation_delay - 1);
  GetEventGenerator()->MoveMouseTo(105, 105);

  // Fast forward until the animation would have started. Now moving the mouse
  // a little does not change the center point.
  FastForwardBy(animation_delay);
  GetEventGenerator()->MoveMouseTo(110, 110);

  // Now wait until the click. It should be at the center point from before
  // the animation started.
  FastForwardBy(full_delay);
  events = GetMouseEvents();
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ(gfx::Point(105, 105), events[0].location());
}

TEST_F(AutoclickTest, DoesActionOnBubbleWhenInDifferentModes) {
  AccessibilityController* accessibility_controller =
      Shell::Get()->accessibility_controller();
  // Enable autoclick from the accessibility controller so that the bubble is
  // constructed too.
  accessibility_controller->SetAutoclickEnabled(true);
  GetAutoclickController()->set_revert_to_left_click(false);
  std::vector<ui::MouseEvent> events;

  // Test at different screen sizes and densities because the fake click on
  // the button involves coordinating between dips and pixels. Try two different
  // positions to ensure offsets are calculated correctly.
  const struct {
    const std::string display_spec;
    float scale;
    mojom::AutoclickMenuPosition position;
  } kTestCases[] = {
      {"800x600", 1.0f, mojom::AutoclickMenuPosition::kBottomRight},
      {"1024x800*2.0", 2.0f, mojom::AutoclickMenuPosition::kBottomRight},
      {"800x600", 1.0f, mojom::AutoclickMenuPosition::kTopLeft},
      {"1024x800*2.0", 2.0f, mojom::AutoclickMenuPosition::kTopLeft},
  };
  for (const auto& test : kTestCases) {
    UpdateDisplay(test.display_spec);
    accessibility_controller->SetAutoclickMenuPosition(test.position);
    accessibility_controller->SetAutoclickEventType(
        mojom::AutoclickEventType::kRightClick);

    AutoclickMenuView* menu = GetAutoclickMenuView();
    ASSERT_TRUE(menu);

    // Outside of the bubble, a right-click still occurs.
    // Move to a central position which will not have any menu but will still be
    // on-screen.
    GetEventGenerator()->MoveMouseTo(200 * test.scale, 200 * test.scale);
    events = WaitForMouseEvents();
    ASSERT_EQ(2u, events.size());
    EXPECT_TRUE(ui::EF_RIGHT_MOUSE_BUTTON & events[0].flags());
    EXPECT_TRUE(ui::EF_RIGHT_MOUSE_BUTTON & events[1].flags());

    // Over the bubble, we get no real click, although the autoclick event
    // type does get changed properly over a button.
    gfx::Point button_location = gfx::ScaleToRoundedPoint(
        GetMenuButton(AutoclickMenuView::ButtonId::kDoubleClick)
            ->GetBoundsInScreen()
            .CenterPoint(),
        test.scale);
    GetEventGenerator()->MoveMouseTo(button_location);
    events = WaitForMouseEvents();
    EXPECT_EQ(0u, events.size());
    // But the event type did change with a the hover on the button.
    EXPECT_EQ(mojom::AutoclickEventType::kDoubleClick,
              accessibility_controller->GetAutoclickEventType());

    // Change to a pause action type.
    accessibility_controller->SetAutoclickEventType(
        mojom::AutoclickEventType::kNoAction);

    // Outside the bubble, no action occurs.
    GetEventGenerator()->MoveMouseTo(200 * test.scale, 200 * test.scale);
    events = WaitForMouseEvents();
    EXPECT_EQ(0u, events.size());

    // If we move over the bubble but not over any button than no real click
    // occurs.
    button_location = gfx::ScaleToRoundedPoint(
        GetAutoclickMenuView()->GetBoundsInScreen().CenterPoint(), test.scale);
    GetEventGenerator()->MoveMouseTo(button_location);
    events = WaitForMouseEvents();
    EXPECT_EQ(0u, events.size());
    // The event type did not change because we were not over any button.
    EXPECT_EQ(mojom::AutoclickEventType::kNoAction,
              accessibility_controller->GetAutoclickEventType());

    // Leaving the bubble we are still paused.
    GetEventGenerator()->MoveMouseTo(200 * test.scale, 200 * test.scale);
    events = WaitForMouseEvents();
    EXPECT_EQ(0u, events.size());

    // Moving over another button takes an action.
    button_location = gfx::ScaleToRoundedPoint(
        GetMenuButton(AutoclickMenuView::ButtonId::kLeftClick)
            ->GetBoundsInScreen()
            .CenterPoint(),
        test.scale);
    GetEventGenerator()->MoveMouseTo(button_location);
    events = WaitForMouseEvents();
    EXPECT_EQ(0u, events.size());
    EXPECT_EQ(mojom::AutoclickEventType::kLeftClick,
              accessibility_controller->GetAutoclickEventType());
  }

  // Reset state.
  accessibility_controller->SetAutoclickEnabled(false);
}

TEST_F(AutoclickTest,
       StartsGestureOnBubbleButDoesNotClickIfMouseMovedWhenPaused) {
  Shell::Get()->accessibility_controller()->SetAutoclickEnabled(true);
  GetAutoclickController()->set_revert_to_left_click(false);
  GetAutoclickController()->SetAutoclickEventType(
      mojom::AutoclickEventType::kNoAction);
  Shell::Get()->accessibility_controller()->SetAutoclickMenuPosition(
      mojom::AutoclickMenuPosition::kBottomRight);

  int animation_delay = 5;
  int full_delay = UpdateAnimationDelayAndGetFullDelay(animation_delay);

  std::vector<ui::MouseEvent> events;
  AutoclickMenuView* menu = GetAutoclickMenuView();
  ASSERT_TRUE(menu);

  // Start a dwell over the bubble.
  GetEventGenerator()->MoveMouseTo(menu->GetBoundsInScreen().origin());

  // Move back off the bubble before anything happens.
  FastForwardBy(animation_delay - 1);
  GetEventGenerator()->MoveMouseTo(30, 30);

  // Now wait the full delay to ensure pause could have happened.
  FastForwardBy(full_delay);
  events = GetMouseEvents();
  ASSERT_EQ(0u, events.size());

  // This time, dwell over the bubble long enough for the animation to begin.
  // No action should occur if we move off during the dwell.
  GetEventGenerator()->MoveMouseTo(menu->GetBoundsInScreen().origin());

  // Move back off the bubble after the animation begins, but before a click
  // would occur.
  FastForwardBy(animation_delay + 1);
  GetEventGenerator()->MoveMouseTo(30, 30);

  // Now wait the full delay to ensure pause could have happened.
  FastForwardBy(full_delay);
  events = GetMouseEvents();
  ASSERT_EQ(0u, events.size());

  // Reset state.
  Shell::Get()->accessibility_controller()->SetAutoclickEnabled(false);
}

// The autoclick tray shouldn't stop the shelf from auto-hiding.
TEST_F(AutoclickTest, ShelfAutohidesWithAutoclickBubble) {
  Shell::Get()->accessibility_controller()->SetAutoclickEnabled(false);
  Shelf* shelf = GetPrimaryShelf();

  // Create a visible window so auto-hide behavior is enforced.
  views::Widget::InitParams params(views::Widget::InitParams::TYPE_WINDOW);
  params.bounds = gfx::Rect(0, 0, 200, 200);
  params.context = CurrentContext();
  views::Widget* widget = new views::Widget;
  widget->Init(params);
  widget->Show();

  // Turn on auto-hide for the shelf.
  shelf->SetAutoHideBehavior(SHELF_AUTO_HIDE_BEHAVIOR_ALWAYS);
  EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
  EXPECT_EQ(SHELF_AUTO_HIDE_HIDDEN, shelf->GetAutoHideState());

  // Enable autoclick. The shelf should remain invisible.
  Shell::Get()->accessibility_controller()->SetAutoclickEnabled(true);
  AutoclickMenuView* menu = GetAutoclickMenuView();
  ASSERT_TRUE(menu);
  EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
  EXPECT_EQ(SHELF_AUTO_HIDE_HIDDEN, shelf->GetAutoHideState());

  // Reset state.
  Shell::Get()->accessibility_controller()->SetAutoclickEnabled(false);
}

}  // namespace ash
