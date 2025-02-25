// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file provides API functions for the file manager to act as the file
// dialog for opening and saving files.

#ifndef CHROME_BROWSER_CHROMEOS_EXTENSIONS_FILE_MANAGER_PRIVATE_API_DIALOG_H_
#define CHROME_BROWSER_CHROMEOS_EXTENSIONS_FILE_MANAGER_PRIVATE_API_DIALOG_H_

#include <vector>

#include "chrome/browser/chromeos/extensions/file_manager/private_api_base.h"

namespace ui {
struct SelectedFileInfo;
}

namespace extensions {

// Cancel file selection Dialog.  Closes the dialog window.
class FileManagerPrivateCancelDialogFunction
    : public LoggedUIThreadExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("fileManagerPrivate.cancelDialog",
                             FILEMANAGERPRIVATE_CANCELDIALOG)

 protected:
  ~FileManagerPrivateCancelDialogFunction() override = default;

  // ExtensionFunction overrides.
  ResponseAction Run() override;
};

class FileManagerPrivateSelectFileFunction
    : public LoggedUIThreadExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("fileManagerPrivate.selectFile",
                             FILEMANAGERPRIVATE_SELECTFILE)

 protected:
  ~FileManagerPrivateSelectFileFunction() override = default;

  // ExtensionFunction overrides.
  ResponseAction Run() override;

 private:
  // A callback method to handle the result of GetSelectedFileInfo.
  void GetSelectedFileInfoResponse(
      bool for_open,
      int index,
      const std::vector<ui::SelectedFileInfo>& files);
};

// Select multiple files.  Closes the dialog window.
class FileManagerPrivateSelectFilesFunction
    : public LoggedUIThreadExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("fileManagerPrivate.selectFiles",
                             FILEMANAGERPRIVATE_SELECTFILES)

 protected:
  ~FileManagerPrivateSelectFilesFunction() override = default;

  // ExtensionFunction overrides.
  ResponseAction Run() override;

 private:
  // A callback method to handle the result of GetSelectedFileInfo.
  void GetSelectedFileInfoResponse(
      bool for_open,
      const std::vector<ui::SelectedFileInfo>& files);
};

}  // namespace extensions

#endif  // CHROME_BROWSER_CHROMEOS_EXTENSIONS_FILE_MANAGER_PRIVATE_API_DIALOG_H_
