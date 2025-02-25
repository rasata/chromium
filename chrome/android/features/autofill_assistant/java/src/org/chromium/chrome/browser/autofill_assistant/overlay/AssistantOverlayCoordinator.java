// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.autofill_assistant.overlay;

import android.graphics.RectF;

import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.util.AccessibilityUtil;
import org.chromium.chrome.browser.widget.ScrimView;
import org.chromium.chrome.browser.widget.ScrimView.ScrimParams;

import java.util.List;

/**
 * Coordinator responsible for showing a full or partial overlay on top of the web page currently
 * displayed.
 */
public class AssistantOverlayCoordinator {
    private final ChromeActivity mActivity;
    private final AssistantOverlayEventFilter mEventFilter;
    private final AssistantOverlayDrawable mDrawable;
    private final ScrimView mScrim;
    private boolean mScrimEnabled;

    public AssistantOverlayCoordinator(ChromeActivity activity, AssistantOverlayModel model) {
        mActivity = activity;
        mScrim = mActivity.getScrim();
        mEventFilter = new AssistantOverlayEventFilter(
                mActivity, mActivity.getFullscreenManager(), mActivity.getCompositorViewHolder());
        mDrawable = new AssistantOverlayDrawable(mActivity, mActivity.getFullscreenManager());

        // Listen for changes in the state.
        // TODO(crbug.com/806868): Bind model to view through a ViewBinder instead.
        model.addObserver((source, propertyKey) -> {
            if (AssistantOverlayModel.STATE == propertyKey) {
                setState(model.get(AssistantOverlayModel.STATE));
            } else if (AssistantOverlayModel.TOUCHABLE_AREA == propertyKey) {
                List<RectF> area = model.get(AssistantOverlayModel.TOUCHABLE_AREA);
                mEventFilter.setTouchableArea(area);
                mDrawable.setTransparentArea(area);
            } else if (AssistantOverlayModel.DELEGATE == propertyKey) {
                AssistantOverlayDelegate delegate = model.get(AssistantOverlayModel.DELEGATE);
                mEventFilter.setDelegate(delegate);
                mDrawable.setDelegate(delegate);
            } else if (AssistantOverlayModel.WEB_CONTENTS == propertyKey) {
                mDrawable.setWebContents(model.get(AssistantOverlayModel.WEB_CONTENTS));
            }
        });
    }

    /**
     * Destroy this coordinator.
     */
    public void destroy() {
        if (mActivity.isViewObscuringAllTabs()) mActivity.removeViewObscuringAllTabs(mScrim);

        setScrimEnabled(false);
        mEventFilter.destroy();
        mDrawable.destroy();
    }

    /**
     * Set the overlay state.
     */
    private void setState(@AssistantOverlayState int state) {
        if (state == AssistantOverlayState.PARTIAL && AccessibilityUtil.isAccessibilityEnabled()) {
            // Touch exploration is fully disabled if there's an overlay in front. In this case, the
            // overlay must be fully gone and filtering elements for touch exploration must happen
            // at another level.
            //
            // TODO(crbug.com/806868): filter elements available to touch exploration, when it
            // is enabled.
            state = AssistantOverlayState.HIDDEN;
        }

        if (state == AssistantOverlayState.HIDDEN) {
            setScrimEnabled(false);
            mEventFilter.reset();
        } else {
            setScrimEnabled(true);
            mDrawable.setPartial(state == AssistantOverlayState.PARTIAL);
            mEventFilter.setPartial(state == AssistantOverlayState.PARTIAL);
        }

        if (state == AssistantOverlayState.FULL && !mActivity.isViewObscuringAllTabs()) {
            mActivity.addViewObscuringAllTabs(mScrim);
        }

        if (state != AssistantOverlayState.FULL && mActivity.isViewObscuringAllTabs()) {
            mActivity.removeViewObscuringAllTabs(mScrim);
        }
    }

    private void setScrimEnabled(boolean enabled) {
        if (enabled == mScrimEnabled) return;

        if (enabled) {
            ScrimParams params = new ScrimParams(mActivity.getCompositorViewHolder(),
                    /* showInFrontOfAnchorView= */ true,
                    /* affectsStatusBar = */ false,
                    /* topMargin= */ 0,
                    /* observer= */ null);
            params.backgroundDrawable = mDrawable;
            params.eventFilter = mEventFilter;
            mScrim.showScrim(params);
        } else {
            mScrim.hideScrim(/* fadeOut= */ true);
        }
        mScrimEnabled = enabled;
    }
}
