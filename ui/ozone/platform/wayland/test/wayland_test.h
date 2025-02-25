// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_WAYLAND_TEST_WAYLAND_TEST_H_
#define UI_OZONE_PLATFORM_WAYLAND_TEST_WAYLAND_TEST_H_

#include <memory>

#include "base/message_loop/message_loop.h"
#include "base/test/scoped_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/buildflags.h"
#include "ui/ozone/platform/wayland/gpu/wayland_connection_proxy.h"
#include "ui/ozone/platform/wayland/host/wayland_connection.h"
#include "ui/ozone/platform/wayland/host/wayland_window.h"
#include "ui/ozone/platform/wayland/test/test_wayland_server_thread.h"
#include "ui/ozone/test/mock_platform_window_delegate.h"

#if BUILDFLAG(USE_XKBCOMMON)
#include "ui/events/ozone/layout/xkb/xkb_evdev_codes.h"
#endif

namespace wl {
class MockSurface;
}  // namespace wl

namespace ui {

const uint32_t kXdgShellV5 = 5;
const uint32_t kXdgShellV6 = 6;

// WaylandTest is a base class that sets up a display, window, and test server,
// and allows easy synchronization between them.
class WaylandTest : public ::testing::TestWithParam<uint32_t> {
 public:
  WaylandTest();
  ~WaylandTest() override;

  void SetUp() override;
  void TearDown() override;

  void Sync();

 protected:
  base::test::ScopedTaskEnvironment scoped_task_environment_;

  wl::TestWaylandServerThread server_;
  wl::MockSurface* surface_;

  MockPlatformWindowDelegate delegate_;
  std::unique_ptr<WaylandConnectionProxy> connection_proxy_;
  std::unique_ptr<WaylandConnection> connection_;
  std::unique_ptr<WaylandWindow> window_;
  gfx::AcceleratedWidget widget_ = gfx::kNullAcceleratedWidget;

 private:
  bool initialized_ = false;

#if BUILDFLAG(USE_XKBCOMMON)
  XkbEvdevCodes xkb_evdev_code_converter_;
#endif

  DISALLOW_COPY_AND_ASSIGN(WaylandTest);
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_WAYLAND_TEST_WAYLAND_TEST_H_
