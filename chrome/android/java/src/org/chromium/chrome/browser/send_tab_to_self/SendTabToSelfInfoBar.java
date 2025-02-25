// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.send_tab_to_self;

import org.chromium.base.annotations.CalledByNative;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.infobar.InfoBar;
import org.chromium.chrome.browser.infobar.InfoBarCompactLayout;

/**
 * This infobar is shown to let users know they have a shared tab from another
 * device that can be opened on this one.
 */
public class SendTabToSelfInfoBar extends InfoBar {
    public SendTabToSelfInfoBar() {
        // TODO(crbug.com/949233): Update this to the right icon
        super(R.drawable.infobar_chrome, null, null);
    }

    @Override
    protected boolean usesCompactLayout() {
        return true;
    }

    @Override
    protected void createCompactLayoutContent(InfoBarCompactLayout layout) {
        new InfoBarCompactLayout.MessageBuilder(layout)
                .withText("Tab shared")
                // TODO(crbug.com/949233): Add the link in
                // .withLink(textResId, onTapCallback)
                .buildAndInsert();
    }

    @CalledByNative
    private static SendTabToSelfInfoBar create() {
        return new SendTabToSelfInfoBar();
    }
}
