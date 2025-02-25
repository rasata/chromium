// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NOTIFICATIONS_NOTIFICATION_BACKGROUND_TASK_SCHEDULER_ANDROID_H_
#define CHROME_BROWSER_NOTIFICATIONS_NOTIFICATION_BACKGROUND_TASK_SCHEDULER_ANDROID_H_

#include "base/macros.h"
#include "base/time/time.h"
#include "chrome/browser/notifications/scheduler/notification_background_task_scheduler.h"

// This class contains:
// 1. Android implementation of NotificationBackgroundTaskScheduler, which
// asks Android API to schedule background task.
// 2. JNI calls to route background task events to native.
// The life cycle of this object is owned by a keyed service in native.
class NotificationBackgroundTaskSchedulerAndroid
    : public notifications::NotificationBackgroundTaskScheduler {
 public:
  NotificationBackgroundTaskSchedulerAndroid();
  ~NotificationBackgroundTaskSchedulerAndroid() override;

 private:
  // NotificationBackgroundTaskScheduler implementation.
  void Schedule(base::TimeDelta window_start,
                base::TimeDelta window_end) override;

  DISALLOW_COPY_AND_ASSIGN(NotificationBackgroundTaskSchedulerAndroid);
};

#endif  // CHROME_BROWSER_NOTIFICATIONS_NOTIFICATION_BACKGROUND_TASK_SCHEDULER_ANDROID_H_
